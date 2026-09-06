/**
 * @file qspi.cpp
 * @brief IP-driver implementation for AXI Quad SPI and attached SPI-NOR devices.
 */

#include "mrs_fpga_dev/ip_drivers/qspi.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <thread>

namespace mrs::fpga {
namespace {

// AMD PG153 AXI4-Lite non-XIP register offsets. IPISR is intentionally used
// without enabling interrupts: its latched TX-empty and command-error bits are
// convenient and deterministic polling conditions.
constexpr std::uint32_t ip_interrupt_status = 0x20;
constexpr std::uint32_t software_reset = 0x40;
constexpr std::uint32_t spi_control = 0x60;
constexpr std::uint32_t spi_status = 0x64;
constexpr std::uint32_t data_transmit = 0x68;
constexpr std::uint32_t data_receive = 0x6c;
constexpr std::uint32_t slave_select = 0x70;
constexpr std::uint32_t software_reset_key = 0x0a;
constexpr std::uint32_t enable = 0x00000002;
constexpr std::uint32_t master = 0x00000004;
constexpr std::uint32_t tx_fifo_reset = 0x00000020;
constexpr std::uint32_t rx_fifo_reset = 0x00000040;
constexpr std::uint32_t manual_select = 0x00000080;
constexpr std::uint32_t inhibit = 0x00000100;
constexpr std::uint32_t idle = enable | master | manual_select | inhibit;
constexpr std::uint32_t run = enable | master | manual_select;
constexpr std::uint32_t interrupt_tx_empty = 0x00000004;
constexpr std::uint32_t interrupt_command_error = 0x00002000;
constexpr std::uint32_t rx_empty = 0x00000001;
constexpr std::uint32_t select_none = 0xffffffff;
constexpr std::uint32_t select_flash = 0xfffffffe;

constexpr std::uint8_t read_id = 0x9f;
constexpr std::uint8_t read_status = 0x05;
constexpr std::uint8_t write_enable = 0x06;
constexpr std::uint8_t read = 0x03;
constexpr std::uint8_t read_4byte = 0x13;
constexpr std::uint8_t page_program = 0x02;
constexpr std::uint8_t page_program_4byte = 0x12;
constexpr std::uint8_t erase_block = 0xd8;
constexpr std::uint8_t erase_block_4byte = 0xdc;
constexpr std::uint8_t release_power_down = 0xab;
constexpr std::uint8_t mode_bit_reset = 0xff;
constexpr std::uint8_t spansion_reset = 0xf0;
constexpr std::uint8_t reset_quad_protocol = 0xf5;
constexpr std::uint8_t reset_enable = 0x66;
constexpr std::uint8_t reset_memory = 0x99;
constexpr std::uint8_t read_nvcr = 0xb5;
constexpr std::uint8_t write_nvcr = 0xb1;
constexpr std::uint8_t wip = 0x01;
constexpr std::uint8_t wel = 0x02;
constexpr std::uint16_t micron_safe_mask = 0x0e3f;

const std::array<QspiFlashProfile, 2> profiles{{
    {"Spansion/Cypress S25FL256S", {0x01, 0x02, 0x19, 0x4d, 0x01, 0x80},
     32ULL * 1024 * 1024, 64 * 1024, 256, 0x1c, 0x60, 3,
     read, page_program, erase_block},
    {"Micron MT25QL512", {0x20, 0xba, 0x20},
     64ULL * 1024 * 1024, 64 * 1024, 256, 0x5c, 0x00, 4,
     read_4byte, page_program_4byte, erase_block_4byte},
}};

std::string opcodeText(std::uint8_t opcode) {
    std::ostringstream output;
    output << "AXI Quad SPI rejected command 0x" << std::hex << std::setw(2)
           << std::setfill('0') << static_cast<unsigned int>(opcode);
    return output.str();
}

}  // namespace

QspiCommandError::QspiCommandError(std::uint8_t opcode)
    : std::runtime_error(opcodeText(opcode)), opcode_(opcode) {}

std::uint8_t QspiCommandError::opcode() const noexcept { return opcode_; }

AxiQuadSpi::AxiQuadSpi(
    RegisterAccess& registers,
    std::uint32_t base_address,
    std::chrono::milliseconds timeout)
    : IpDriverLite(registers, base_address, 0x200), timeout_(timeout) {
    if (timeout_.count() <= 0) {
        throw std::invalid_argument("AXI Quad SPI timeout must be positive");
    }
    primeStartupClock();
}

AxiQuadSpi::~AxiQuadSpi() {
    try {
        write32(spi_control, idle);
        write32(slave_select, select_none);
    } catch (...) {
    }
}

std::vector<std::uint8_t> AxiQuadSpi::transfer(
    const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty() || bytes.size() > fifo_depth) {
        throw std::invalid_argument("SPI transfer must contain 1..256 bytes");
    }
    resetFifos();
    write32(slave_select, select_none);
    clearInterrupts();
    for (const std::uint8_t value : bytes) write32(data_transmit, value);
    (void)read32(spi_control);
    write32(slave_select, select_flash);
    write32(spi_control, run);
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    while (true) {
        const std::uint32_t pending = read32(ip_interrupt_status);
        if ((pending & interrupt_command_error) != 0U) {
            write32(spi_control, idle);
            write32(slave_select, select_none);
            write32(ip_interrupt_status, pending);
            throw QspiCommandError(bytes.front());
        }
        if ((pending & interrupt_tx_empty) != 0U) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            write32(spi_control, idle);
            write32(slave_select, select_none);
            throw std::runtime_error("timeout waiting for AXI Quad SPI TX-empty interrupt");
        }
    }
    write32(spi_control, idle);
    std::vector<std::uint8_t> result(bytes.size());
    for (auto& value : result) value = static_cast<std::uint8_t>(read32(data_receive));
    write32(slave_select, select_none);
    clearInterrupts();
    return result;
}

