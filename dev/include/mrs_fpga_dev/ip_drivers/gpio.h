/**
 * @file gpio.h
 * @brief Generic driver for the AMD/Xilinx AXI GPIO register interface.
 */

#pragma once

#include "mrs_fpga_dev/export.h"
#include "mrs_fpga_dev/ip_driver_lite.h"

#include <cstdint>

namespace mrs::fpga {

/** Select one of the two channels that an AXI GPIO instance may expose. */
enum class AxiGpioChannel : std::uint8_t {
    channel1 = 1,
    channel2 = 2,
};

/**
 * Register-level access to one AXI GPIO IP instance.
 *
 * Register offsets and bit semantics follow AMD AXI GPIO Product Guide PG144
 * v2.0. DATA contains the GPIO value and TRI uses one bits for inputs and zero
 * bits for outputs. The IP interrupt status register uses toggle-on-write
 * semantics, which the driver's clearing helper handles without toggling an
 * interrupt that was not pending when the operation began.
 *
 * AXI GPIO is configurable as either a single-channel or dual-channel IP. A
 * single channel is the AMD customization default. Enabling the synthesized
 * dual-channel option adds the channel-2 DATA/TRI registers and interrupt bit.
 * The driver deliberately assigns no application meaning to either channel.
 */
class MRS_FPGA_API AxiGpio final : public IpDriverLite {
public:
    static constexpr std::uint32_t default_base_address = 0x0000;
    static constexpr std::uint32_t channel1_interrupt = 1U << 0U;
    static constexpr std::uint32_t channel2_interrupt = 1U << 1U;

    /**
     * Construct a driver for a single- or dual-channel AXI GPIO instance.
     *
     * @param registers Underlying 32-bit AXI-Lite register transport.
     * @param base_address Absolute user-BAR address of the IP register bank.
     * @param dual_channel Whether channel 2 exists in the synthesized IP.
     */
    explicit AxiGpio(
        RegisterAccess& registers,
        std::uint32_t base_address = default_base_address,
        bool dual_channel = false);

    /** Read the DATA register for the selected channel. */
    std::uint32_t readData(AxiGpioChannel channel) const;

    /** Write the DATA register for the selected channel. */
    void writeData(AxiGpioChannel channel, std::uint32_t value);

    /** Read the TRI direction mask; one denotes input and zero denotes output. */
    std::uint32_t direction(AxiGpioChannel channel) const;

    /** Set the TRI direction mask; one denotes input and zero denotes output. */
    void setDirection(AxiGpioChannel channel, std::uint32_t input_mask);

    /** Update selected DATA bits while preserving all bits outside mask. */
    std::uint32_t updateData(
        AxiGpioChannel channel,
        std::uint32_t mask,
        std::uint32_t value);

    /** Return whether the AXI GPIO global interrupt gate is enabled. */
    bool globalInterruptEnabled() const;

    /** Enable or disable the AXI GPIO global interrupt gate. */
    void setGlobalInterruptEnabled(bool enabled);

    /** Read the enabled channel-interrupt mask. */
    std::uint32_t interruptEnableMask() const;

    /** Enable channel interrupts selected by channel1_interrupt/channel2_interrupt. */
    void setInterruptEnableMask(std::uint32_t channel_mask);

    /** Read pending channel interrupts. */
    std::uint32_t interruptStatus() const;

    /**
     * Clear selected pending channel interrupts.
     *
     * PG144 defines IPISR as read/toggle-on-write rather than write-one-to-
     * clear. The implementation first reads IPISR and writes only selected
     * bits observed as pending, preventing clear requests for inactive bits
     * from creating false pending status.
     */
    void clearInterrupts(std::uint32_t channel_mask);

    /** Return whether this synthesized instance exposes channel 2. */
    bool hasChannel2() const noexcept;

private:
    std::uint32_t dataOffset(AxiGpioChannel channel) const;
    std::uint32_t directionOffset(AxiGpioChannel channel) const;
    std::uint32_t validateInterruptMask(std::uint32_t channel_mask) const;

    bool dual_channel_;
};

}  // namespace mrs::fpga
