#!/usr/bin/env python3
"""Program the board boot flash through AXI Quad SPI and the XDMA user BAR.

This utility intentionally uses only Python's standard library.  It implements
the small polled subset of the AMD AXI Quad SPI legacy register interface that
is needed to identify, erase, program, read, and verify the board flash.
"""

from __future__ import annotations

import argparse
import ctypes
from dataclasses import dataclass
import hashlib
import mmap
import os
from pathlib import Path
import sys
import time


# AXI Quad SPI legacy register offsets (PG153 / XSpi driver).
IISR = 0x20
SRR = 0x40
CR = 0x60
SR = 0x64
DTR = 0x68
DRR = 0x6C
SSR = 0x70

RESET_VALUE = 0x0A
CR_ENABLE = 0x00000002
CR_MASTER = 0x00000004
CR_TX_FIFO_RESET = 0x00000020
CR_RX_FIFO_RESET = 0x00000040
CR_MANUAL_SS = 0x00000080
CR_TRANS_INHIBIT = 0x00000100
CR_IDLE = CR_ENABLE | CR_MASTER | CR_MANUAL_SS | CR_TRANS_INHIBIT
CR_RUN = CR_ENABLE | CR_MASTER | CR_MANUAL_SS

INTR_TX_EMPTY = 0x00000004
SR_RX_EMPTY = 0x00000001
SR_TX_FULL = 0x00000008

FIFO_DEPTH = 256
ADDRESS_BYTES = 3
MAX_DATA_PER_TRANSFER = FIFO_DEPTH - 1 - ADDRESS_BYTES

CMD_READ_ID = 0x9F
CMD_READ_STATUS = 0x05
CMD_WRITE_ENABLE = 0x06
CMD_READ = 0x03
CMD_PAGE_PROGRAM = 0x02
CMD_ERASE_BLOCK = 0xD8

STATUS_WIP = 0x01
STATUS_WEL = 0x02


@dataclass(frozen=True)
class FlashProfile:
    """Geometry and status-register layout for one supported board flash."""

    name: str
    jedec_prefix: bytes
    physical_capacity: int
    erase_size: int
    page_size: int
    block_protect_mask: int
    operation_error_mask: int


# The 9Fh JEDEC prefix selects the board's fitted flash automatically.  The
# exact Aller A7 ISSI ordering code uses option R (standard 256-byte pages and
# 64-KiB blocks); its alternative K/T geometries are intentionally not named as
# supported here even though JEDEC 9Fh does not encode the package option.
FLASH_PROFILES = (
    FlashProfile(
        name="Spansion/Cypress S25FL256S",
        jedec_prefix=bytes.fromhex("01 02 19 4d 01 80"),
        physical_capacity=32 * 1024 * 1024,
        erase_size=64 * 1024,
        page_size=256,
        block_protect_mask=0x1C,
        operation_error_mask=0x60,
    ),
    FlashProfile(
        name="Infineon S25FL512SDSBHV210",
        jedec_prefix=bytes.fromhex("01 02 20"),
        physical_capacity=64 * 1024 * 1024,
        erase_size=256 * 1024,
        page_size=512,
        block_protect_mask=0x1C,
        operation_error_mask=0x60,
    ),
    FlashProfile(
        name="ISSI IS25LP512M-RHLE",
        jedec_prefix=bytes.fromhex("9d 60 1a"),
        physical_capacity=64 * 1024 * 1024,
        erase_size=64 * 1024,
        page_size=256,
        block_protect_mask=0x3C,
        operation_error_mask=0x00,
    ),
)


class FlashError(RuntimeError):
    pass


