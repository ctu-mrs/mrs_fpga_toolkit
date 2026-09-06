/**
 * @file qspi.h
 * @brief AXI Quad SPI and supported SPI-NOR device drivers.
 */

#pragma once

#include "mrs_fpga_dev/export.h"
#include "mrs_fpga_dev/ip_driver_lite.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mrs::fpga {

/** Static geometry and opcodes for an identified SPI-NOR device. */
struct QspiFlashProfile {
    std::string name;
    std::vector<std::uint8_t> jedec_prefix;
    std::uint64_t physical_capacity = 0;
    std::uint32_t erase_size = 0;
    std::uint32_t page_size = 0;
    std::uint8_t protect_mask = 0;
    std::uint8_t error_mask = 0;
    unsigned int address_bytes = 0;
    std::uint8_t read_command = 0;
    std::uint8_t program_command = 0;
    std::uint8_t erase_command = 0;
};

/** Raised when the synthesized AXI Quad SPI core rejects an opcode. */
class MRS_FPGA_API QspiCommandError : public std::runtime_error {
public:
    explicit QspiCommandError(std::uint8_t opcode);
    std::uint8_t opcode() const noexcept;

private:
    std::uint8_t opcode_;
};

/**
 * Polled byte transfers through an AMD AXI Quad SPI core in standard mode.
 *
 * The register map and control semantics follow Product Guide PG153 v3.2 for
 * the 32-bit AXI4-Lite, non-XIP register interface. This driver targets a
 * master configured for eight-bit transfers, slave 0, manual slave selection,
 * and 256-element transmit/receive FIFOs. Those synthesis settings make each
 * software transfer a complete flash command of at most 256 bytes.
 *
 * Construction performs the STARTUP-clock handoff sequence used when the SPI
 * clock is routed through STARTUPE2. Every transfer resets both FIFOs, selects
 * exactly one flash slave, and restores the controller to an inhibited,
 * deselected state.
 */
class MRS_FPGA_API AxiQuadSpi final : public IpDriverLite {
public:
    static constexpr std::uint32_t default_base_address = 0x10000;
    /** Synthesized transmit and receive FIFO depth required by this driver. */
    static constexpr std::size_t fifo_depth = 256;

    AxiQuadSpi(
        RegisterAccess& registers,
        std::uint32_t base_address = default_base_address,
        std::chrono::milliseconds timeout = std::chrono::seconds(1));
    ~AxiQuadSpi() override;

    std::vector<std::uint8_t> transfer(const std::vector<std::uint8_t>& transmit);

private:
    void clearInterrupts();
    void resetFifos();
    void reset();
    void primeStartupClock();

    std::chrono::milliseconds timeout_;
};

/**
 * Auto-detected SPI-NOR access for the flash devices used on supported boards.
 *
 * Writes are page-aware, erases are sector-aligned, and every mutating command
 * checks Write Enable and waits for completion or a bounded timeout.
 */
class MRS_FPGA_API SpiNor final {
public:
    explicit SpiNor(AxiQuadSpi& spi);

    const QspiFlashProfile& profile() const noexcept;
    const std::vector<std::uint8_t>& jedecId() const noexcept;
    std::uint64_t capacity() const noexcept;
    std::uint8_t readStatus();

    bool isMicron() const noexcept;
    std::uint16_t readMicronNvcr();
    static bool micronBootSafe(std::uint16_t value) noexcept;
    std::pair<std::uint16_t, std::uint16_t> repairMicronBootMode();

    void checkRange(std::uint64_t address, std::uint64_t length) const;
    std::vector<std::uint8_t> read(std::uint64_t address, std::size_t length);
    void eraseSector(std::uint64_t address);
    void program(std::uint64_t address, const std::vector<std::uint8_t>& data);

private:
    std::vector<std::uint8_t> readId();
    static const QspiFlashProfile* matchProfile(const std::vector<std::uint8_t>& id);
    static bool isIdleBus(const std::vector<std::uint8_t>& id);
    bool tryRecovery(std::uint8_t command);
    void recoverSerialMode();
    std::vector<std::uint8_t> encodeAddress(std::uint64_t address) const;
    void writeEnable();
    void waitReady(
        std::chrono::milliseconds timeout,
        const std::string& operation,
        std::chrono::microseconds interval = std::chrono::milliseconds(2));

    AxiQuadSpi& spi_;
    std::vector<std::uint8_t> jedec_id_;
    const QspiFlashProfile* profile_ = nullptr;
    std::uint64_t capacity_ = 0;
};

}  // namespace mrs::fpga
