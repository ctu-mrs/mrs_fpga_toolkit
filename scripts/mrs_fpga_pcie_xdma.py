#!/usr/bin/env python3
"""Generic Linux PCIe inventory, tuning, and XDMA lifecycle control.

The utility deliberately uses sysfs and Python's standard library.  It does
not assume an application register map, FPGA part, board name, or XDMA channel
count.  The Xilinx PCI vendor ID and device index zero are only defaults.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import time
from typing import Any


SYSFS_PCI = Path("/sys/bus/pci")
SYSFS_XDMA = Path("/sys/class/xdma")
DEV = Path("/dev")
RUNTIME_STATE = Path("/run/mrs_fpga_pcie_xdma")
VALID_MRRS = (128, 256, 512, 1024, 2048, 4096)
BDF_PATTERN = re.compile(
    r"^(?:(?P<domain>[0-9a-fA-F]{4}):)?"
    r"(?P<bus>[0-9a-fA-F]{2}):(?P<device>[0-9a-fA-F]{2})\."
    r"(?P<function>[0-7])$"
)


class ToolError(RuntimeError):
    """An actionable user or hardware error."""


def require_root() -> None:
    if not hasattr(os, "geteuid") or os.geteuid() != 0:
        raise ToolError("this utility must be run as root")


def normalize_bdf(value: str) -> str:
    match = BDF_PATTERN.fullmatch(value.strip())
    if not match:
        raise ToolError(f"invalid PCI BDF: {value!r}")
    domain = match.group("domain") or "0000"
    return (
        f"{domain.lower()}:{match.group('bus').lower()}:"
        f"{match.group('device').lower()}.{match.group('function')}"
    )


def parse_hex_id(value: str | None, name: str) -> int | None:
    if value is None or value == "":
        return None
    try:
        number = int(value, 0)
    except ValueError as error:
        raise ToolError(f"invalid {name}: {value!r}") from error
    if not 0 <= number <= 0xFFFF:
        raise ToolError(f"{name} must fit in 16 bits")
    return number


def read_text(path: Path, default: str = "unknown") -> str:
    try:
        return path.read_text(encoding="ascii").strip()
    except (FileNotFoundError, OSError):
        return default


def read_hex_file(path: Path) -> int | None:
    value = read_text(path, "")
    try:
        return int(value, 0) if value else None
    except ValueError:
        return None


def resolved_name(path: Path) -> str | None:
    try:
        return path.resolve(strict=True).name
    except (FileNotFoundError, OSError):
        return None


def endpoint_path(bdf: str) -> Path:
    return SYSFS_PCI / "devices" / normalize_bdf(bdf)


def saved_bdf_path(device_index: int) -> Path:
    return RUNTIME_STATE / f"xdma{device_index}.bdf"


def read_saved_bdf(device_index: int) -> str | None:
    value = read_text(saved_bdf_path(device_index), "")
    if not value:
        return None
    try:
        return normalize_bdf(value)
    except ToolError:
        return None


def remember_bdf(device_index: int, bdf: str) -> None:
    """Best-effort runtime hint for recovery after the endpoint disappears."""

    try:
        RUNTIME_STATE.mkdir(mode=0o755, parents=True, exist_ok=True)
        saved_bdf_path(device_index).write_text(
            normalize_bdf(bdf) + "\n", encoding="ascii"
        )
    except OSError as error:
        print(f"warning: could not save runtime BDF hint: {error}", file=sys.stderr)


def xdma_indices_for_bdf(bdf: str) -> list[int]:
    normalized = normalize_bdf(bdf)
    indices: set[int] = set()
    for link in glob.glob(str(SYSFS_XDMA / "xdma*_*")):
        match = re.match(r"xdma(\d+)_", Path(link).name)
        if not match:
            continue
        try:
            target = (Path(link) / "device").resolve(strict=True).name
        except (FileNotFoundError, OSError):
            continue
        if target == normalized:
            indices.add(int(match.group(1)))
    return sorted(indices)


def xdma_nodes(index: int) -> list[str]:
    return sorted(str(path) for path in DEV.glob(f"xdma{index}_*"))


def driver_bound_bdfs(module: str = "xdma") -> list[str]:
    driver_dir = SYSFS_PCI / "drivers" / module
    if not driver_dir.is_dir():
        return []
    return sorted(
        path.name for path in driver_dir.iterdir() if BDF_PATTERN.fullmatch(path.name)
    )


def matching_endpoints(
    vendor_id: int | None, device_id: int | None, include_all: bool = False
) -> list[str]:
    matches: list[str] = []
    devices_dir = SYSFS_PCI / "devices"
    if not devices_dir.is_dir():
        return matches
    for path in sorted(devices_dir.iterdir()):
        if not BDF_PATTERN.fullmatch(path.name):
            continue
        vendor = read_hex_file(path / "vendor")
        device = read_hex_file(path / "device")
        if not include_all:
            if vendor_id is not None and vendor != vendor_id:
                continue
            if device_id is not None and device != device_id:
                continue
        matches.append(path.name)
    return matches


def find_current_bdf(
    *,
    explicit: str | None,
    device_index: int,
    vendor_id: int | None,
    device_id: int | None,
    module: str = "xdma",
    allow_absent_explicit: bool = False,
) -> str | None:
    if explicit:
        bdf = normalize_bdf(explicit)
        if not endpoint_path(bdf).exists() and not allow_absent_explicit:
            raise ToolError(f"PCIe endpoint does not exist: {bdf}")
        return bdf

    for suffix in ("control", "user"):
        link = SYSFS_XDMA / f"xdma{device_index}_{suffix}" / "device"
        name = resolved_name(link)
        if name and BDF_PATTERN.fullmatch(name):
            return name

    bound = driver_bound_bdfs(module)
    if len(bound) == 1:
        return bound[0]
    if len(bound) > 1:
        raise ToolError(
            f"multiple endpoints are bound to {module}; select one with --bdf"
        )

    matches = matching_endpoints(vendor_id, device_id)
    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1:
        raise ToolError(
            "multiple PCIe endpoints match the fallback IDs; use --bdf or "
            "--device-id"
        )
    return None


def detect_bdf(
    *,
    explicit: str | None,
    device_index: int,
    vendor_id: int | None,
    device_id: int | None,
    module: str = "xdma",
    allow_absent_explicit: bool = False,
) -> str:
    bdf = find_current_bdf(
        explicit=explicit,
        device_index=device_index,
        vendor_id=vendor_id,
        device_id=device_id,
        module=module,
        allow_absent_explicit=allow_absent_explicit,
    )
    if bdf is not None:
        return bdf
    raise ToolError("cannot find a matching PCIe endpoint; use --bdf explicitly")


def read_config(path: Path, size: int = 4096) -> bytes:
    fd = os.open(path, os.O_RDONLY)
    try:
        return os.pread(fd, size, 0)
    finally:
        os.close(fd)


def find_pcie_capability(config: bytes) -> int:
    if len(config) < 0x40:
        raise ToolError("PCI configuration space is truncated")
    status = struct.unpack_from("<H", config, 0x06)[0]
    if not status & 0x10:
        raise ToolError("PCI endpoint has no conventional capability list")
    pointer = config[0x34] & 0xFC
    visited: set[int] = set()
    while pointer:
        if pointer in visited or pointer + 2 > len(config):
            raise ToolError("malformed PCI capability list")
        visited.add(pointer)
        capability_id = config[pointer]
        next_pointer = config[pointer + 1] & 0xFC
        if capability_id == 0x10:
            return pointer
        pointer = next_pointer
    raise ToolError("PCI Express capability was not found")


def decode_pcie_control(config: bytes) -> dict[str, int]:
    capability = find_pcie_capability(config)
    if capability + 10 > len(config):
        raise ToolError("PCI Express capability is truncated")
    control = struct.unpack_from("<H", config, capability + 8)[0]
    return {
        "capability_offset": capability,
        "device_control": control,
        "mps_bytes": 128 << ((control >> 5) & 0x7),
        "mrrs_bytes": 128 << ((control >> 12) & 0x7),
    }


def parse_resources(path: Path) -> list[dict[str, Any]]:
    resources: list[dict[str, Any]] = []
    text = read_text(path / "resource", "")
    for index, line in enumerate(text.splitlines()):
        fields = line.split()
        if len(fields) < 3:
            continue
        start, end, flags = (int(field, 16) for field in fields[:3])
        size = end - start + 1 if start and end >= start else 0
        if size:
            resources.append(
                {
                    "bar": index,
                    "start": start,
                    "end": end,
                    "size": size,
                    "flags": flags,
                }
            )
    return resources


def endpoint_details(bdf: str) -> dict[str, Any]:
    bdf = normalize_bdf(bdf)
    path = endpoint_path(bdf)
    if not path.is_dir():
        raise ToolError(f"PCIe endpoint does not exist: {bdf}")

    details: dict[str, Any] = {
        "bdf": bdf,
        "vendor": read_text(path / "vendor"),
        "device": read_text(path / "device"),
        "subsystem_vendor": read_text(path / "subsystem_vendor"),
        "subsystem_device": read_text(path / "subsystem_device"),
        "class": read_text(path / "class"),
        "revision": read_text(path / "revision"),
        "driver": resolved_name(path / "driver"),
        "driver_module": resolved_name(path / "driver" / "module"),
        "iommu_group": resolved_name(path / "iommu_group"),
        "numa_node": read_text(path / "numa_node"),
        "current_link_speed": read_text(path / "current_link_speed"),
        "current_link_width": read_text(path / "current_link_width"),
        "max_link_speed": read_text(path / "max_link_speed"),
        "max_link_width": read_text(path / "max_link_width"),
        "resources": parse_resources(path),
    }
    indices = xdma_indices_for_bdf(bdf)
    details["xdma_indices"] = indices
    details["xdma_nodes"] = {str(index): xdma_nodes(index) for index in indices}
    try:
        details["pcie"] = decode_pcie_control(read_config(path / "config"))
    except (OSError, ToolError) as error:
        details["pcie_error"] = str(error)
    return details


def format_size(value: int) -> str:
    units = ("B", "KiB", "MiB", "GiB")
    number = float(value)
    for unit in units:
        if number < 1024 or unit == units[-1]:
            return (
                f"{number:.0f} {unit}"
                if number.is_integer()
                else f"{number:.1f} {unit}"
            )
        number /= 1024
    return f"{value} B"


def print_endpoint(details: dict[str, Any]) -> None:
    print(
        f"{details['bdf']} vendor={details['vendor']} device={details['device']} "
        f"class={details['class']} revision={details['revision']}"
    )
    print(
        "  subsystem: "
        f"{details['subsystem_vendor']}:{details['subsystem_device']}  "
        f"driver: {details['driver'] or 'unbound'}  "
        f"module: {details['driver_module'] or 'none'}"
    )
    print(
        "  link: "
        f"{details['current_link_speed']} x{details['current_link_width']} "
        f"(max {details['max_link_speed']} x{details['max_link_width']})"
    )
    print(
        f"  NUMA: {details['numa_node']}  "
        f"IOMMU group: {details['iommu_group'] or 'none'}"
    )
    pcie = details.get("pcie")
    if pcie:
        print(
            f"  PCIe control: MPS={pcie['mps_bytes']} B "
            f"MRRS={pcie['mrrs_bytes']} B "
            f"DevCtl=0x{pcie['device_control']:04x}"
        )
    else:
        print(f"  PCIe control: unavailable ({details.get('pcie_error')})")
    for resource in details["resources"]:
        print(
            f"  BAR{resource['bar']}: "
            f"0x{resource['start']:x}-0x{resource['end']:x} "
            f"({format_size(resource['size'])}) flags=0x{resource['flags']:x}"
        )
    if details["xdma_indices"]:
        for index in details["xdma_indices"]:
            nodes = details["xdma_nodes"][str(index)]
            print(f"  XDMA index {index}: {', '.join(nodes) if nodes else 'no nodes'}")
    else:
        print("  XDMA: no class devices mapped")


def wait_until(
    predicate, timeout: float, description: str, interval: float = 0.2
) -> None:
    deadline = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() >= deadline:
            raise ToolError(f"timeout waiting for {description}")
        time.sleep(interval)


def write_one(path: Path) -> None:
    path.write_text("1\n", encoding="ascii")


def run_checked(command: list[str]) -> None:
    try:
        subprocess.run(command, check=True)
    except FileNotFoundError as error:
        raise ToolError(f"required command was not found: {command[0]}") from error
    except subprocess.CalledProcessError as error:
        raise ToolError(
            f"command failed with status {error.returncode}: {shlex.join(command)}"
        ) from error


def parse_required_nodes(value: str) -> list[str]:
    return [item for item in re.split(r"[\s,]+", value.strip()) if item]


def unload_module(module: str) -> None:
    if (Path("/sys/module") / module).is_dir():
        print(f"unloading {module}")
        run_checked(["modprobe", "-r", module])


def load_module(module: str, module_args: list[str]) -> None:
    print(f"loading {module}")
    run_checked(["modprobe", module, *module_args])


def finish_xdma_binding(
    *,
    bdf: str,
    device_index: int,
    module: str,
    required_nodes: list[str],
    timeout: float,
    device_mode: int | None,
) -> None:
    path = endpoint_path(bdf)
    wait_until(lambda: (path / "driver").exists(), timeout, f"driver binding at {bdf}")
    driver = resolved_name(path / "driver")
    if driver != module:
        raise ToolError(f"endpoint bound to {driver!r}, expected {module!r}")

    for suffix in required_nodes:
        node = DEV / f"xdma{device_index}_{suffix}"
        wait_until(node.exists, timeout, str(node))

    if device_mode is not None:
        for node in DEV.glob(f"xdma{device_index}_*"):
            os.chmod(node, device_mode)

    remember_bdf(device_index, bdf)
    details = endpoint_details(bdf)
    print(
        f"ready: {bdf}, link {details['current_link_speed']} "
        f"x{details['current_link_width']}"
    )
    nodes = xdma_nodes(device_index)
    print("nodes: " + (", ".join(nodes) if nodes else "none"))


def reload_driver_without_endpoint(
    *,
    bdf: str | None,
    device_index: int,
    module: str,
    module_args: list[str],
    required_nodes: list[str],
    timeout: float,
    device_mode: int | None,
) -> str | None:
    """Reload the module even when no matching PCI function is visible."""

    print("mode:         driver-only")
    print(f"endpoint:     {bdf or 'not currently visible'}")
    print(f"XDMA index:   {device_index}")
    print(f"module:       {module}")
    unload_module(module)
    load_module(module, module_args)
    if bdf is None or not endpoint_path(bdf).exists():
        print(
            "XDMA module reloaded, but no matching PCIe endpoint is visible. "
            "Use --probe or --reload to rescan PCIe."
        )
        return None
    finish_xdma_binding(
        bdf=bdf,
        device_index=device_index,
        module=module,
        required_nodes=required_nodes,
        timeout=timeout,
        device_mode=device_mode,
    )
    return bdf


def recover_missing_endpoint(
    *,
    preferred_bdf: str | None,
    strict_bdf: bool,
    device_index: int,
    vendor_id: int | None,
    device_id: int | None,
    module: str,
    module_args: list[str],
    required_nodes: list[str],
    timeout: float,
    device_mode: int | None,
    unload_first: bool,
) -> str:
    """Rescan first, then discover and bind an endpoint that was absent."""

    preferred = normalize_bdf(preferred_bdf) if preferred_bdf else None
    print("mode:         recover-missing-endpoint")
    print(f"endpoint:     {preferred or 'auto-discover after rescan'}")
    print(f"XDMA index:   {device_index}")
    print(f"module:       {module}")
    if unload_first:
        unload_module(module)

    rescan = SYSFS_PCI / "rescan"
    deadline = time.monotonic() + timeout
    discovered: str | None = None
    print("rescanning PCIe while waiting for the FPGA endpoint")
    while discovered is None:
        write_one(rescan)
        if preferred and endpoint_path(preferred).exists():
            discovered = preferred
        elif not strict_bdf:
            discovered = find_current_bdf(
                explicit=None,
                device_index=device_index,
                vendor_id=vendor_id,
                device_id=device_id,
                module=module,
            )
        if discovered is not None:
            break
        if time.monotonic() >= deadline:
            target = preferred or "a matching endpoint"
            raise ToolError(
                f"timeout waiting for {target} after repeated PCIe rescans; "
                "the FPGA PCIe image may not be running, the link may not be "
                "trained, or a physical-JTAG reload may be required"
            )
        time.sleep(0.5)

    print(f"discovered endpoint: {discovered}")
    load_module(module, module_args)
    finish_xdma_binding(
        bdf=discovered,
        device_index=device_index,
        module=module,
        required_nodes=required_nodes,
        timeout=timeout,
        device_mode=device_mode,
    )
    return discovered


def reload_xdma(
    *,
    mode: str,
    bdf: str,
    device_index: int,
    module: str,
    module_args: list[str],
    required_nodes: list[str],
    timeout: float,
    device_mode: int | None,
) -> None:
    path = endpoint_path(bdf)
    post_reconfigure = mode == "post-reconfigure"
    if not path.exists() and not post_reconfigure:
        raise ToolError(f"PCIe endpoint does not exist: {bdf}")

    print(f"mode:         {mode}")
    print(f"endpoint:     {bdf}")
    print(f"XDMA index:   {device_index}")
    print(f"module:       {module}")
    remember_bdf(device_index, bdf)

    if mode in {"reenumerate", "post-reconfigure", "driver-only", "remove"}:
        unload_module(module)

    if mode in {"reenumerate", "post-reconfigure", "remove"}:
        remove = path / "remove"
        if remove.exists():
            print(f"removing endpoint {bdf}")
            write_one(remove)
            wait_until(lambda: not path.exists(), timeout, f"removal of {bdf}")
        elif post_reconfigure:
            print("endpoint is already absent after FPGA reconfiguration")
        else:
            raise ToolError(f"endpoint cannot be removed: {bdf}")

    if mode == "remove":
        print("endpoint removed; program the FPGA before rescanning")
        return

    if mode in {"reenumerate", "post-reconfigure", "probe"}:
        print("rescanning PCIe")
        rescan = SYSFS_PCI / "rescan"
        write_one(rescan)
        deadline = time.monotonic() + timeout
        while not path.exists():
            if time.monotonic() >= deadline:
                raise ToolError(f"timeout waiting for {bdf} after PCIe rescan")
            time.sleep(0.5)
            write_one(rescan)

    load_module(module, module_args)
    finish_xdma_binding(
        bdf=bdf,
        device_index=device_index,
        module=module,
        required_nodes=required_nodes,
        timeout=timeout,
        device_mode=device_mode,
    )


def set_mrrs(bdf: str, requested: int) -> tuple[int, int, int]:
    if requested not in VALID_MRRS:
        raise ToolError(
            f"MRRS must be one of: {', '.join(str(value) for value in VALID_MRRS)}"
        )
    path = endpoint_path(bdf) / "config"
    config = read_config(path)
    pcie = decode_pcie_control(config)
    offset = pcie["capability_offset"] + 8
    before = pcie["device_control"]
    encoded = VALID_MRRS.index(requested)
    after = (before & ~0x7000) | (encoded << 12)

    fd = os.open(path, os.O_RDWR)
    try:
        written = os.pwrite(fd, struct.pack("<H", after), offset)
        if written != 2:
            raise ToolError(f"short PCI config write at offset 0x{offset:x}")
        verified_data = os.pread(fd, 2, offset)
    finally:
        os.close(fd)
    if len(verified_data) != 2:
        raise ToolError("short PCI config readback")
    verified = struct.unpack("<H", verified_data)[0]
    if verified & 0x7000 != after & 0x7000:
        raise ToolError(
            f"MRRS readback failed: wrote 0x{after:04x}, read 0x{verified:04x}"
        )
    return before, verified, 128 << ((verified >> 12) & 0x7)


def parse_mode(value: str) -> int | None:
    lowered = value.lower()
    if lowered in {"preserve", "none"}:
        return None
    try:
        mode = int(value, 8)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "device mode must be octal or 'preserve'"
        ) from error
    if not 0 <= mode <= 0o777:
        raise argparse.ArgumentTypeError("device mode must be between 000 and 777")
    return mode


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generic PCIe inventory/tuning and XDMA lifecycle control"
    )
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument("--list", action="store_true", help="list matching PCIe cards")
    actions.add_argument(
        "--status", action="store_true", help="show one endpoint and XDMA"
    )
    actions.add_argument(
        "--set-mrrs",
        type=int,
        metavar="BYTES",
        help="set PCIe Max Read Request Size",
    )
    actions.add_argument(
        "--reload",
        action="store_true",
        help=(
            "recover XDMA; remove/re-enumerate a visible endpoint or repeatedly "
            "rescan when it is missing"
        ),
    )
    actions.add_argument(
        "--post-reconfigure",
        action="store_true",
        help="recover PCIe/XDMA after whole-FPGA programming",
    )
    actions.add_argument(
        "--driver-only",
        action="store_true",
        help="reload the XDMA module even if no endpoint is currently visible",
    )
    actions.add_argument(
        "--probe",
        action="store_true",
        help="rescan and bind XDMA without removing an endpoint",
    )
    actions.add_argument(
        "--remove",
        action="store_true",
        help="unload XDMA and remove the endpoint before FPGA programming",
    )
    parser.add_argument("--bdf", help="PCI BDF, for example 0000:03:00.0")
    parser.add_argument(
        "--device-index", type=int, default=0, help="XDMA device index (default: 0)"
    )
    parser.add_argument(
        "--vendor-id", default="0x10ee", help="fallback PCI vendor ID (default: 0x10ee)"
    )
    parser.add_argument("--device-id", help="optional fallback PCI device ID")
    parser.add_argument(
        "--all", action="store_true", help="with --list, include all vendors"
    )
    parser.add_argument("--module", default="xdma", help="kernel module name")
    parser.add_argument(
        "--module-args",
        default="",
        help="arguments passed to modprobe, parsed without a shell",
    )
    parser.add_argument(
        "--required-nodes",
        default="control",
        help="comma/space-separated XDMA suffixes to await; empty disables",
    )
    parser.add_argument(
        "--wait", type=float, default=30.0, help="recovery timeout in seconds"
    )
    parser.add_argument(
        "--device-mode",
        type=parse_mode,
        default=0o666,
        help="mode for /dev/xdmaN_* after reload (default: 666; 'preserve' disables)",
    )
    parser.add_argument("--json", action="store_true", help="emit JSON for list/status")
    return parser.parse_args()


def main() -> int:
    require_root()
    args = parse_args()
    if args.device_index < 0:
        raise ToolError("--device-index must be nonnegative")
    if args.wait <= 0:
        raise ToolError("--wait must be positive")
    vendor_id = parse_hex_id(args.vendor_id, "vendor ID")
    device_id = parse_hex_id(args.device_id, "device ID")

    if args.list:
        bdfs = matching_endpoints(vendor_id, device_id, include_all=args.all)
        records = [endpoint_details(bdf) for bdf in bdfs]
        if args.json:
            print(json.dumps(records, indent=2, sort_keys=True))
        elif records:
            for index, record in enumerate(records):
                if index:
                    print()
                print_endpoint(record)
        else:
            print("no matching PCIe endpoints")
        return 0

    module_args = shlex.split(args.module_args)
    required_nodes = parse_required_nodes(args.required_nodes)

    if args.status:
        current = find_current_bdf(
            explicit=args.bdf,
            device_index=args.device_index,
            vendor_id=vendor_id,
            device_id=device_id,
            module=args.module,
            allow_absent_explicit=bool(args.bdf),
        )
        visible = current is not None and endpoint_path(current).exists()
        if visible:
            remember_bdf(args.device_index, current)
        status = {
            "endpoint": endpoint_details(current) if visible else None,
            "endpoint_visible": visible,
            "requested_bdf": current,
            "saved_bdf": read_saved_bdf(args.device_index),
            "module_loaded": (Path("/sys/module") / args.module).is_dir(),
            "requested_xdma_index": args.device_index,
            "requested_xdma_nodes": xdma_nodes(args.device_index),
        }
        if args.json:
            print(json.dumps(status, indent=2, sort_keys=True))
        elif visible:
            print_endpoint(status["endpoint"])
            print(
                f"  module {args.module}: "
                f"{'loaded' if status['module_loaded'] else 'not loaded'}"
            )
        else:
            print("PCIe endpoint: not currently visible")
            print(f"saved BDF hint: {status['saved_bdf'] or 'none'}")
            print(
                f"module {args.module}: "
                f"{'loaded' if status['module_loaded'] else 'not loaded'}"
            )
            nodes = status["requested_xdma_nodes"]
            print("XDMA nodes: " + (", ".join(nodes) if nodes else "none"))
            print("recovery: run --probe or --reload")
        return 0

    if args.set_mrrs is not None or args.remove:
        bdf = detect_bdf(
            explicit=args.bdf,
            device_index=args.device_index,
            vendor_id=vendor_id,
            device_id=device_id,
            module=args.module,
        )
        remember_bdf(args.device_index, bdf)

    if args.set_mrrs is not None:
        before, after, actual = set_mrrs(bdf, args.set_mrrs)
        print(
            f"PCIe MRRS updated: BDF={bdf} bytes={actual} "
            f"DeviceControl=0x{before:04x}->0x{after:04x}"
        )
        if actual > 1024:
            print(
                "warning: large MRRS values can expose root-complex or endpoint "
                "limitations; validate DMA under load"
            )
        return 0

    if args.remove:
        reload_xdma(
            mode="remove",
            bdf=bdf,
            device_index=args.device_index,
            module=args.module,
            module_args=module_args,
            required_nodes=[],
            timeout=args.wait,
            device_mode=args.device_mode,
        )
        return 0

    current = find_current_bdf(
        explicit=args.bdf,
        device_index=args.device_index,
        vendor_id=vendor_id,
        device_id=device_id,
        module=args.module,
        allow_absent_explicit=bool(args.bdf),
    )
    if current is None:
        current = read_saved_bdf(args.device_index)

    if args.driver_only:
        reload_driver_without_endpoint(
            bdf=current,
            device_index=args.device_index,
            module=args.module,
            module_args=module_args,
            required_nodes=required_nodes,
            timeout=args.wait,
            device_mode=args.device_mode,
        )
        return 0

    if current is not None and endpoint_path(current).exists():
        if args.post_reconfigure:
            mode = "post-reconfigure"
        elif args.probe:
            mode = "probe"
        else:
            mode = "reenumerate"
        reload_xdma(
            mode=mode,
            bdf=current,
            device_index=args.device_index,
            module=args.module,
            module_args=module_args,
            required_nodes=required_nodes,
            timeout=args.wait,
            device_mode=args.device_mode,
        )
        return 0

    recover_missing_endpoint(
        preferred_bdf=current,
        strict_bdf=bool(args.bdf),
        device_index=args.device_index,
        vendor_id=vendor_id,
        device_id=device_id,
        module=args.module,
        module_args=module_args,
        required_nodes=required_nodes,
        timeout=args.wait,
        device_mode=args.device_mode,
        unload_first=not args.probe,
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ToolError, OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