void AxiQuadSpi::clearInterrupts() {
    // PG153 defines IPISR as read/toggle-on-write. Writing back the value just
    // observed toggles each pending source to its inactive state.
    const std::uint32_t pending = read32(ip_interrupt_status);
    if (pending != 0U) write32(ip_interrupt_status, pending);
}

void AxiQuadSpi::resetFifos() {
    const std::uint32_t mask = tx_fifo_reset | rx_fifo_reset;
    write32(spi_control, idle | mask);
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    while ((read32(spi_control) & mask) != 0U) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("timeout waiting for AXI Quad SPI FIFO reset");
        }
    }
    write32(spi_control, idle);
}

void AxiQuadSpi::reset() {
    // PG153 requires the exact 0x0a key; every other SRR write is undefined.
    write32(software_reset, software_reset_key);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    resetFifos();
    write32(slave_select, select_none);
    clearInterrupts();
}

void AxiQuadSpi::primeStartupClock() {
    resetFifos();
    write32(slave_select, select_none);
    clearInterrupts();
    try {
        for (const std::uint8_t value : {read_id, std::uint8_t{0}, std::uint8_t{0}}) {
            write32(data_transmit, value);
        }
        (void)read32(spi_control);
        write32(spi_control, run);
        (void)read32(spi_control);
        write32(spi_control, idle);
        for (int index = 0; index < 2; ++index) {
            if ((read32(spi_status) & rx_empty) != 0U) break;
            (void)read32(data_receive);
        }
    } catch (...) {
        write32(spi_control, idle);
        write32(slave_select, select_none);
        throw;
    }
    write32(spi_control, idle);
    write32(slave_select, select_none);
    reset();
}

