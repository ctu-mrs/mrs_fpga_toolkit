#!/usr/bin/env python3
"""Manage persistent SPI-NOR through XDMA, AXI Quad SPI, and AXI HWICAP.

Supported operations are identification/status, reading to a local file,
validating an image without writing, verified erase/program, and optional
whole-FPGA reload from flash.  The implementation uses only Python's standard
library and the companion mrs_fpga_pcie_xdma.py module.
"""

from __future__ import annotations

import argparse
import ctypes
from dataclasses import dataclass
import hashlib
import json
import mmap
import os
from pathlib import Path
import shlex
import sys
import time
from typing import Protocol

import mrs_fpga_pcie_xdma as pcie


# AXI Quad SPI legacy register map (AMD PG153 / standalone XSpi driver).
SPI_IISR = 0x20
SPI_SRR = 0x40
SPI_CR = 0x60
SPI_SR = 0x64
SPI_DTR = 0x68
SPI_DRR = 0x6C
SPI_SSR = 0x70

SPI_RESET_VALUE = 0x0A
SPI_CR_ENABLE = 0x00000002
SPI_CR_MASTER = 0x00000004
SPI_CR_TX_FIFO_RESET = 0x00000020
SPI_CR_RX_FIFO_RESET = 0x00000040
SPI_CR_MANUAL_SS = 0x00000080
SPI_CR_TRANS_INHIBIT = 0x00000100
SPI_CR_IDLE = SPI_CR_ENABLE | SPI_CR_MASTER | SPI_CR_MANUAL_SS | SPI_CR_TRANS_INHIBIT
SPI_CR_RUN = SPI_CR_ENABLE | SPI_CR_MASTER | SPI_CR_MANUAL_SS
SPI_INTR_TX_EMPTY = 0x00000004
SPI_INTR_COMMAND_ERROR = 0x00002000
SPI_SR_RX_EMPTY = 0x00000001
SPI_SR_TX_EMPTY = 0x00000004
SPI_SR_COMMAND_ERROR = 0x00000400

SPI_FIFO_DEPTH = 256
SPI_ADDRESS_BYTES = 3

CMD_READ_ID = 0x9F
CMD_READ_STATUS = 0x05
CMD_WRITE_ENABLE = 0x06
CMD_READ = 0x03
CMD_READ_4BYTE = 0x13
CMD_PAGE_PROGRAM = 0x02
CMD_PAGE_PROGRAM_4BYTE = 0x12
CMD_ERASE_BLOCK = 0xD8
CMD_ERASE_BLOCK_4BYTE = 0xDC
CMD_RELEASE_POWER_DOWN = 0xAB
CMD_MODE_BIT_RESET = 0xFF
CMD_SPANSION_RESET = 0xF0
CMD_RESET_QUAD_PROTOCOL = 0xF5
CMD_RESET_ENABLE = 0x66
CMD_RESET_MEMORY = 0x99
STATUS_WIP = 0x01
STATUS_WEL = 0x02
FLASH_RESET_RECOVERY = 0.001

# AXI HWICAP v3.0 register map (AMD PG134 / standalone XHwIcap driver).
ICAP_WF = 0x100
ICAP_CR = 0x10C
ICAP_SR = 0x110
ICAP_WFV = 0x114
ICAP_CR_WRITE = 0x00000001
ICAP_CR_READ = 0x00000002
ICAP_CR_SW_RESET = 0x00000008
ICAP_CR_ACTIVE = ICAP_CR_WRITE | ICAP_CR_READ
ICAP_SR_READY = 0x00000001 | 0x00000004
ICAP_FIFO_VACANCY = 63
ICAP_DUMMY = 0xFFFFFFFF
ICAP_SYNC = 0xAA995566
ICAP_NOOP = 0x20000000
ICAP_WRITE_WBSTAR = 0x30020001
ICAP_WRITE_CMD = 0x30008001
ICAP_CMD_IPROG = 0x0000000F
ICAP_MAX_FLASH_ADDRESS = 0x00FFFF00


@dataclass(frozen=True)
class FlashProfile:
    name: str
    jedec_prefix: bytes
    physical_capacity: int
    erase_size: int
    page_size: int
    block_protect_mask: int
    operation_error_mask: int
    address_bytes: int = SPI_ADDRESS_BYTES
    read_command: int = CMD_READ
    program_command: int = CMD_PAGE_PROGRAM
    erase_command: int = CMD_ERASE_BLOCK


FLASH_PROFILES = (
    FlashProfile(
        "Spansion/Cypress S25FL256S",
        bytes.fromhex("01 02 19 4d 01 80"),
        32 * 1024 * 1024,
        64 * 1024,
        256,
        0x1C,
        0x60,
    ),
    FlashProfile(
        "Infineon S25FL512SDSBHV210",
        bytes.fromhex("01 02 20"),
        64 * 1024 * 1024,
        256 * 1024,
        512,
        0x1C,
        0x60,
    ),
    FlashProfile(
        "ISSI IS25LP512M-RHLE",
        bytes.fromhex("9d 60 1a"),
        64 * 1024 * 1024,
        64 * 1024,
        256,
        0x3C,
        0x00,
    ),
    FlashProfile(
        "Micron MT25QL01GBBB8E12-0SIT",
        bytes.fromhex("20 ba 21"),
        128 * 1024 * 1024,
        64 * 1024,
        256,
        0x5C,
        0x00,
        address_bytes=4,
        read_command=CMD_READ_4BYTE,
        program_command=CMD_PAGE_PROGRAM_4BYTE,
        erase_command=CMD_ERASE_BLOCK_4BYTE,
    ),
)


