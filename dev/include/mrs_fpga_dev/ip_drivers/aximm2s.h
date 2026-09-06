/**
 * @file aximm2s.h
 * @brief Driver for a wb2axip AXI memory-to-stream reader.
 */

#pragma once

#include "mrs_fpga_dev/ip_driver_lite.h"
#include "mrs_fpga_dev/ip_drivers/axis_dma_common.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mrs::fpga {

/** Transfer counters returned by the detailed aximm2s read interface. */
struct AxiMm2sStatistics {
    std::size_t logical_bytes = 0;
    std::size_t transport_bytes = 0;
    std::size_t read_calls = 0;
    std::size_t short_reads = 0;
    std::size_t smallest_read = 0;
    std::size_t largest_read = 0;
    std::chrono::nanoseconds blocked_time{};
};

/**
 * Program aximm2s and collect its C2H stream.
 *
 * transport_beat_bytes describes the final stream width presented to XDMA.
 * Logical transfers are padded to that width and padding is removed from the
 * returned vector.  This handles a native 64-bit path directly and is also
 * essential for a 64-to-128-bit stream converter, where an odd number of
 * 64-bit words must request one complete 16-byte beat.
 */
class MRS_FPGA_API AxiMm2s final : public IpDriverLite {
public:
    static constexpr std::uint32_t default_base_address = 0x4000;

    AxiMm2s(
        RegisterAccess& registers,
        XDMAStream& stream,
        std::uint32_t base_address = default_base_address,
        std::size_t transport_beat_bytes = sizeof(std::uint64_t));

    /**
     * Read an arbitrary logical byte range and discard final beat padding.
     *
     * max_bytes_per_read is rounded up to a whole transport beat.  It permits
     * both single-read transfers and the bounded chunked pattern used for
     * sustained streams.
     */
    std::vector<std::uint8_t> read(
        std::uint64_t address,
        std::size_t byte_count,
        std::chrono::milliseconds timeout = std::chrono::seconds(3),
        std::size_t max_bytes_per_read = 32 * 1024,
        AxiMm2sStatistics* statistics = nullptr);

    /** Read 64-bit words with one timeout covering setup, C2H, and completion. */
    std::vector<std::uint64_t> readWords(
        std::uint64_t address,
        std::size_t word_count,
        std::chrono::milliseconds timeout = std::chrono::seconds(3),
        std::size_t max_words_per_read = 4096,
        AxiMm2sStatistics* statistics = nullptr);

    /** Decode the current DMA control/status register. */
    AxisDmaStatus status() const;

    /** Expose the configured native stream beat size. */
    std::size_t transportBeatBytes() const noexcept;

private:
    XDMAStream& stream_;
    std::size_t transport_beat_bytes_;
};

}  // namespace mrs::fpga
