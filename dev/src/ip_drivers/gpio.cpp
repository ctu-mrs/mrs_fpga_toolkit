/**
 * @file gpio.cpp
 * @brief Register-level implementation of the generic AXI GPIO driver.
 */

#include "mrs_fpga_dev/ip_drivers/gpio.h"

#include <stdexcept>

namespace mrs::fpga {
namespace {

// AMD PG144 register offsets. The interrupt registers are synthesized only
// when the IP's interrupt option is enabled; PG144 specifies zero reads and
// no-effect writes when configuration removes a register.
constexpr std::uint32_t channel1_data = 0x000;
constexpr std::uint32_t channel1_direction = 0x004;
constexpr std::uint32_t channel2_data = 0x008;
constexpr std::uint32_t channel2_direction = 0x00c;
constexpr std::uint32_t global_interrupt_enable = 0x11c;
constexpr std::uint32_t interrupt_status = 0x120;
constexpr std::uint32_t interrupt_enable = 0x128;
constexpr std::uint32_t global_interrupt_bit = 1U << 31U;

}  // namespace

AxiGpio::AxiGpio(
    RegisterAccess& registers,
    std::uint32_t base_address,
    bool dual_channel)
    : IpDriverLite(registers, base_address, 0x12c),
      dual_channel_(dual_channel) {}

std::uint32_t AxiGpio::readData(AxiGpioChannel channel) const {
    return read32(dataOffset(channel));
}

void AxiGpio::writeData(AxiGpioChannel channel, std::uint32_t value) {
    write32(dataOffset(channel), value);
}

std::uint32_t AxiGpio::direction(AxiGpioChannel channel) const {
    return read32(directionOffset(channel));
}

void AxiGpio::setDirection(AxiGpioChannel channel, std::uint32_t input_mask) {
    write32(directionOffset(channel), input_mask);
}

std::uint32_t AxiGpio::updateData(
    AxiGpioChannel channel,
    std::uint32_t mask,
    std::uint32_t value) {
    return update32(dataOffset(channel), mask, value);
}

bool AxiGpio::globalInterruptEnabled() const {
    return (read32(global_interrupt_enable) & global_interrupt_bit) != 0U;
}

void AxiGpio::setGlobalInterruptEnabled(bool enabled) {
    write32(global_interrupt_enable, enabled ? global_interrupt_bit : 0U);
}

std::uint32_t AxiGpio::interruptEnableMask() const {
    const std::uint32_t supported =
        channel1_interrupt | (dual_channel_ ? channel2_interrupt : 0U);
    return read32(interrupt_enable) & supported;
}

void AxiGpio::setInterruptEnableMask(std::uint32_t channel_mask) {
    write32(interrupt_enable, validateInterruptMask(channel_mask));
}

std::uint32_t AxiGpio::interruptStatus() const {
    const std::uint32_t supported =
        channel1_interrupt | (dual_channel_ ? channel2_interrupt : 0U);
    return read32(interrupt_status) & supported;
}

void AxiGpio::clearInterrupts(std::uint32_t channel_mask) {
    // IPISR is read/toggle-on-write. Writing the requested mask directly
    // would set currently inactive bits, so toggle only bits that were
    // observed pending in this read-modify-write transaction.
    const std::uint32_t selected = validateInterruptMask(channel_mask);
    const std::uint32_t pending = interruptStatus();
    const std::uint32_t clear_mask = pending & selected;
    if (clear_mask != 0U) write32(interrupt_status, clear_mask);
}

bool AxiGpio::hasChannel2() const noexcept { return dual_channel_; }

std::uint32_t AxiGpio::dataOffset(AxiGpioChannel channel) const {
    if (channel == AxiGpioChannel::channel1) return channel1_data;
    if (channel == AxiGpioChannel::channel2 && dual_channel_) return channel2_data;
    throw std::out_of_range("AXI GPIO channel is not present in this IP instance");
}

std::uint32_t AxiGpio::directionOffset(AxiGpioChannel channel) const {
    if (channel == AxiGpioChannel::channel1) return channel1_direction;
    if (channel == AxiGpioChannel::channel2 && dual_channel_) return channel2_direction;
    throw std::out_of_range("AXI GPIO channel is not present in this IP instance");
}

std::uint32_t AxiGpio::validateInterruptMask(std::uint32_t channel_mask) const {
    const std::uint32_t supported =
        channel1_interrupt | (dual_channel_ ? channel2_interrupt : 0U);
    if ((channel_mask & ~supported) != 0U) {
        throw std::invalid_argument("AXI GPIO interrupt mask selects an unavailable channel");
    }
    return channel_mask & supported;
}

}  // namespace mrs::fpga