class FlashError(RuntimeError):
    """An actionable flash, image, or controller error."""


class SpiCommandError(FlashError):
    """The synthesized dual/quad controller rejected an opcode."""

    def __init__(self, opcode: int, state: dict[str, int]) -> None:
        self.opcode = opcode
        self.state = state
        super().__init__(
            f"AXI Quad SPI rejected opcode 0x{opcode:02x}; the synthesized "
            "controller command profile does not support it "
            f"({AxiQuadSpi.format_state(state)})"
        )


def require_root() -> None:
    if not hasattr(os, "geteuid") or os.geteuid() != 0:
        raise FlashError("this utility must be run as root")


def integer(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid integer: {value!r}") from error


class MappedRegisters:
    """Aligned 32-bit MMIO access to one IP in an XDMA user BAR."""

    def __init__(self, device: str, base: int, span: int = 0x200) -> None:
        self.device = device
        self.base = base
        self.span = span
        self.fd = -1
        self.mapping: mmap.mmap | None = None
        self.words = None
        self.register_base = 0

    def __enter__(self) -> "MappedRegisters":
        self.fd = os.open(self.device, os.O_RDWR | os.O_SYNC)
        map_offset = self.base & ~(mmap.PAGESIZE - 1)
        self.register_base = self.base - map_offset
        map_length = (self.register_base + self.span + mmap.PAGESIZE - 1) & ~(
            mmap.PAGESIZE - 1
        )
        try:
            self.mapping = mmap.mmap(
                self.fd,
                map_length,
                flags=mmap.MAP_SHARED,
                prot=mmap.PROT_READ | mmap.PROT_WRITE,
                offset=map_offset,
            )
            self.words = (ctypes.c_uint32 * (map_length // 4)).from_buffer(self.mapping)
        except Exception:
            os.close(self.fd)
            self.fd = -1
            raise
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        if self.mapping is not None:
            self.words = None
            self.mapping.close()
            self.mapping = None
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def read32(self, register: int) -> int:
        assert self.words is not None
        return int(self.words[(self.register_base + register) // 4])

    def write32(self, register: int, value: int) -> None:
        assert self.words is not None
        self.words[(self.register_base + register) // 4] = value & 0xFFFFFFFF


class AxiQuadSpi(MappedRegisters):
    """Polled AXI Quad SPI transfers for the synthesized legacy-mode core."""

    def __init__(self, device: str, base: int, timeout: float) -> None:
        super().__init__(device, base)
        self.timeout = timeout
        self.last_transfer_state: dict[str, int] = {}

    def __enter__(self) -> "AxiQuadSpi":
        super().__enter__()
        # With C_USE_STARTUP=1, clock STARTUP before the first software reset.
        # This transaction follows the AMD driver workaround exactly, including
        # selecting the flash before loading the transmit FIFO.
        self.prime_startup_clock()
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        if self.mapping is not None:
            try:
                self.write32(SPI_CR, SPI_CR_IDLE)
                self.write32(SPI_SSR, 0xFFFFFFFF)
            finally:
                super().__exit__(exc_type, exc_value, traceback)
        else:
            super().__exit__(exc_type, exc_value, traceback)

    def reset(self) -> None:
        self.write32(SPI_SRR, SPI_RESET_VALUE)
        time.sleep(0.001)
        self.write32(
            SPI_CR,
            SPI_CR_IDLE | SPI_CR_TX_FIFO_RESET | SPI_CR_RX_FIFO_RESET,
        )
        self.write32(SPI_CR, SPI_CR_IDLE)
        self.write32(SPI_SSR, 0xFFFFFFFF)
        pending = self.read32(SPI_IISR)
        if pending:
            self.write32(SPI_IISR, pending)

    def prime_startup_clock(self) -> None:
        self.write32(
            SPI_CR,
            SPI_CR_IDLE | SPI_CR_TX_FIFO_RESET | SPI_CR_RX_FIFO_RESET,
        )
        self.write32(SPI_CR, SPI_CR_IDLE)
        self.write32(SPI_SSR, 0xFFFFFFFF)
        pending = self.read32(SPI_IISR)
        if pending:
            self.write32(SPI_IISR, pending)

        try:
            self.write32(SPI_SSR, 0x00000000)
            for value in (CMD_READ_ID, 0x00, 0x00):
                self.write32(SPI_DTR, value)
            self.write32(SPI_CR, SPI_CR_RUN)

            deadline = time.monotonic() + self.timeout
            while True:
                status = self.read32(SPI_SR)
                pending = self.read32(SPI_IISR)
                if status & SPI_SR_COMMAND_ERROR or pending & SPI_INTR_COMMAND_ERROR:
                    state = self.register_state()
                    raise FlashError(
                        "AXI Quad SPI rejected its STARTUP clock transaction "
                        f"({self.format_state(state)})"
                    )
                if status & SPI_SR_TX_EMPTY:
                    break
                if time.monotonic() >= deadline:
                    state = self.register_state()
                    raise FlashError(
                        "timeout priming the AXI Quad SPI STARTUP clock "
                        f"({self.format_state(state)})"
                    )

            self.write32(SPI_CR, SPI_CR_IDLE)
            for _ in range(SPI_FIFO_DEPTH):
                if self.read32(SPI_SR) & SPI_SR_RX_EMPTY:
                    break
                self.read32(SPI_DRR)
        finally:
            self.write32(SPI_CR, SPI_CR_IDLE)
            self.write32(SPI_SSR, 0xFFFFFFFF)
            self.reset()

    def register_state(self) -> dict[str, int]:
        return {
            "IISR": self.read32(SPI_IISR),
            "SR": self.read32(SPI_SR),
            "CR": self.read32(SPI_CR),
            "SSR": self.read32(SPI_SSR),
        }

    @staticmethod
    def format_state(state: dict[str, int]) -> str:
        return ", ".join(f"{name}=0x{value:08x}" for name, value in state.items())

    def transfer(self, transmit: bytes) -> bytes:
        if not 1 <= len(transmit) <= SPI_FIFO_DEPTH:
            raise ValueError(f"SPI transfer must contain 1..{SPI_FIFO_DEPTH} bytes")
        self.write32(
            SPI_CR,
            SPI_CR_IDLE | SPI_CR_TX_FIFO_RESET | SPI_CR_RX_FIFO_RESET,
        )
        self.write32(SPI_CR, SPI_CR_IDLE)
        self.write32(SPI_SSR, 0xFFFFFFFF)
        pending = self.read32(SPI_IISR)
        if pending:
            self.write32(SPI_IISR, pending)

        # The reset FIFOs and bounded transfer make per-byte status polling
        # unnecessary.  Avoiding those extra PCIe MMIO reads triples throughput
        # while retaining the same final byte-for-byte verification.
        for value in transmit:
            self.write32(SPI_DTR, value)
        self.read32(SPI_CR)  # Flush posted FIFO writes.
        self.write32(SPI_SSR, 0x00000000)
        self.write32(SPI_CR, SPI_CR_RUN)

        deadline = time.monotonic() + self.timeout
        while True:
            pending = self.read32(SPI_IISR)
            if pending & SPI_INTR_COMMAND_ERROR:
                self.write32(SPI_CR, SPI_CR_IDLE)
                state = self.register_state()
                self.last_transfer_state = state
                self.write32(SPI_SSR, 0xFFFFFFFF)
                self.write32(SPI_IISR, pending)
                raise SpiCommandError(transmit[0], state)
            if pending & SPI_INTR_TX_EMPTY:
                break
            if time.monotonic() >= deadline:
                self.write32(SPI_CR, SPI_CR_IDLE)
                self.write32(SPI_SSR, 0xFFFFFFFF)
                state = self.register_state()
                raise FlashError(
                    "timeout waiting for AXI Quad SPI TX-empty interrupt "
                    f"({self.format_state(state)})"
                )

        self.write32(SPI_CR, SPI_CR_IDLE)
        self.last_transfer_state = self.register_state()
        received = bytes(self.read32(SPI_DRR) & 0xFF for _ in transmit)
        self.write32(SPI_SSR, 0xFFFFFFFF)
        pending = self.read32(SPI_IISR)
        if pending:
            self.write32(SPI_IISR, pending)
        return received


class SpiNor:
    """Profile-driven SPI-NOR operations for auto-detected board flashes."""

    def __init__(self, spi: AxiQuadSpi) -> None:
        self.spi = spi
        self.rejected_recovery_commands: list[int] = []
        self.jedec_id = self.read_id()
        self.profile = self.match_profile(self.jedec_id)
        if self.profile is None and self.is_idle_bus_id(self.jedec_id):
            self.recover_serial_mode()
            self.profile = self.match_profile(self.jedec_id)
        if self.profile is None:
            supported = ", ".join(
                f"{profile.name} ({profile.jedec_prefix.hex(' ')})"
                for profile in FLASH_PROFILES
            )
            state = self.spi.last_transfer_state
            diagnostic = (
                f"; controller after Read-ID: {AxiQuadSpi.format_state(state)}"
                if state
                else ""
            )
            if self.is_idle_bus_id(self.jedec_id):
                level = "high" if self.jedec_id[0] == 0xFF else "low"
                rejected = ""
                if self.rejected_recovery_commands:
                    opcodes = ", ".join(
                        f"0x{opcode:02x}" for opcode in self.rejected_recovery_commands
                    )
                    rejected = (
                        f" The controller rejected recovery opcode(s) {opcodes}; "
                        "its synthesized flash-vendor command profile does not "
                        "match all recovery commands required by the installed "
                        "device."
                    )
                raise FlashError(
                    f"SPI flash did not respond; JEDEC ID remained "
                    f"{self.jedec_id.hex(' ')} (DQ1 sampled {level}) after "
                    f"STARTUP handoff and safe reset/wake recovery{diagnostic}. "
                    f"{rejected} Cold-power-cycle the board if the flash was "
                    "previously left in 4-4-4 QPI mode; the AXI Quad SPI core "
                    "cannot issue a 4-bit command phase. "
                    f"Supported devices: {supported}"
                )
            raise FlashError(
                f"unsupported flash JEDEC ID {self.jedec_id.hex(' ')}"
                f"{diagnostic}; supported devices: {supported}"
            )
        self.physical_capacity = self.profile.physical_capacity
        self.capacity = min(
            self.physical_capacity,
            1 << (8 * self.profile.address_bytes),
        )
        self.erase_size = self.profile.erase_size
        self.page_size = self.profile.page_size

    def read_id(self) -> bytes:
        return self.spi.transfer(bytes([CMD_READ_ID]) + bytes(6))[1:]

    @staticmethod
    def match_profile(jedec_id: bytes) -> FlashProfile | None:
        return next(
            (
                profile
                for profile in FLASH_PROFILES
                if jedec_id.startswith(profile.jedec_prefix)
            ),
            None,
        )

    @staticmethod
    def is_idle_bus_id(jedec_id: bytes) -> bool:
        return bool(jedec_id) and len(set(jedec_id)) == 1 and jedec_id[0] in (0, 0xFF)

    def recover_serial_mode(self) -> None:
        """Apply only non-destructive commands accepted by the built core."""
        # ABh releases Micron/ISSI devices from deep power-down and is also a
        # harmless electronic-signature command on S25FL-S.
        self.try_recovery_command(CMD_RELEASE_POWER_DOWN)
        time.sleep(FLASH_RESET_RECOVERY)
        self.jedec_id = self.read_id()
        if self.match_profile(self.jedec_id) is not None:
            return

        # Micron uses F5h to reset quad protocol and 66h/99h for software reset.
        # A vendor-mismatched dual/quad AXI core can reject these before they
        # reach the pins, so unsupported commands are recorded and skipped.
        self.try_recovery_command(CMD_RESET_QUAD_PROTOCOL)
        reset_enabled = self.try_recovery_command(CMD_RESET_ENABLE)
        if reset_enabled:
            self.try_recovery_command(CMD_RESET_MEMORY)
        time.sleep(FLASH_RESET_RECOVERY)
        self.jedec_id = self.read_id()
        if self.match_profile(self.jedec_id) is not None:
            return

        # S25FL-S uses FFh followed by F0h to leave enhanced modes and reset.
        mode_reset = self.try_recovery_command(CMD_MODE_BIT_RESET)
        if mode_reset:
            self.try_recovery_command(CMD_SPANSION_RESET)
        time.sleep(FLASH_RESET_RECOVERY)
        self.jedec_id = self.read_id()

    def try_recovery_command(self, opcode: int) -> bool:
        try:
            self.spi.transfer(bytes([opcode]))
        except SpiCommandError:
            self.rejected_recovery_commands.append(opcode)
            return False
        return True

    def read_status(self) -> int:
        return self.spi.transfer(bytes([CMD_READ_STATUS, 0]))[1]

    def address(self, value: int) -> bytes:
        maximum = (1 << (8 * self.profile.address_bytes)) - 1
        if not 0 <= value <= maximum:
            raise ValueError(
                f"address outside {self.profile.address_bytes * 8}-bit flash "
                f"range: 0x{value:x}"
            )
        return value.to_bytes(self.profile.address_bytes, "big")

    def check_range(self, address: int, length: int) -> None:
        if address < 0 or length < 0 or address + length > self.capacity:
            raise ValueError(
                f"range 0x{address:x}+0x{length:x} exceeds the "
                f"{self.capacity}-byte accessible flash window"
            )

    def read(self, address: int, length: int) -> bytes:
        self.check_range(address, length)
        output = bytearray()
        maximum_data = SPI_FIFO_DEPTH - 1 - self.profile.address_bytes
        while len(output) < length:
            chunk = min(maximum_data, length - len(output))
            command = bytes([self.profile.read_command]) + self.address(
                address + len(output)
            )
            response = self.spi.transfer(command + bytes(chunk))
            output.extend(response[len(command) :])
        return bytes(output)

    def write_enable(self) -> None:
        self.spi.transfer(bytes([CMD_WRITE_ENABLE]))
        status = self.read_status()
        if not status & STATUS_WEL:
            raise FlashError(f"flash rejected Write Enable (status=0x{status:02x})")

    def wait_ready(
        self, timeout: float, operation: str, poll_interval: float = 0.002
    ) -> None:
        deadline = time.monotonic() + timeout
        while True:
            status = self.read_status()
            if status & self.profile.operation_error_mask:
                raise FlashError(
                    f"flash reported an error during {operation} "
                    f"(status=0x{status:02x})"
                )
            if not status & STATUS_WIP:
                if status & STATUS_WEL:
                    raise FlashError(
                        f"{operation} did not start "
                        f"(WEL remained set, status=0x{status:02x})"
                    )
                return
            if time.monotonic() >= deadline:
                raise FlashError(
                    f"timeout waiting for {operation} (status=0x{status:02x})"
                )
            time.sleep(poll_interval)

    def erase_sector(self, address: int) -> None:
        if address % self.erase_size:
            raise ValueError(f"erase address must be {self.erase_size}-byte aligned")
        self.write_enable()
        self.spi.transfer(bytes([self.profile.erase_command]) + self.address(address))
        self.wait_ready(
            180.0,
            f"{self.erase_size // 1024}-KiB erase at 0x{address:06x}",
        )

    def program(self, address: int, data: bytes) -> None:
        self.check_range(address, len(data))
        offset = 0
        maximum_data = SPI_FIFO_DEPTH - 1 - self.profile.address_bytes
        while offset < len(data):
            current = address + offset
            page_remaining = self.page_size - current % self.page_size
            chunk_size = min(
                maximum_data,
                page_remaining,
                len(data) - offset,
            )
            chunk = data[offset : offset + chunk_size]
            self.write_enable()
            self.spi.transfer(
                bytes([self.profile.program_command]) + self.address(current) + chunk
            )
            self.wait_ready(
                5.0,
                f"page program at 0x{current:06x}",
                poll_interval=0.0001,
            )
            offset += chunk_size


class RegisterIo(Protocol):
    def read32(self, register: int) -> int: ...

    def write32(self, register: int, value: int) -> None: ...


class AxiHwIcap:
    """The safe atomic IPROG subset of AXI HWICAP."""

    def __init__(
        self,
        registers: RegisterIo,
        timeout: float,
        expected_fifo_vacancy: int = ICAP_FIFO_VACANCY,
    ) -> None:
        self.registers = registers
        self.timeout = timeout
        self.expected_fifo_vacancy = expected_fifo_vacancy

    def read32(self, register: int) -> int:
        return self.registers.read32(register)

    def write32(self, register: int, value: int) -> None:
        self.registers.write32(register, value)

    def wait_idle(self) -> None:
        deadline = time.monotonic() + self.timeout
        while self.read32(ICAP_CR) & ICAP_CR_ACTIVE:
            if time.monotonic() >= deadline:
                raise FlashError(
                    "timeout waiting for AXI HWICAP "
                    f"(CR=0x{self.read32(ICAP_CR):08x}, "
                    f"SR=0x{self.read32(ICAP_SR):08x})"
                )

    def require_ready(self) -> None:
        status = self.read32(ICAP_SR)
        if status & ICAP_SR_READY != ICAP_SR_READY:
            raise FlashError(
                "AXI HWICAP is not ready: DONE and EOS must both be set "
                f"(SR=0x{status:08x})"
            )

    def validate(self) -> None:
        self.require_ready()
        control = self.read32(ICAP_CR)
        vacancy = self.read32(ICAP_WFV)
        if control & ~0x1F:
            raise FlashError(
                f"AXI HWICAP control register has reserved bits set "
                f"(CR=0x{control:08x})"
            )
        if not 0 <= vacancy <= self.expected_fifo_vacancy:
            raise FlashError(
                f"unexpected AXI HWICAP FIFO vacancy {vacancy}; "
                f"expected at most {self.expected_fifo_vacancy}"
            )

    def reset(self) -> None:
        control = self.read32(ICAP_CR) & ~ICAP_CR_ACTIVE
        self.write32(ICAP_CR, control | ICAP_CR_SW_RESET)
        time.sleep(0.000010)
        self.write32(ICAP_CR, control & ~ICAP_CR_SW_RESET)
        self.wait_idle()
        self.require_ready()
        vacancy = self.read32(ICAP_WFV)
        if vacancy != self.expected_fifo_vacancy:
            raise FlashError(
                f"AXI HWICAP FIFO vacancy is {vacancy} after reset; "
                f"expected {self.expected_fifo_vacancy}"
            )

    def trigger_iprog(self, flash_address: int) -> None:
        if not 0 <= flash_address <= ICAP_MAX_FLASH_ADDRESS:
            raise ValueError(
                f"warm-boot address must be 0..0x{ICAP_MAX_FLASH_ADDRESS:08x}"
            )
        if flash_address & 0xFF:
            raise ValueError("warm-boot address must be 256-byte aligned")
        self.reset()
        words = (
            ICAP_DUMMY,
            ICAP_SYNC,
            ICAP_NOOP,
            ICAP_WRITE_WBSTAR,
            flash_address >> 8,
            ICAP_WRITE_CMD,
            ICAP_CMD_IPROG,
            ICAP_NOOP,
        )
        vacancy = self.read32(ICAP_WFV)
        if vacancy < len(words):
            raise FlashError(
                f"AXI HWICAP needs {len(words)} vacant FIFO words, has {vacancy}"
            )
        for word in words:
            self.write32(ICAP_WF, word)
        # The read orders all posted BAR writes.  Never poll after the control
        # write: successful IPROG removes the device and this MMIO path.
        self.read32(ICAP_WFV)
        sys.stdout.flush()
        self.write32(ICAP_CR, ICAP_CR_WRITE)
        time.sleep(0.050)


def parse_bitstream(path: Path) -> tuple[bytes, dict[str, str]]:
    raw = path.read_bytes()
    if len(raw) < 16:
        raise FlashError(f"bitstream is too short: {path}")
    position = 0
    first_length = int.from_bytes(raw[position : position + 2], "big")
    position += 2 + first_length
    if position + 2 > len(raw):
        raise FlashError("truncated Xilinx .bit header")
    second_length = int.from_bytes(raw[position : position + 2], "big")
    position += 2
    if second_length != 1:
        raise FlashError(
            f"unexpected Xilinx .bit secondary-header length {second_length}"
        )

    names = {
        ord("a"): "design",
        ord("b"): "part",
        ord("c"): "date",
        ord("d"): "time",
    }
    metadata: dict[str, str] = {}
    while position < len(raw):
        tag = raw[position]
        position += 1
        if tag == ord("e"):
            if position + 4 > len(raw):
                raise FlashError("truncated Xilinx .bit payload length")
            length = int.from_bytes(raw[position : position + 4], "big")
            position += 4
            if position + length > len(raw):
                raise FlashError("Xilinx .bit payload is shorter than declared")
            payload = raw[position : position + length]
            break
        if position + 2 > len(raw):
            raise FlashError("truncated Xilinx .bit metadata length")
        length = int.from_bytes(raw[position : position + 2], "big")
        position += 2
        if position + length > len(raw):
            raise FlashError("truncated Xilinx .bit metadata")
        value = raw[position : position + length].rstrip(b"\x00")
        position += length
        if tag in names:
            metadata[names[tag]] = value.decode("ascii", errors="replace")
    else:
        raise FlashError("Xilinx .bit payload field was not found")
    if b"\xaa\x99\x55\x66" not in payload[:4096]:
        raise FlashError("Xilinx sync word was not found near the .bit payload start")
    return payload, metadata


def load_image(path: Path, expected_part: str | None) -> tuple[bytes, dict[str, str]]:
    if not path.is_file():
        raise FlashError(f"image not found: {path}")
    suffix = path.suffix.lower()
    if suffix == ".bit":
        image, metadata = parse_bitstream(path)
    elif suffix == ".bin":
        image = path.read_bytes()
        metadata = {}
        if b"\xaa\x99\x55\x66" not in image[:4096]:
            raise FlashError(
                "Xilinx sync word was not found near the .bin start; provide "
                "a raw configuration or write_cfgmem image"
            )
    else:
        raise FlashError("supported image extensions are .bit and .bin")
    if not image or all(value == 0xFF for value in image):
        raise FlashError("refusing an empty/all-0xff image")
    if expected_part:
        actual = metadata.get("part", "")
        if not actual:
            raise FlashError("--expected-part requires a .bit file with part metadata")
        if expected_part.lower() not in actual.lower():
            raise FlashError(
                f"bitstream targets {actual!r}, expected part containing "
                f"{expected_part!r}"
            )
    return image, metadata


def flash_record(flash: SpiNor) -> dict[str, object]:
    status = flash.read_status()
    return {
        "jedec_id": flash.jedec_id.hex(" "),
        "profile": flash.profile.name,
        "physical_capacity": flash.physical_capacity,
        "accessible_capacity": flash.capacity,
        "erase_size": flash.erase_size,
        "page_size": flash.page_size,
        "status": status,
        "block_protected": bool(status & flash.profile.block_protect_mask),
        "operation_error": bool(status & flash.profile.operation_error_mask),
    }


def display_flash(record: dict[str, object]) -> None:
    print(f"JEDEC ID:        {record['jedec_id']}")
    print(f"flash:           {record['profile']}")
    print(f"physical size:   {int(record['physical_capacity']) // 1048576} MiB")
    print(f"accessible size: {int(record['accessible_capacity']) // 1048576} MiB")
    print(f"erase block:     {int(record['erase_size']) // 1024} KiB")
    print(f"program page:    {record['page_size']} bytes")
    print(f"status:          0x{int(record['status']):02x}")
    print(
        "write protect:   "
        + ("block-protect bits are set" if record["block_protected"] else "clear")
    )


def require_writable(flash: SpiNor) -> None:
    record = flash_record(flash)
    if record["operation_error"]:
        raise FlashError(
            f"flash has a latched erase/program error "
            f"(status=0x{int(record['status']):02x})"
        )
    if record["block_protected"]:
        raise FlashError(
            f"flash block-protect bits are set "
            f"(status=0x{int(record['status']):02x}); protection is not altered"
        )


def image_intersection(
    image: bytes, image_offset: int, sector_address: int, sector_size: int
) -> tuple[int, int, bytes]:
    start = max(image_offset, sector_address)
    end = min(image_offset + len(image), sector_address + sector_size)
    if start >= end:
        return 0, 0, b""
    source_start = start - image_offset
    return (
        start - sector_address,
        end - sector_address,
        image[source_start : source_start + end - start],
    )


def program_and_verify(
    flash: SpiNor,
    image: bytes,
    image_offset: int,
    *,
    rewrite_all: bool,
) -> None:
    flash.check_range(image_offset, len(image))
    require_writable(flash)
    first_sector = image_offset // flash.erase_size
    last_sector = (image_offset + len(image) + flash.erase_size - 1) // flash.erase_size
    sector_count = last_sector - first_sector
    changed = 0
    started = time.monotonic()

    for ordinal, sector_index in enumerate(range(first_sector, last_sector), start=1):
        address = sector_index * flash.erase_size
        destination_start, destination_end, source = image_intersection(
            image, image_offset, address, flash.erase_size
        )
        fully_covered = destination_start == 0 and destination_end == flash.erase_size
        current: bytes | None
        if rewrite_all and fully_covered:
            current = None
            desired = source
        else:
            current = flash.read(address, flash.erase_size)
            desired_buffer = bytearray(current)
            desired_buffer[destination_start:destination_end] = source
            desired = bytes(desired_buffer)
        if not rewrite_all and current == desired:
            print(f"[{ordinal:3d}/{sector_count:3d}] " f"0x{address:06x}: unchanged")
            continue

        changed += 1
        print(f"[{ordinal:3d}/{sector_count:3d}] " f"0x{address:06x}: erase/program")
        flash.erase_sector(address)
        for page_offset in range(0, flash.erase_size, flash.page_size):
            page = desired[page_offset : page_offset + flash.page_size]
            if any(value != 0xFF for value in page):
                flash.program(address + page_offset, page)

    print("final byte-for-byte verification")
    verify_started = time.monotonic()
    actual = flash.read(image_offset, len(image))
    if actual != image:
        mismatch = next(
            index
            for index, (observed, expected) in enumerate(zip(actual, image))
            if observed != expected
        )
        raise FlashError(
            f"verification failed at flash 0x{image_offset + mismatch:06x}: "
            f"read 0x{actual[mismatch]:02x}, expected 0x{image[mismatch]:02x}"
        )
    verify_elapsed = time.monotonic() - verify_started
    elapsed = time.monotonic() - started
    print(f"changed sectors: {changed}/{sector_count}")
    print(f"verified SHA256: {hashlib.sha256(actual).hexdigest()}")
    print(
        f"verification:    {verify_elapsed:.1f} s "
        f"({len(image) / max(verify_elapsed, 1e-9) / 1e6:.3f} MB/s)"
    )
    print(f"total elapsed:   {elapsed:.1f} s")


def validate_image(flash: SpiNor, image: bytes, offset: int) -> None:
    flash.check_range(offset, len(image))
    started = time.monotonic()
    actual = flash.read(offset, len(image))
    elapsed = time.monotonic() - started
    if actual != image:
        mismatch = next(
            index
            for index, (observed, expected) in enumerate(zip(actual, image))
            if observed != expected
        )
        raise FlashError(
            f"validation failed at flash 0x{offset + mismatch:06x}: "
            f"read 0x{actual[mismatch]:02x}, expected 0x{image[mismatch]:02x}"
        )
    print(f"validated bytes:  {len(image)}")
    print(f"validated SHA256: {hashlib.sha256(actual).hexdigest()}")
    print(f"read throughput:  {len(actual) / max(elapsed, 1e-9) / 1e6:.3f} MB/s")


def reload_from_flash(args: argparse.Namespace) -> None:
    if not args.yes:
        raise FlashError("refusing whole-FPGA reload without explicit --yes")
    vendor_id = pcie.parse_hex_id(args.vendor_id, "vendor ID")
    device_id = pcie.parse_hex_id(args.device_id, "device ID")
    module_args = shlex.split(args.module_args)
    required_nodes = pcie.parse_required_nodes(args.required_nodes)
    bdf = pcie.find_current_bdf(
        explicit=args.bdf,
        device_index=args.device_index,
        vendor_id=vendor_id,
        device_id=device_id,
        module=args.module,
        allow_absent_explicit=bool(args.bdf),
    )
    if bdf is None:
        bdf = pcie.read_saved_bdf(args.device_index)
    if (
        bdf is None
        or not pcie.endpoint_path(bdf).exists()
        or not Path(args.device).exists()
    ):
        print(
            "PCIe/XDMA is not currently usable; attempting PCI rescan and "
            "driver recovery before accessing AXI HWICAP"
        )
        bdf = pcie.recover_missing_endpoint(
            preferred_bdf=bdf,
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
    if not Path(args.device).exists():
        raise FlashError(
            f"XDMA recovery did not create {args.device}; AXI HWICAP cannot "
            "be reached without a working PCIe user BAR"
        )
    pcie.remember_bdf(args.device_index, bdf)
    print(f"saved PCI endpoint: {bdf}")
    print(
        f"issuing IPROG from flash address 0x{args.boot_address:08x}; "
        "XDMA disappearance is expected"
    )
    with MappedRegisters(args.device, args.hwicap_base) as registers:
        icap = AxiHwIcap(
            registers,
            args.hwicap_timeout,
            expected_fifo_vacancy=args.hwicap_fifo_vacancy,
        )
        icap.validate()
        icap.trigger_iprog(args.boot_address)
    pcie.reload_xdma(
        mode="post-reconfigure",
        bdf=bdf,
        device_index=args.device_index,
        module=args.module,
        module_args=module_args,
        required_nodes=required_nodes,
        timeout=args.wait,
        device_mode=args.device_mode,
    )
    print("FPGA reloaded from persistent flash and PCIe/XDMA recovered")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Read, validate, or program persistent SPI-NOR through AXI Quad "
            "SPI, with optional verified reload"
        )
    )
    parser.add_argument(
        "image", nargs="?", type=Path, help=".bit or .bin image to program"
    )
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument(
        "--status", action="store_true", help="identify flash and show status"
    )
    actions.add_argument(
        "--read", type=Path, metavar="FILE", help="read flash to a local file"
    )
    actions.add_argument(
        "--validate",
        type=Path,
        metavar="IMAGE",
        help="compare flash to IMAGE without writing",
    )
    parser.add_argument(
        "--reload",
        action="store_true",
        help="after programming, or by itself, boot through AXI HWICAP IPROG",
    )
    parser.add_argument(
        "--yes", action="store_true", help="confirm erase/program or FPGA reload"
    )
    parser.add_argument(
        "--rewrite-all",
        action="store_true",
        help="rewrite every touched sector instead of skipping equal sectors",
    )
    parser.add_argument(
        "--offset",
        type=integer,
        default=0,
        help="flash byte offset for read/write/validate (default: 0)",
    )
    parser.add_argument(
        "--length",
        type=integer,
        help="bytes for --read (default: accessible capacity minus offset)",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="allow --read to overwrite an existing local file",
    )
    parser.add_argument(
        "--expected-part",
        help="optional substring required in .bit part metadata",
    )
    parser.add_argument(
        "--device", default="/dev/xdma0_user", help="XDMA user BAR device"
    )
    parser.add_argument(
        "--qspi-base",
        type=integer,
        default=0x10000,
        help="AXI Quad SPI BAR offset (default: 0x10000)",
    )
    parser.add_argument(
        "--spi-timeout",
        type=float,
        default=1.0,
        help="timeout for one AXI Quad SPI transaction",
    )
    parser.add_argument(
        "--hwicap-base",
        type=integer,
        default=0x30000,
        help="AXI HWICAP BAR offset (default: 0x30000)",
    )
    parser.add_argument(
        "--hwicap-timeout",
        type=float,
        default=1.0,
        help="AXI HWICAP transfer timeout",
    )
    parser.add_argument(
        "--hwicap-fifo-vacancy",
        type=int,
        default=ICAP_FIFO_VACANCY,
        help="expected empty HWICAP FIFO vacancy (default: 63)",
    )
    parser.add_argument(
        "--boot-address",
        type=integer,
        help="IPROG warm-boot byte address (default: image --offset, otherwise 0)",
    )
    parser.add_argument("--json", action="store_true", help="JSON output for --status")
    parser.add_argument("--bdf", help="PCI BDF saved across IPROG")
    parser.add_argument("--device-index", type=int, default=0, help="XDMA device index")
    parser.add_argument("--vendor-id", default="0x10ee", help="fallback PCI vendor ID")
    parser.add_argument("--device-id", help="optional fallback PCI device ID")
    parser.add_argument("--module", default="xdma", help="XDMA kernel module")
    parser.add_argument("--module-args", default="", help="modprobe arguments")
    parser.add_argument(
        "--required-nodes",
        default="control",
        help="XDMA node suffixes required after reload",
    )
    parser.add_argument("--wait", type=float, default=30.0, help="recovery timeout")
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
    if args.qspi_base < 0 or args.qspi_base % 4:
        raise FlashError("--qspi-base must be a nonnegative aligned offset")
    if args.hwicap_base < 0 or args.hwicap_base % 4:
        raise FlashError("--hwicap-base must be a nonnegative aligned offset")
    if args.spi_timeout <= 0 or args.hwicap_timeout <= 0:
        raise FlashError("controller timeouts must be positive")
    if args.hwicap_fifo_vacancy < 8:
        raise FlashError("--hwicap-fifo-vacancy must be at least 8")
    if args.offset < 0:
        raise FlashError("--offset must be nonnegative")
    if args.length is not None and args.length < 0:
        raise FlashError("--length must be nonnegative")
    if args.device_index < 0 or args.wait <= 0:
        raise FlashError("device index must be nonnegative and wait must be positive")

    secondary_action = args.status or args.read is not None or args.validate is not None
    if secondary_action and args.image is not None:
        raise FlashError(
            "positional IMAGE cannot be combined with status/read/validate"
        )
    if args.rewrite_all and args.image is None:
        raise FlashError("--rewrite-all is valid only when programming IMAGE")
    if args.length is not None and args.read is None:
        raise FlashError("--length is valid only with --read")
    if args.force and args.read is None:
        raise FlashError("--force is valid only with --read")
    if args.json and not args.status:
        raise FlashError("--json is valid only with --status")
    if args.reload and secondary_action:
        raise FlashError("--reload cannot be combined with status/read/validate")
    if not secondary_action and args.image is None and not args.reload:
        raise FlashError(
            "select --status, --read, --validate, provide IMAGE, or use --reload"
        )
    if args.boot_address is None:
        args.boot_address = args.offset if args.image is not None else 0

    image_path = args.image or args.validate
    image = b""
    metadata: dict[str, str] = {}
    if image_path is not None:
        image, metadata = load_image(image_path, args.expected_part)
        print(f"image:           {image_path.resolve()}")
        print(f"image bytes:     {len(image)}")
        print(f"image SHA256:    {hashlib.sha256(image).hexdigest()}")
        for key in ("design", "part", "date", "time"):
            if key in metadata:
                print(f"bit {key + ':':<11}{metadata[key]}")
    if args.image is not None and not args.yes:
        raise FlashError("refusing erase/program without explicit --yes")

    if not (args.reload and args.image is None):
        if not (args.status and args.json):
            print(f"XDMA user BAR:   {args.device}")
            print(f"AXI QSPI base:   0x{args.qspi_base:08x}")
        with AxiQuadSpi(args.device, args.qspi_base, args.spi_timeout) as spi:
            flash = SpiNor(spi)
            record = flash_record(flash)
            if args.status and args.json:
                serializable = dict(record)
                print(json.dumps(serializable, indent=2, sort_keys=True))
            else:
                display_flash(record)

            if args.status:
                return 0
            if args.read is not None:
                output = args.read.expanduser()
                if output.exists() and not args.force:
                    raise FlashError(
                        f"output exists: {output}; use --force to overwrite"
                    )
                length = (
                    args.length
                    if args.length is not None
                    else flash.capacity - args.offset
                )
                started = time.monotonic()
                data = flash.read(args.offset, length)
                elapsed = time.monotonic() - started
                output.write_bytes(data)
                print(f"read file:        {output.resolve()}")
                print(f"read bytes:       {len(data)}")
                print(f"read SHA256:      {hashlib.sha256(data).hexdigest()}")
                print(
                    f"read throughput:  "
                    f"{len(data) / max(elapsed, 1e-9) / 1e6:.3f} MB/s"
                )
                return 0
            if args.validate is not None:
                validate_image(flash, image, args.offset)
                print("flash validation passed")
                return 0
            assert args.image is not None
            program_and_verify(
                flash,
                image,
                args.offset,
                rewrite_all=args.rewrite_all,
            )
        print("QSPI erase/program and final verification completed successfully")

    if args.reload:
        reload_from_flash(args)
    elif args.image is not None:
        print(
            "the running FPGA image is unchanged; add --reload --yes to boot "
            "the verified flash image"
        )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (FlashError, pcie.ToolError, OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
