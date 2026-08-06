#!/usr/bin/env python3
"""Program FPGA SRAM or SPI configuration flash through physical USB-JTAG.

The default operation remains volatile SRAM programming.  ``--flash --yes``
loads an SPI-over-JTAG bridge, detects the attached board flash, persistently
writes and verifies it, then lets the FPGA reload from flash.  PCIe is not
required.  The optional ``--reload`` mode removes a currently visible endpoint
before either operation and recovers PCIe/XDMA afterward with the companion
mrs_fpga_pcie_xdma.py implementation.
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
import tempfile

import mrs_fpga_pcie_xdma as pcie


class JtagError(RuntimeError):
    """An actionable JTAG programming error."""


IDCODE_PATTERN = re.compile(r"\bidcode\s+(0x[0-9a-fA-F]+)\b", re.IGNORECASE)
XADC_TEMPERATURE_PATTERN = re.compile(
    r'"temp"\s*:\s*(-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)'
)
FLASH_DETAIL_PATTERN = re.compile(
    r"Jedec ID\s*:\s*([0-9a-fA-F]{2}).*?"
    r"memory type\s*:\s*([0-9a-fA-F]{2}).*?"
    r"memory capacity\s*:\s*([0-9a-fA-F]{2})",
    re.IGNORECASE | re.DOTALL,
)

SUPPORTED_FLASHES = {
    bytes.fromhex("01 02 19"): (
        "NiteFury",
        "Spansion/Cypress S25FL256S",
        32 * 1024 * 1024,
    ),
    bytes.fromhex("20 ba 20"): (
        "Aller",
        "Micron MT25QL512",
        64 * 1024 * 1024,
    ),
}


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


def parse_xadc_temperature(output: str) -> float | None:
    match = XADC_TEMPERATURE_PATTERN.search(output)
    return float(match.group(1)) if match else None


def report_xadc_temperature(args: argparse.Namespace) -> float | None:
    """Read XADC through JTAG, but do not block programming if unavailable."""
    command = [*loader_base_command(args), "--read_xadc"]
    try:
        completed = subprocess.run(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
        )
    except FileNotFoundError as error:
        raise JtagError(f"required command was not found: {args.loader}") from error

    output = completed.stdout.replace(b"\x00", b"").decode("utf-8", errors="replace")
    temperature = parse_xadc_temperature(output)
    if completed.returncode or temperature is None:
        detail = f"loader status {completed.returncode}"
        if completed.returncode == 0:
            detail = "temperature was absent from loader output"
        print(f"WARNING: XADC temperature unavailable ({detail})", file=sys.stderr)
        return None
    print(f"XADC temperature: {temperature:.3f} degC")
    return temperature


def parse_flash_jedec(output: str) -> bytes | None:
    """Parse both known-model and generic-detail openFPGALoader output."""
    lowered = output.lower()
    if "s25fl256s" in lowered:
        return bytes.fromhex("01 02 19")
    if "mt25ql512" in lowered or "n25q512" in lowered:
        return bytes.fromhex("20 ba 20")
    match = FLASH_DETAIL_PATTERN.search(output)
    if match:
        return bytes(int(value, 16) for value in match.groups())
    return None


def detect_flash_over_jtag(
    args: argparse.Namespace,
) -> tuple[bytes, str, str, int]:
    """Load the bridge and read one byte so JEDEC is known before any erase."""
    with tempfile.TemporaryDirectory(prefix="mrs-fpga-jtag-flash-") as directory:
        probe = Path(directory, "probe.bin")
        command = [
            *loader_base_command(args),
            "--fpga-part",
            args.fpga_part,
            "--offset",
            "0",
            "--dump-flash",
            "--file-size",
            "1",
            str(probe),
        ]
        completed = subprocess.run(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
        )

    output = completed.stdout.replace(b"\x00", b"").decode("utf-8", errors="replace")
    print(output, end="" if output.endswith("\n") else "\n")
    if completed.returncode:
        raise JtagError(
            "read-only SPI-over-JTAG flash detection failed with status "
            f"{completed.returncode}"
        )

    jedec = parse_flash_jedec(output)
    if jedec is None:
        raise JtagError("SPI-over-JTAG probe did not report a usable JEDEC ID")
    profile = SUPPORTED_FLASHES.get(jedec)
    if profile is None:
        supported = ", ".join(
            f"{key.hex(' ')} ({value[1]})" for key, value in SUPPORTED_FLASHES.items()
        )
        raise JtagError(
            f"unsupported board flash JEDEC ID {jedec.hex(' ')}; "
            f"supported parts: {supported}"
        )
    board, model, capacity = profile
    print(f"flash JEDEC ID:   {jedec.hex(' ')}")
    print(f"flash:            {model}")
    print(f"board profile:    {board}")
    print(f"flash capacity:   {capacity // (1024 * 1024)} MiB")
    return jedec, board, model, capacity


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
            "Program FPGA SRAM or supported board SPI flash through physical "
            "USB-JTAG and report XADC temperature"
        ),
        epilog=(
            "SRAM: IMAGE.bit. Persistent flash: IMAGE.bit --flash --yes. "
            "Supported flash profiles are NiteFury S25FL256S and Aller "
            "MT25QL512; writes are verified."
        ),
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
        "--flash",
        action="store_true",
        help="detect, write, and verify the board SPI flash through JTAG",
    )
    parser.add_argument(
        "--yes",
        action="store_true",
        help="confirm the persistent erase/program operation required by --flash",
    )
    parser.add_argument(
        "--offset",
        type=integer,
        default=0,
        help="flash byte offset (default: 0)",
    )
    parser.add_argument(
        "--fpga-part",
        default="xc7a200tfbg484",
        help=(
            "FPGA model/package selecting the SPI-over-JTAG bridge "
            "(default: xc7a200tfbg484 for Aller and NiteFury)"
        ),
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
        "--reload",
        action="store_true",
        help="cleanly remove a visible PCIe endpoint and recover PCIe/XDMA afterward",
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
    args = parse_args()
    require_root()
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
    if args.offset < 0:
        raise JtagError("--offset must be nonnegative")
    if not args.fpga_part:
        raise JtagError("--fpga-part must not be empty")
    if shutil.which(args.loader) is None:
        raise JtagError(f"required command was not found: {args.loader}")
    if not args.detect and args.image is None:
        raise JtagError("provide IMAGE or use --detect")
    if args.detect and args.image is not None:
        raise JtagError("--detect does not accept IMAGE")
    if args.flash and args.image is None:
        raise JtagError("--flash requires IMAGE")
    if args.flash and not args.yes:
        raise JtagError("refusing to erase/program persistent flash without --yes")
    if args.yes and not args.flash:
        raise JtagError("--yes is valid only with --flash")
    if args.offset and not args.flash:
        raise JtagError("--offset is valid only with --flash")

    idcode, _ = run_detection(args)
    validate_idcode(idcode, args.expected_idcode, args.idcode_mask)
    report_xadc_temperature(args)
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
    if args.flash:
        print(f"flash offset:     0x{args.offset:x}")
        print(f"FPGA/package:     {args.fpga_part}")
        print("persistent QSPI: detect, erase/program, and verify over JTAG")
    else:
        print("persistent QSPI: unchanged")

    selected_bdf: str | None = None
    vendor_id: int | None = None
    device_id: int | None = None
    required_nodes: list[str] = []
    module_args: list[str] = []

    if args.reload:
        vendor_id = pcie.parse_hex_id(args.vendor_id, "vendor ID")
        device_id = pcie.parse_hex_id(args.device_id, "device ID")
        required_nodes = pcie.parse_required_nodes(args.required_nodes)
        module_args = shlex.split(args.module_args)
        selected_bdf = pcie.find_current_bdf(
            explicit=args.bdf,
            device_index=args.device_index,
            vendor_id=vendor_id,
            device_id=device_id,
            module=args.module,
            allow_absent_explicit=bool(args.bdf),
        )
        if selected_bdf is None:
            selected_bdf = pcie.read_saved_bdf(args.device_index)
        if selected_bdf is not None:
            print(f"PCI endpoint:    {selected_bdf}")
            path = pcie.endpoint_path(selected_bdf)
            if path.exists():
                print(
                    "removing the live PCI endpoint before whole-device configuration"
                )
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
                print("PCI endpoint is absent; recovery will rescan after programming")
        else:
            print("PCI endpoint:    not visible; recovery will auto-discover it")

    if args.flash:
        _, _, _, capacity = detect_flash_over_jtag(args)
        image_end = args.offset + args.image.stat().st_size
        if image_end > capacity:
            raise JtagError(
                f"image end 0x{image_end:x} exceeds detected flash capacity "
                f"0x{capacity:x}"
            )

    command = [*loader_base_command(args), "--file-type", image_type]
    if args.flash:
        command.extend(
            [
                "--fpga-part",
                args.fpga_part,
                "--offset",
                str(args.offset),
                "--write-flash",
                "--verify",
                str(args.image),
            ]
        )
        print("programming detected board SPI flash through physical USB-JTAG")
    else:
        command.extend(["--write-sram", str(args.image)])
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

    if args.reload:
        assert vendor_id is not None
        recovered_bdf = pcie.recover_missing_endpoint(
            preferred_bdf=selected_bdf,
            strict_bdf=bool(args.bdf),
            device_index=args.device_index,
            vendor_id=vendor_id,
            device_id=device_id,
            module=args.module,
            module_args=module_args,
            required_nodes=required_nodes,
            timeout=args.wait,
            device_mode=args.device_mode,
            unload_first=True,
        )
        pcie.remember_bdf(args.device_index, recovered_bdf)
        target = "SPI flash" if args.flash else "FPGA"
        print(f"{target} programmed and PCIe/XDMA recovered successfully")
    else:
        target = "SPI flash" if args.flash else "FPGA"
        print(f"{target} programmed successfully through USB-JTAG")
        print("PCIe/XDMA was not inspected; add --reload to recover it automatically")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (JtagError, pcie.ToolError, OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
