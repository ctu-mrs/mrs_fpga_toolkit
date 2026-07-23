#!/usr/bin/env python3
"""Program volatile FPGA SRAM through a physical USB-JTAG adapter.

Persistent flash is never modified.  When a PCIe endpoint is selected, it is
removed before whole-device configuration and recovered afterward with the
companion mrs_fpga_pcie_xdma.py implementation.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

import mrs_fpga_pcie_xdma as pcie


class JtagError(RuntimeError):
    """An actionable JTAG programming error."""


IDCODE_PATTERN = re.compile(r"\bidcode\s+(0x[0-9a-fA-F]+)\b", re.IGNORECASE)


def require_root() -> None:
    if not hasattr(os, "geteuid") or os.geteuid() != 0:
        raise JtagError("this utility must be run as root")


def integer(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid integer: {value!r}") from error


def existing_file(value: str) -> Path:
    path = Path(value).expanduser()
    if not path.is_file():
        raise argparse.ArgumentTypeError(f"file not found: {path}")
    return path


def loader_base_command(args: argparse.Namespace) -> list[str]:
    command = [
        args.loader,
        "--cable",
        args.cable,
        "--freq",
        str(args.frequency),
    ]
    if args.serial:
        command.extend(["--ftdi-serial", args.serial])
    for value in args.loader_arg:
        command.append(value)
    return command


def run_detection(args: argparse.Namespace) -> tuple[int, str]:
    command = [*loader_base_command(args), "--detect"]
    try:
        completed = subprocess.run(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
        )
    except FileNotFoundError as error:
        raise JtagError(f"required command was not found: {args.loader}") from error
    output = completed.stdout.replace(b"\x00", b"").decode("utf-8", errors="replace")
    print(output, end="" if output.endswith("\n") else "\n")
    if completed.returncode:
        raise JtagError(
            f"physical USB-JTAG detection failed with status {completed.returncode}"
        )
    match = IDCODE_PATTERN.search(output)
    if not match:
        raise JtagError("JTAG detection did not report an IDCODE")
    return int(match.group(1), 16), output


def validate_idcode(idcode: int, expected: int | None, mask: int) -> None:
    print(f"detected IDCODE: 0x{idcode:08x}")
    if expected is None:
        print("IDCODE check:    disabled")
        return
    if idcode & mask != expected & mask:
        raise JtagError(
            f"detected IDCODE 0x{idcode:08x} does not match expected "
            f"0x{expected:08x} with mask 0x{mask:08x}"
        )
    print(f"IDCODE check:    passed (mask 0x{mask:08x})")


def infer_image_type(path: Path, override: str | None) -> str:
    if override:
        return override
    suffix = path.suffix.lower()
    if suffix == ".bit":
        return "bit"
    if suffix == ".bin":
        return "bin"
    raise JtagError(
        f"cannot infer image type from {path.name!r}; use --file-type bit or bin"
    )


def parse_expected_idcode(value: str) -> int | None:
    if value.lower() in {"none", "off", "disabled"}:
        return None
    return integer(value)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Program volatile FPGA SRAM through physical USB-JTAG; persistent "
            "flash is unchanged"
        )
    )
    parser.add_argument(
        "image", nargs="?", type=existing_file, help=".bit or .bin image"
    )
    parser.add_argument(
        "--detect", action="store_true", help="detect and validate the JTAG target only"
    )
    parser.add_argument(
        "--loader", default="openFPGALoader", help="JTAG loader executable"
    )
    parser.add_argument("--cable", default="digilent_hs2", help="loader cable profile")
    parser.add_argument(
        "--frequency",
        type=int,
        default=15_000_000,
        help="JTAG clock in Hz (default: 15000000)",
    )
    parser.add_argument("--serial", help="optional adapter serial number")
    parser.add_argument(
        "--loader-arg",
        action="append",
        default=[],
        help="extra loader argument; repeat for multiple arguments",
    )
    parser.add_argument(
        "--file-type", choices=("bit", "bin"), help="override image type"
    )
    parser.add_argument(
        "--expected-idcode",
        type=parse_expected_idcode,
        default=0x03636093,
        help="expected target IDCODE, or 'none' (default: 0x03636093)",
    )
    parser.add_argument(
        "--idcode-mask",
        type=integer,
        default=0x0FFFFFFF,
        help="IDCODE comparison mask (default: 0x0fffffff)",
    )
    parser.add_argument(
        "--no-pcie-recovery",
        action="store_true",
        help="do not remove/re-enumerate a PCIe endpoint",
    )
    parser.add_argument("--bdf", help="PCI BDF to save and recover")
    parser.add_argument(
        "--device-index", type=int, default=0, help="XDMA device index (default: 0)"
    )
    parser.add_argument("--vendor-id", default="0x10ee", help="fallback PCI vendor ID")
    parser.add_argument("--device-id", help="optional fallback PCI device ID")
    parser.add_argument("--module", default="xdma", help="XDMA kernel module")
    parser.add_argument(
        "--module-args",
        default="",
        help="arguments passed to modprobe after programming",
    )
    parser.add_argument(
        "--required-nodes",
        default="control",
        help="comma/space-separated XDMA node suffixes required after programming",
    )
    parser.add_argument(
        "--wait", type=float, default=30.0, help="PCIe/XDMA recovery timeout"
    )
    parser.add_argument(
        "--device-mode",
        type=pcie.parse_mode,
        default=0o666,
        help="mode for recovered /dev/xdmaN_* nodes (default: 666)",
    )
    return parser.parse_args()


def main() -> int:
    require_root()
    args = parse_args()
    if args.frequency <= 0:
        raise JtagError("--frequency must be positive")
    if not 0 <= args.idcode_mask <= 0xFFFFFFFF:
        raise JtagError("--idcode-mask must fit in 32 bits")
    if args.expected_idcode is not None and not 0 <= args.expected_idcode <= 0xFFFFFFFF:
        raise JtagError("--expected-idcode must fit in 32 bits")
    if args.device_index < 0:
        raise JtagError("--device-index must be nonnegative")
    if args.wait <= 0:
        raise JtagError("--wait must be positive")
    if shutil.which(args.loader) is None:
        raise JtagError(f"required command was not found: {args.loader}")
    if not args.detect and args.image is None:
        raise JtagError("provide IMAGE or use --detect")
    if args.detect and args.image is not None:
        raise JtagError("--detect does not accept IMAGE")

    idcode, _ = run_detection(args)
    validate_idcode(idcode, args.expected_idcode, args.idcode_mask)
    if args.detect:
        print("physical USB-JTAG detection passed")
        return 0

    assert args.image is not None
    image_type = infer_image_type(args.image, args.file_type)
    image_hash = hashlib.sha256(args.image.read_bytes()).hexdigest()
    print(f"image:           {args.image.resolve()}")
    print(f"image type:      {image_type}")
    print(f"image bytes:     {args.image.stat().st_size}")
    print(f"image SHA256:    {image_hash}")
    print(f"JTAG cable:      {args.cable}")
    print(f"JTAG frequency:  {args.frequency} Hz")
    if args.serial:
        print(f"JTAG serial:     {args.serial}")
    print("persistent QSPI: unchanged")

    selected_bdf: str | None = None
    vendor_id = pcie.parse_hex_id(args.vendor_id, "vendor ID")
    device_id = pcie.parse_hex_id(args.device_id, "device ID")
    required_nodes = pcie.parse_required_nodes(args.required_nodes)
    module_args = shlex.split(args.module_args)

    if not args.no_pcie_recovery:
        selected_bdf = pcie.detect_bdf(
            explicit=args.bdf,
            device_index=args.device_index,
            vendor_id=vendor_id,
            device_id=device_id,
            module=args.module,
            allow_absent_explicit=bool(args.bdf),
        )
        print(f"PCI endpoint:    {selected_bdf}")
        path = pcie.endpoint_path(selected_bdf)
        if path.exists():
            print("removing the live PCI endpoint before whole-device configuration")
            pcie.reload_xdma(
                mode="remove",
                bdf=selected_bdf,
                device_index=args.device_index,
                module=args.module,
                module_args=module_args,
                required_nodes=[],
                timeout=args.wait,
                device_mode=args.device_mode,
            )
        else:
            print("PCI endpoint is already absent; using the saved BDF for recovery")

    command = [
        *loader_base_command(args),
        "--file-type",
        image_type,
        "--write-sram",
        str(args.image),
    ]
    print("programming volatile FPGA SRAM through physical USB-JTAG")
    try:
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError as error:
        if selected_bdf:
            print(
                f"PCI endpoint {selected_bdf} was removed before programming. "
                "After restoring a valid FPGA image, recover it with "
                "mrs_fpga_pcie_xdma.py --post-reconfigure "
                f"--bdf {selected_bdf}",
                file=sys.stderr,
            )
        raise JtagError(
            f"JTAG programming failed with status {error.returncode}"
        ) from error

    if selected_bdf:
        pcie.reload_xdma(
            mode="post-reconfigure",
            bdf=selected_bdf,
            device_index=args.device_index,
            module=args.module,
            module_args=module_args,
            required_nodes=required_nodes,
            timeout=args.wait,
            device_mode=args.device_mode,
        )
        print("FPGA programmed and PCIe/XDMA recovered successfully")
    else:
        print("FPGA programmed successfully; PCIe recovery was disabled")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (JtagError, pcie.ToolError, OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