SpiNor::SpiNor(AxiQuadSpi& spi) : spi_(spi) {
    jedec_id_ = readId();
    profile_ = matchProfile(jedec_id_);
    if (profile_ == nullptr && isIdleBus(jedec_id_)) {
        recoverSerialMode();
        profile_ = matchProfile(jedec_id_);
    }
    if (profile_ == nullptr) {
        throw std::runtime_error("unsupported or unresponsive SPI flash JEDEC ID");
    }
    capacity_ = std::min<std::uint64_t>(
        profile_->physical_capacity,
        std::uint64_t{1} << (8U * profile_->address_bytes));
}

const QspiFlashProfile& SpiNor::profile() const noexcept { return *profile_; }
const std::vector<std::uint8_t>& SpiNor::jedecId() const noexcept { return jedec_id_; }
std::uint64_t SpiNor::capacity() const noexcept { return capacity_; }

std::uint8_t SpiNor::readStatus() {
    return spi_.transfer({read_status, 0})[1];
}

bool SpiNor::isMicron() const noexcept {
    return profile_ == &profiles[1];
}

std::uint16_t SpiNor::readMicronNvcr() {
    if (!isMicron()) throw std::runtime_error("NVCR is available only on Micron MT25Q");
    const auto response = spi_.transfer({read_nvcr, 0, 0});
    return static_cast<std::uint16_t>(response[1]) |
           (static_cast<std::uint16_t>(response[2]) << 8U);
}

bool SpiNor::micronBootSafe(std::uint16_t value) noexcept {
    return (value & micron_safe_mask) == micron_safe_mask;
}

std::pair<std::uint16_t, std::uint16_t> SpiNor::repairMicronBootMode() {
    const std::uint16_t before = readMicronNvcr();
    const std::uint16_t after = before | micron_safe_mask;
    if (after != before) {
        writeEnable();
        spi_.transfer({
            write_nvcr,
            static_cast<std::uint8_t>(after),
            static_cast<std::uint8_t>(after >> 8U),
        });
        waitReady(std::chrono::seconds(10), "Micron nonvolatile configuration write");
        if (readMicronNvcr() != after) {
            throw std::runtime_error("Micron NVCR verification failed");
        }
    }
    spi_.transfer({reset_enable});
    spi_.transfer({reset_memory});
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    jedec_id_ = readId();
    if (matchProfile(jedec_id_) != profile_) {
        throw std::runtime_error("Micron flash did not return to extended SPI after NVCR reload");
    }
    return {before, after};
}

void SpiNor::checkRange(std::uint64_t address, std::uint64_t length) const {
    if (address > capacity_ || length > capacity_ - address) {
        throw std::out_of_range("requested range exceeds the accessible flash window");
    }
}

std::vector<std::uint8_t> SpiNor::read(std::uint64_t address, std::size_t length) {
    checkRange(address, length);
    std::vector<std::uint8_t> output;
    output.reserve(length);
    const std::size_t maximum_data = AxiQuadSpi::fifo_depth - 1 - profile_->address_bytes;
    while (output.size() < length) {
        const std::size_t chunk = std::min(maximum_data, length - output.size());
        std::vector<std::uint8_t> command{profile_->read_command};
        const auto encoded = encodeAddress(address + output.size());
        command.insert(command.end(), encoded.begin(), encoded.end());
        const std::size_t header = command.size();
        command.resize(header + chunk, 0);
        const auto response = spi_.transfer(command);
        output.insert(output.end(), response.begin() + header, response.end());
    }
    return output;
}

void SpiNor::eraseSector(std::uint64_t address) {
    if ((address % profile_->erase_size) != 0U) {
        throw std::invalid_argument("flash erase address is not sector aligned");
    }
    writeEnable();
    std::vector<std::uint8_t> command{profile_->erase_command};
    const auto encoded = encodeAddress(address);
    command.insert(command.end(), encoded.begin(), encoded.end());
    spi_.transfer(command);
    waitReady(std::chrono::seconds(180), "flash sector erase");
}

