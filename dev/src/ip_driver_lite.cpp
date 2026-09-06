/**
 * @file ip_driver_lite.cpp
 * @brief Bounds-checked AXI-Lite IP register helpers.
 */

#include "mrs_fpga_dev/ip_driver_lite.h"

#include <limits>
#include <stdexcept>
#include <thread>

namespace mrs::fpga {

IpDriverLite::IpDriverLite(
    RegisterAccess& registers,
    std::uint32_t base_address,
    std::uint32_t register_span)
    : registers_(registers),
      base_address_(base_address),
      register_span_(register_span) {
    if ((base_address_ & 0x3U) != 0U) {
        throw std::invalid_argument("AXI-Lite IP base address must be 32-bit aligned");
    }
    if (register_span_ < sizeof(std::uint32_t) || (register_span_ & 0x3U) != 0U) {
        throw std::invalid_argument("AXI-Lite IP register span must be positive and aligned");
    }
}

std::uint32_t IpDriverLite::read32(std::uint32_t offset) const {
    validateOffset(offset, sizeof(std::uint32_t));
    return registers_.readReg(base_address_ + offset);
}

void IpDriverLite::write32(std::uint32_t offset, std::uint32_t value) {
    validateOffset(offset, sizeof(std::uint32_t));
    registers_.writeReg(base_address_ + offset, value);
}

std::uint64_t IpDriverLite::read64(std::uint32_t low_offset) const {
    return read64(low_offset, low_offset + sizeof(std::uint32_t));
}

std::uint64_t IpDriverLite::read64(
    std::uint32_t low_offset, std::uint32_t high_offset) const {
    validateOffset(low_offset, sizeof(std::uint32_t));
    validateOffset(high_offset, sizeof(std::uint32_t));
    // Read low then high to match the register ordering used by MRS IP.  IPs
    // with live counters should provide their own atomic snapshot protocol.
    const std::uint32_t low = read32(low_offset);
    const std::uint32_t high = read32(high_offset);
    return (static_cast<std::uint64_t>(high) << 32U) | low;
}

std::uint64_t IpDriverLite::read64Stable(
    std::uint32_t low_offset, std::uint32_t high_offset) const {
    validateOffset(low_offset, sizeof(std::uint32_t));
    validateOffset(high_offset, sizeof(std::uint32_t));
    while (true) {
        const std::uint32_t first_high = read32(high_offset);
        const std::uint32_t low = read32(low_offset);
        const std::uint32_t second_high = read32(high_offset);
        if (first_high == second_high) {
            return (static_cast<std::uint64_t>(second_high) << 32U) | low;
        }
    }
}

void IpDriverLite::write64(std::uint32_t low_offset, std::uint64_t value) {
    write64(low_offset, low_offset + sizeof(std::uint32_t), value);
}

void IpDriverLite::write64(
    std::uint32_t low_offset,
    std::uint32_t high_offset,
    std::uint64_t value) {
    validateOffset(low_offset, sizeof(std::uint32_t));
    validateOffset(high_offset, sizeof(std::uint32_t));
    write32(low_offset, static_cast<std::uint32_t>(value));
    write32(high_offset, static_cast<std::uint32_t>(value >> 32U));
}

std::uint32_t IpDriverLite::update32(
    std::uint32_t offset, std::uint32_t mask, std::uint32_t value) {
    const std::uint32_t updated = (read32(offset) & ~mask) | (value & mask);
    write32(offset, updated);
    return updated;
}

bool IpDriverLite::waitMasked(
    std::uint32_t offset,
    std::uint32_t mask,
    std::uint32_t expected,
    std::chrono::steady_clock::time_point deadline,
    std::chrono::microseconds poll_interval,
    std::uint32_t* final_value) const {
    validateOffset(offset, sizeof(std::uint32_t));
    if (poll_interval.count() < 0) {
        throw std::invalid_argument("AXI-Lite polling interval must not be negative");
    }
    while (true) {
        const std::uint32_t observed = read32(offset);
        if (final_value != nullptr) {
            *final_value = observed;
        }
        if ((observed & mask) == (expected & mask)) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        if (poll_interval.count() != 0) {
            std::this_thread::sleep_for(poll_interval);
        }
    }
}

std::uint32_t IpDriverLite::baseAddress() const noexcept { return base_address_; }
std::uint32_t IpDriverLite::registerSpan() const noexcept { return register_span_; }
void IpDriverLite::setBaseAddress(std::uint32_t base_address) {
    if ((base_address & 0x3U) != 0U) {
        throw std::invalid_argument("AXI-Lite IP base address must be 32-bit aligned");
    }
    base_address_ = base_address;
}
RegisterAccess& IpDriverLite::registers() noexcept { return registers_; }
const RegisterAccess& IpDriverLite::registers() const noexcept { return registers_; }

void IpDriverLite::validateOffset(std::uint32_t offset, std::uint32_t width) const {
    if ((offset & 0x3U) != 0U) {
        throw std::invalid_argument("AXI-Lite register offset must be 32-bit aligned");
    }
    if (width > register_span_ || offset > register_span_ - width) {
        throw std::out_of_range("AXI-Lite register offset lies outside the IP span");
    }
    if (base_address_ > std::numeric_limits<std::uint32_t>::max() - offset) {
        throw std::out_of_range("AXI-Lite absolute register address overflows 32 bits");
    }
}

}  // namespace mrs::fpga
