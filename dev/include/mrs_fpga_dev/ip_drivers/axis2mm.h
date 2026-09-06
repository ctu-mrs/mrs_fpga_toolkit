/**
 * @file axis2mm.h
 * @brief Driver for a wb2axip AXI stream-to-memory writer.
 */

#pragma once

#include "mrs_fpga_dev/ip_driver_lite.h"
#include "mrs_fpga_dev/ip_drivers/axis_dma_common.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mrs::fpga {

/** axis2mm-specific error state in addition to common DMA status. */
struct Axis2MmStatus : AxisDmaStatus {
    bool tlast_synchronized = false;
    bool decode_error = false;
    bool slave_error = false;
    bool overflow_error = false;
    bool abort_in_progress = false;
};

/**
 * Program axis2mm and submit one H2C packet to board memory.
 *
 * The byte count must be a multiple of the configured transport beat.  This
 * makes packet termination explicit and prevents accidental padding from
 * becoming application data in DDR.
 */
class MRS_FPGA_API Axis2Mm final : public IpDriverLite {
public:
    static constexpr std::uint32_t default_base_address = 0x5000;

    Axis2Mm(
        RegisterAccess& registers,
        XDMAStream& stream,
        std::uint32_t base_address = default_base_address,
        std::size_t transport_beat_bytes = sizeof(std::uint64_t));

    /** Write a complete aligned byte buffer and wait for hardware completion. */
    void write(
        std::uint64_t address,
        const void* data,
        std::size_t byte_count,
        std::chrono::milliseconds timeout = std::chrono::seconds(3));

    /** Type-safe vector overload for write(). */
    template<typename T>
    void write(
        std::uint64_t address,
        const std::vector<T>& data,
        std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        write(address, data.data(), data.size() * sizeof(T), timeout);
    }

    /** Decode common and axis2mm-specific status bits. */
    Axis2MmStatus status() const;

private:
    XDMAStream& stream_;
    std::size_t transport_beat_bytes_;
};

}  // namespace mrs::fpga