void SpiNor::program(std::uint64_t address, const std::vector<std::uint8_t>& data) {
    checkRange(address, data.size());
    std::size_t offset = 0;
    const std::size_t maximum_data = AxiQuadSpi::fifo_depth - 1 - profile_->address_bytes;
    while (offset < data.size()) {
        const std::uint64_t current = address + offset;
        const std::size_t page_remaining = profile_->page_size - current % profile_->page_size;
        const std::size_t chunk = std::min({maximum_data, page_remaining, data.size() - offset});
        writeEnable();
        std::vector<std::uint8_t> command{profile_->program_command};
        const auto encoded = encodeAddress(current);
        command.insert(command.end(), encoded.begin(), encoded.end());
        command.insert(command.end(), data.begin() + offset, data.begin() + offset + chunk);
        spi_.transfer(command);
        waitReady(std::chrono::seconds(5), "flash page program", std::chrono::microseconds(100));
        offset += chunk;
    }
}

std::vector<std::uint8_t> SpiNor::readId() {
    std::vector<std::uint8_t> command(7, 0);
    command[0] = read_id;
    const auto response = spi_.transfer(command);
    return {response.begin() + 1, response.end()};
}

const QspiFlashProfile* SpiNor::matchProfile(const std::vector<std::uint8_t>& id) {
    for (const auto& profile : profiles) {
        if (id.size() >= profile.jedec_prefix.size() &&
            std::equal(profile.jedec_prefix.begin(), profile.jedec_prefix.end(), id.begin())) {
            return &profile;
        }
    }
    return nullptr;
}

bool SpiNor::isIdleBus(const std::vector<std::uint8_t>& id) {
    return !id.empty() && (id.front() == 0 || id.front() == 0xff) &&
           std::all_of(id.begin(), id.end(), [&](std::uint8_t value) { return value == id.front(); });
}

bool SpiNor::tryRecovery(std::uint8_t command) {
    try {
        spi_.transfer({command});
        return true;
    } catch (const QspiCommandError&) {
        return false;
    }
}

void SpiNor::recoverSerialMode() {
    tryRecovery(release_power_down);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    jedec_id_ = readId();
    if (matchProfile(jedec_id_) != nullptr) return;
    tryRecovery(reset_quad_protocol);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    jedec_id_ = readId();
    if (matchProfile(jedec_id_) != nullptr) return;
    if (tryRecovery(reset_enable)) tryRecovery(reset_memory);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    tryRecovery(reset_quad_protocol);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    jedec_id_ = readId();
    if (matchProfile(jedec_id_) != nullptr) return;
    if (tryRecovery(mode_bit_reset)) tryRecovery(spansion_reset);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    jedec_id_ = readId();
}

std::vector<std::uint8_t> SpiNor::encodeAddress(std::uint64_t address) const {
    const std::uint64_t maximum = (std::uint64_t{1} << (8U * profile_->address_bytes)) - 1;
    if (address > maximum) throw std::out_of_range("flash address exceeds command width");
    std::vector<std::uint8_t> result(profile_->address_bytes);
    for (unsigned int index = 0; index < profile_->address_bytes; ++index) {
        const unsigned int shift = 8U * (profile_->address_bytes - index - 1U);
        result[index] = static_cast<std::uint8_t>(address >> shift);
    }
    return result;
}

void SpiNor::writeEnable() {
    spi_.transfer({write_enable});
    if ((readStatus() & wel) == 0U) throw std::runtime_error("flash rejected Write Enable");
}

void SpiNor::waitReady(
    std::chrono::milliseconds timeout,
    const std::string& operation,
    std::chrono::microseconds interval) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const std::uint8_t current = readStatus();
        if ((current & profile_->error_mask) != 0U) {
            throw std::runtime_error("flash reported an error during " + operation);
        }
        if ((current & wip) == 0U) {
            if ((current & wel) != 0U) {
                throw std::runtime_error(operation + " did not start; Write Enable remained set");
            }
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("timeout waiting for " + operation);
        }
        std::this_thread::sleep_for(interval);
    }
}

}  // namespace mrs::fpga
