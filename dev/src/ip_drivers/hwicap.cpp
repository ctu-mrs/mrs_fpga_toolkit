/**
 * @file hwicap.cpp
 * @brief IP-driver implementation for atomic AXI HWICAP IPROG sequences.
 */

#include "mrs_fpga_dev/ip_drivers/hwicap.h"

#include <array>
#include <stdexcept>
#include <thread>

namespace mrs::fpga {
namespace {
// AXI HWICAP PG134 v3.0 register map and CR/SR bit definitions.
constexpr std::uint32_t write_fifo = 0x100;
constexpr std::uint32_t control = 0x10c;
constexpr std::uint32_t status = 0x110;
constexpr std::uint32_t write_vacancy = 0x114;
constexpr std::uint32_t control_write = 0x00000001;
constexpr std::uint32_t control_read = 0x00000002;
constexpr std::uint32_t control_reset = 0x00000008;
constexpr std::uint32_t control_active = control_write | control_read;
constexpr std::uint32_t status_ready = 0x00000001 | 0x00000004;
}  // namespace

AxiHwIcap::AxiHwIcap(
    RegisterAccess& registers,
    std::uint32_t base_address,
    std::chrono::milliseconds timeout,
    std::uint32_t expected_fifo_vacancy)
    : IpDriverLite(registers, base_address, 0x200),
      timeout_(timeout),
      expected_fifo_vacancy_(expected_fifo_vacancy) {
    if (timeout_.count() <= 0 || expected_fifo_vacancy_ < 8) {
        throw std::invalid_argument("HWICAP timeout must be positive and FIFO vacancy at least 8");
    }
}

void AxiHwIcap::validate() const {
    requireReady();
    if ((read32(control) & ~0x1fU) != 0U) {
        throw std::runtime_error("AXI HWICAP control has reserved bits set");
    }
    if (read32(write_vacancy) > expected_fifo_vacancy_) {
        throw std::runtime_error("unexpected AXI HWICAP FIFO vacancy");
    }
}

void AxiHwIcap::triggerIprog(std::uint32_t flash_address) {
    if (flash_address > maximum_flash_address || (flash_address & 0xffU) != 0U) {
        throw std::invalid_argument(
            "warm-boot address must be 256-byte aligned and within 0x00ffff00");
    }
    reset();
    // UG470's complete 7-series IPROG packet. In 24-bit SPI configuration
    // mode, WBSTAR carries flash byte-address bits 23:8.
    const std::array<std::uint32_t, 8> words{
        0xffffffff, 0xaa995566, 0x20000000, 0x30020001,
        flash_address >> 8U, 0x30008001, 0x0000000f, 0x20000000,
    };
    if (read32(write_vacancy) < words.size()) {
        throw std::runtime_error("AXI HWICAP has insufficient write-FIFO vacancy");
    }
    for (const std::uint32_t word : words) write32(write_fifo, word);
    (void)read32(write_vacancy);
    write32(control, control_write);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void AxiHwIcap::requireReady() const {
    if ((read32(status) & status_ready) != status_ready) {
        throw std::runtime_error("AXI HWICAP is not ready; DONE and EOS are required");
    }
}

void AxiHwIcap::waitIdle() const {
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    while ((read32(control) & control_active) != 0U) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("timeout waiting for AXI HWICAP");
        }
    }
}

void AxiHwIcap::reset() {
    const std::uint32_t inactive = read32(control) & ~control_active;
    write32(control, inactive | control_reset);
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    write32(control, inactive & ~control_reset);
    waitIdle();
    requireReady();
    if (read32(write_vacancy) != expected_fifo_vacancy_) {
        throw std::runtime_error("AXI HWICAP FIFO did not return to its expected empty state");
    }
}

}  // namespace mrs::fpga
