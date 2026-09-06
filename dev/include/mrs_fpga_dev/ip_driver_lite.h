/**
 * @file ip_driver_lite.h
 * @brief Reusable base class for register-mapped AXI-Lite IP drivers.
 */

#pragma once

#include "mrs_fpga_dev/export.h"
#include "mrs_fpga_dev/xdma.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace mrs::fpga {

/**
 * Easy-to-use base for one IP register bank in an AXI-Lite address space.
 *
 * Derived drivers use offsets local to their IP.  The class validates
 * alignment and span, provides ordered 32/64-bit operations, and supplies a
 * bounded polling helper suitable for status bits. Any RegisterAccess
 * implementation may provide the transport; the IP driver is not coupled to
 * XDMA or to a particular board-level address map.
 */
class MRS_FPGA_API IpDriverLite {
public:
    IpDriverLite(
        RegisterAccess& registers,
        std::uint32_t base_address,
        std::uint32_t register_span = 0x1000);
    virtual ~IpDriverLite() = default;

    /** Read a 32-bit register at the supplied block-relative offset. */
    std::uint32_t read32(std::uint32_t offset) const;

    /** Write a 32-bit register at the supplied block-relative offset. */
    void write32(std::uint32_t offset, std::uint32_t value);

    /** Read a little-endian 64-bit register stored in adjacent low/high words. */
    std::uint64_t read64(std::uint32_t low_offset) const;

    /** Read a little-endian 64-bit value from explicitly located words. */
    std::uint64_t read64(
        std::uint32_t low_offset, std::uint32_t high_offset) const;

    /**
     * Read a free-running split counter without accepting a rollover tear.
     *
     * The high word is sampled before and after the low word and the sequence
     * is retried until both high samples agree.
     */
    std::uint64_t read64Stable(
        std::uint32_t low_offset, std::uint32_t high_offset) const;

    /** Write a little-endian 64-bit register as low word followed by high word. */
    void write64(std::uint32_t low_offset, std::uint64_t value);

    /** Write a little-endian 64-bit value to explicitly located words. */
    void write64(
        std::uint32_t low_offset,
        std::uint32_t high_offset,
        std::uint64_t value);

    /** Replace only bits selected by mask and return the resulting value. */
    std::uint32_t update32(
        std::uint32_t offset, std::uint32_t mask, std::uint32_t value);

    /**
     * Poll until `(read32(offset) & mask) == expected` or the deadline passes.
     *
     * The return value is false on timeout.  The final register value can be
     * requested without performing a second MMIO read.
     */
    bool waitMasked(
        std::uint32_t offset,
        std::uint32_t mask,
        std::uint32_t expected,
        std::chrono::steady_clock::time_point deadline,
        std::chrono::microseconds poll_interval = std::chrono::microseconds(50),
        std::uint32_t* final_value = nullptr) const;

    std::uint32_t baseAddress() const noexcept;
    std::uint32_t registerSpan() const noexcept;

    /**
     * Select a different aligned register-bank base while retaining the span.
     *
     * This is intended for drivers whose generated configuration is supplied
     * by a separate initialize step. Derived classes should rebase before
     * their first register access and should not change the base concurrently
     * with I/O from another thread.
     */
    void setBaseAddress(std::uint32_t base_address);

protected:
    RegisterAccess& registers() noexcept;
    const RegisterAccess& registers() const noexcept;

private:
    void validateOffset(std::uint32_t offset, std::uint32_t width) const;

    RegisterAccess& registers_;
    std::uint32_t base_address_;
    std::uint32_t register_span_;
};

}  // namespace mrs::fpga