class AxiQuadSpi:
    def __init__(self, device: str, base: int, timeout: float) -> None:
        self.device = device
        self.base = base
        self.timeout = timeout
        self.fd = -1
        self.bar: mmap.mmap | None = None
        self.words = None
        self.register_base = 0

    def __enter__(self) -> "AxiQuadSpi":
        self.fd = os.open(self.device, os.O_RDWR | os.O_SYNC)
        map_offset = self.base & ~(mmap.PAGESIZE - 1)
        self.register_base = self.base - map_offset
        map_length = (
            self.register_base + 0x100 + mmap.PAGESIZE - 1
        ) & ~(mmap.PAGESIZE - 1)
        try:
            self.bar = mmap.mmap(
                self.fd,
                map_length,
                flags=mmap.MAP_SHARED,
                prot=mmap.PROT_READ | mmap.PROT_WRITE,
                offset=map_offset,
            )
            # ctypes forces each register operation to be one aligned 32-bit
            # MMIO access.  Byte-wise buffer writes are not valid for this IP.
            self.words = (ctypes.c_uint32 * (map_length // 4)).from_buffer(self.bar)
        except Exception:
            os.close(self.fd)
            self.fd = -1
            raise
        self.reset()
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        if self.bar is not None:
            try:
                self.write32(CR, CR_IDLE)
                self.write32(SSR, 0xFFFFFFFF)
            finally:
                self.words = None
                self.bar.close()
                self.bar = None
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def read32(self, register: int) -> int:
        assert self.words is not None
        return int(self.words[(self.register_base + register) // 4])

    def write32(self, register: int, value: int) -> None:
        assert self.words is not None
        self.words[(self.register_base + register) // 4] = value & 0xFFFFFFFF

    def reset(self) -> None:
        self.write32(SRR, RESET_VALUE)
        time.sleep(0.001)
        self.write32(
            CR,
            CR_IDLE | CR_TX_FIFO_RESET | CR_RX_FIFO_RESET,
        )
        self.write32(CR, CR_IDLE)
        self.write32(SSR, 0xFFFFFFFF)
        pending = self.read32(IISR)
        if pending:
            self.write32(IISR, pending)

    def transfer(self, transmit: bytes) -> bytes:
        if not 1 <= len(transmit) <= FIFO_DEPTH:
            raise ValueError(f"SPI transfer must contain 1..{FIFO_DEPTH} bytes")

        self.write32(CR, CR_IDLE | CR_TX_FIFO_RESET | CR_RX_FIFO_RESET)
        self.write32(CR, CR_IDLE)
        self.write32(SSR, 0xFFFFFFFF)
        pending = self.read32(IISR)
        if pending:
            self.write32(IISR, pending)

        for value in transmit:
            if self.read32(SR) & SR_TX_FULL:
                raise FlashError("AXI Quad SPI TX FIFO filled before the advertised depth")
            self.write32(DTR, value)

        # A non-posted read flushes all preceding PCIe BAR writes before the
        # slave-select/control writes that start the SPI transaction.
        self.read32(CR)

        self.write32(SSR, 0x00000000)
        self.write32(CR, CR_RUN)
        deadline = time.monotonic() + self.timeout
        while True:
            pending = self.read32(IISR)
            if pending & INTR_TX_EMPTY:
                break
            if time.monotonic() >= deadline:
                self.write32(CR, CR_IDLE)
                self.write32(SSR, 0xFFFFFFFF)
                raise FlashError(
                    "timeout waiting for AXI Quad SPI TX-empty interrupt "
                    f"(IISR=0x{self.read32(IISR):08x}, "
                    f"SR=0x{self.read32(SR):08x}, "
                    f"CR=0x{self.read32(CR):08x}, "
                    f"SSR=0x{self.read32(SSR):08x})"
                )

        self.write32(CR, CR_IDLE)
        received = bytearray()
        for _ in transmit:
            if self.read32(SR) & SR_RX_EMPTY:
                self.write32(SSR, 0xFFFFFFFF)
                raise FlashError(
                    f"AXI Quad SPI RX FIFO underflow after {len(received)} bytes"
                )
            received.append(self.read32(DRR) & 0xFF)

        self.write32(SSR, 0xFFFFFFFF)
        pending = self.read32(IISR)
        if pending:
            self.write32(IISR, pending)
        return bytes(received)


class SpiNor:
    def __init__(self, spi: AxiQuadSpi) -> None:
        self.spi = spi
        self.jedec_id = self.read_id()
        if len(self.jedec_id) < 3:
            raise FlashError("flash returned a truncated JEDEC ID")
        self.profile = next(
            (
                profile
                for profile in FLASH_PROFILES
                if self.jedec_id.startswith(profile.jedec_prefix)
            ),
            None,
        )
        if self.profile is None:
            supported = ", ".join(
                f"{profile.name} ({profile.jedec_prefix.hex(' ')})"
                for profile in FLASH_PROFILES
            )
            raise FlashError(
                f"unsupported flash JEDEC ID {self.jedec_id.hex(' ')}; "
                f"supported devices: {supported}"
            )
        self.physical_capacity = self.profile.physical_capacity
        self.erase_size = self.profile.erase_size
        self.page_size = self.profile.page_size
        # The synthesized core has C_SPI_MEM_ADDR_BITS=24.  A 256-Mbit flash
        # or 512-Mbit flash is valid, but this programmer intentionally accesses
        # only bank 0 (addresses 0x000000..0xffffff).  That lower boot window is
        # large enough for the Artix-7 configuration image and avoids relying on
        # vendor-specific bank-register or four-byte-address state.
        self.capacity = min(self.physical_capacity, 1 << 24)

    def read_id(self) -> bytes:
        response = self.spi.transfer(bytes([CMD_READ_ID]) + bytes(6))
        return response[1:]

    def read_status(self) -> int:
        return self.spi.transfer(bytes([CMD_READ_STATUS, 0x00]))[1]

    def write_enable(self) -> None:
        self.spi.transfer(bytes([CMD_WRITE_ENABLE]))
        status = self.read_status()
        if not status & STATUS_WEL:
            raise FlashError(f"flash rejected Write Enable (status=0x{status:02x})")

    def wait_ready(self, timeout: float, operation: str) -> None:
        deadline = time.monotonic() + timeout
        while True:
            status = self.read_status()
            if status & self.profile.operation_error_mask:
                raise FlashError(
                    f"flash reported an error during {operation} (status=0x{status:02x})"
                )
            if not status & STATUS_WIP:
                if status & STATUS_WEL:
                    raise FlashError(
                        f"{operation} did not start (WEL remained set, status=0x{status:02x})"
                    )
                return
            if time.monotonic() >= deadline:
                raise FlashError(
                    f"timeout waiting for {operation} (status=0x{status:02x})"
                )
            time.sleep(0.002)

    @staticmethod
    def address(address: int) -> bytes:
        if not 0 <= address <= 0xFFFFFF:
            raise ValueError(f"address outside 24-bit flash range: 0x{address:x}")
        return address.to_bytes(ADDRESS_BYTES, "big")

    def read(self, address: int, length: int) -> bytes:
        if address < 0 or length < 0 or address + length > self.capacity:
            raise ValueError("flash read outside device capacity")
        output = bytearray()
        while len(output) < length:
            chunk = min(MAX_DATA_PER_TRANSFER, length - len(output))
            command = bytes([CMD_READ]) + self.address(address + len(output))
            response = self.spi.transfer(command + bytes(chunk))
            output.extend(response[len(command) :])
        return bytes(output)

    def erase_sector(self, address: int) -> None:
        if address % self.erase_size:
            raise ValueError(
                f"erase address is not {self.erase_size // 1024}-KiB aligned"
            )
        self.write_enable()
        self.spi.transfer(bytes([CMD_ERASE_BLOCK]) + self.address(address))
        self.wait_ready(
            180.0,
            f"{self.erase_size // 1024}-KiB erase at 0x{address:06x}",
        )

    def program(self, address: int, data: bytes) -> None:
        offset = 0
        while offset < len(data):
            current = address + offset
            page_remaining = self.page_size - (current % self.page_size)
            chunk_size = min(MAX_DATA_PER_TRANSFER, page_remaining, len(data) - offset)
            chunk = data[offset : offset + chunk_size]
            self.write_enable()
            self.spi.transfer(
                bytes([CMD_PAGE_PROGRAM]) + self.address(current) + chunk
            )
            self.wait_ready(5.0, f"page program at 0x{current:06x}")
            offset += chunk_size


def parse_bitstream(path: Path) -> tuple[bytes, dict[str, str]]:
    raw = path.read_bytes()
    if len(raw) < 16:
        raise FlashError(f"bitstream is too short: {path}")

    position = 0
    first_length = int.from_bytes(raw[position : position + 2], "big")
    position += 2 + first_length
    if position + 2 > len(raw):
        raise FlashError("truncated Xilinx .bit header")

    # The second length describes the following one-byte field tag.  Like the
    # AMD/openFPGALoader parser, consume the length but let the loop consume tag.
    second_length = int.from_bytes(raw[position : position + 2], "big")
    position += 2
    if second_length != 1:
        raise FlashError(
            f"unexpected Xilinx .bit secondary-header length {second_length}"
        )

    metadata: dict[str, str] = {}
    field_names = {ord("a"): "design", ord("b"): "part", ord("c"): "date", ord("d"): "time"}
    while position < len(raw):
        field_type = raw[position]
        position += 1
        if field_type == ord("e"):
            if position + 4 > len(raw):
                raise FlashError("truncated Xilinx .bit payload length")
            payload_length = int.from_bytes(raw[position : position + 4], "big")
            position += 4
            if position + payload_length > len(raw):
                raise FlashError(
                    "Xilinx .bit payload is shorter than its declared length"
                )
            payload = raw[position : position + payload_length]
            break

        if position + 2 > len(raw):
            raise FlashError("truncated Xilinx .bit metadata length")
        field_length = int.from_bytes(raw[position : position + 2], "big")
        position += 2
        if position + field_length > len(raw):
            raise FlashError("truncated Xilinx .bit metadata")
        value = raw[position : position + field_length].rstrip(b"\x00")
        position += field_length
        name = field_names.get(field_type)
        if name:
            metadata[name] = value.decode("ascii", errors="replace")
    else:
        raise FlashError("Xilinx .bit payload field was not found")

    if b"\xaa\x99\x55\x66" not in payload[:4096]:
        raise FlashError("Xilinx sync word was not found near the .bit payload start")
    part = metadata.get("part", "").lower()
    if part and "7a200t" not in part:
        raise FlashError(
            f"bitstream targets {metadata['part']!r}, not the expected xc7a200t"
        )
    return payload, metadata


def load_image(path: Path) -> tuple[bytes, dict[str, str]]:
    suffix = path.suffix.lower()
    if suffix == ".bit":
        image, metadata = parse_bitstream(path)
    elif suffix == ".bin":
        image = path.read_bytes()
        metadata = {}
        if b"\xaa\x99\x55\x66" not in image[:4096]:
            raise FlashError(
                "Xilinx sync word was not found near the .bin start; provide a raw "
                "write_bitstream -bin_file output or SPIx4 write_cfgmem image"
            )
    else:
        raise FlashError("supported image extensions are .bit and .bin")

    if not image or all(value == 0xFF for value in image):
        raise FlashError("refusing to program an empty/all-0xff image")
    return image, metadata


def display_flash(flash: SpiNor) -> None:
    jedec = " ".join(f"{value:02x}" for value in flash.jedec_id)
    status = flash.read_status()
    print(f"JEDEC ID:       {jedec}")
    print(f"flash:          {flash.profile.name}")
    print(f"physical size:  {flash.physical_capacity // (1024 * 1024)} MiB")
    print(f"24-bit window:  {flash.capacity // (1024 * 1024)} MiB")
    print(f"erase block:    {flash.erase_size // 1024} KiB")
    print(f"program page:   {flash.page_size} bytes")
    print(f"status:         0x{status:02x}")
    if status & flash.profile.block_protect_mask:
        print("write protect:  block-protect bits are set")
    else:
        print("write protect:  block-protect bits are clear")


def program_and_verify(flash: SpiNor, image: bytes) -> None:
    if len(image) > flash.capacity:
        raise FlashError(
            f"image is {len(image)} bytes but flash capacity is {flash.capacity} bytes"
        )
    status = flash.read_status()
    if status & flash.profile.operation_error_mask:
        raise FlashError(
            f"flash has a latched erase/program error (status=0x{status:02x}); "
            "power-cycle or clear the condition before programming"
        )
    if status & flash.profile.block_protect_mask:
        raise FlashError(
            f"flash block-protect bits are set (status=0x{status:02x}); refusing to alter protection"
        )

    sector_count = (len(image) + flash.erase_size - 1) // flash.erase_size
    changed = 0
    for sector_index in range(sector_count):
        address = sector_index * flash.erase_size
        desired = image[address : address + flash.erase_size]
        if len(desired) < flash.erase_size:
            desired += bytes([0xFF]) * (flash.erase_size - len(desired))
        current = flash.read(address, flash.erase_size)
        if current == desired:
            print(f"[{sector_index + 1:3d}/{sector_count:3d}] 0x{address:06x}: unchanged")
            continue

        changed += 1
        print(f"[{sector_index + 1:3d}/{sector_count:3d}] 0x{address:06x}: erase/program")
        flash.erase_sector(address)
        for page_offset in range(0, flash.erase_size, flash.page_size):
            page = desired[page_offset : page_offset + flash.page_size]
            if any(value != 0xFF for value in page):
                flash.program(address + page_offset, page)
        if flash.read(address, flash.erase_size) != desired:
            raise FlashError(f"verification failed in sector at 0x{address:06x}")

    programmed = flash.read(0, len(image))
    if programmed != image:
        raise FlashError("final byte-for-byte verification failed")
    print(f"changed sectors: {changed}/{sector_count}")
    print(f"verified SHA256: {hashlib.sha256(programmed).hexdigest()}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Program the boot SPI flash through XDMA and AXI Quad SPI"
    )
    parser.add_argument("image", nargs="?", type=Path, help="Xilinx .bit or raw .bin image")
    parser.add_argument("--identify", action="store_true", help="identify flash only; do not write")
    parser.add_argument(
        "--yes",
        action="store_true",
        help="confirm destructive erase/program operation",
    )
    parser.add_argument("--device", default="/dev/xdma0_user", help="XDMA user device")
    parser.add_argument(
        "--base",
        type=lambda value: int(value, 0),
        default=0x10000,
        help="AXI Quad SPI byte offset in the user BAR (default: 0x10000)",
    )
    parser.add_argument(
        "--spi-timeout",
        type=float,
        default=1.0,
        help="timeout for one AXI Quad SPI FIFO transaction",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if os.geteuid() != 0:
        raise FlashError("this utility must be run as root")
    if args.base < 0 or args.base % 4:
        raise FlashError("AXI Quad SPI base must be a nonnegative 32-bit-aligned offset")
    if not args.identify and args.image is None:
        raise FlashError("an IMAGE path is required unless --identify is used")
    if args.identify and args.image is not None:
        raise FlashError("--identify does not accept an image path")

    image = b""
    metadata: dict[str, str] = {}
    if args.image is not None:
        if not args.image.is_file():
            raise FlashError(f"image not found: {args.image}")
        image, metadata = load_image(args.image)
        print(f"image:          {args.image.resolve()}")
        print(f"image bytes:    {len(image)}")
        print(f"image SHA256:   {hashlib.sha256(image).hexdigest()}")
        for key in ("design", "part", "date", "time"):
            if key in metadata:
                print(f"bit {key + ':':<10} {metadata[key]}")
        if not args.yes:
            raise FlashError("refusing to erase/program without explicit --yes")

    print(f"XDMA user BAR:  {args.device}")
    print(f"AXI QSPI base:  0x{args.base:08x}")
    with AxiQuadSpi(args.device, args.base, args.spi_timeout) as spi:
        flash = SpiNor(spi)
        display_flash(flash)
        if args.identify:
            return 0
        program_and_verify(flash, image)
    print("QSPI flash programming and verification completed successfully.")
    print("Power-cycle the board to boot this image; the current design has no ICAP/IPROG reload path.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (FlashError, OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
