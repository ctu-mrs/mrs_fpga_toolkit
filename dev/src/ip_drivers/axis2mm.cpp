/**
 * @file axis2mm.cpp
 * @brief IP-driver implementation for packet-safe H2C-to-DDR transfers.
 */

#include "mrs_fpga_dev/ip_drivers/axis2mm.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace mrs::fpga {
namespace reg = axis_dma_register;
namespace {
constexpr std::uint32_t tlast_not_synchronized = 1U << 26U;
constexpr std::uint32_t decode_error = 1U << 25U;
constexpr std::uint32_t slave_error = 1U << 24U;
constexpr std::uint32_t overflow_error = 1U << 23U;
constexpr std::uint32_t abort_in_progress = 1U << 22U;

/** Convert the remaining common transfer deadline to the stream API's unit. */
int remainingMilliseconds(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        throw XDMATimeout("axis2mm deadline expired before the H2C write started");
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return static_cast<int>(std::clamp<long long>(
        remaining.count() + 1, 1, std::numeric_limits<int>::max()));
}
}  // namespace

Axis2Mm::Axis2Mm(
    RegisterAccess& registers,
    XDMAStream& stream,
    std::uint32_t base_address,
    std::size_t transport_beat_bytes)
    : IpDriverLite(registers, base_address, 0x20),
      stream_(stream),
      transport_beat_bytes_(transport_beat_bytes) {
    if (!stream_.hasH2C()) {
        throw std::invalid_argument("axis2mm requires an open H2C stream");
    }
    if (transport_beat_bytes_ == 0U || (transport_beat_bytes_ & 0x3U) != 0U) {
        throw std::invalid_argument("axis2mm transport beat must be a positive multiple of 4 bytes");
    }
}

void Axis2Mm::write(
    std::uint64_t address,
    const void* data,
    std::size_t byte_count,
    std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0) {
        throw std::invalid_argument("axis2mm timeout must be positive");
    }
    if (byte_count == 0U || (byte_count % transport_beat_bytes_) != 0U) {
        throw std::invalid_argument(
            "axis2mm byte count must be nonzero and aligned to the transport beat");
    }
    if (byte_count > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("axis2mm transfer exceeds its 32-bit byte-count register");
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    write64(reg::address_low, address);
    write64(reg::length_low, byte_count);
    write32(reg::control, reg::start);
    stream_.writeH2C(data, byte_count, remainingMilliseconds(deadline));

    if (!waitMasked(reg::control, reg::busy, 0, deadline, std::chrono::milliseconds(1))) {
        throw XDMATimeout("axis2mm did not complete before its transfer deadline");
    }
    const Axis2MmStatus final_status = status();
    if (final_status.error || !final_status.complete || !final_status.tlast_synchronized ||
        final_status.decode_error || final_status.slave_error ||
        final_status.overflow_error || final_status.abort_in_progress) {
        throw std::runtime_error(
            "axis2mm completed with an error, incomplete packet, or invalid status");
    }
}

Axis2MmStatus Axis2Mm::status() const {
    const std::uint32_t raw = read32(reg::control);
    Axis2MmStatus result;
    static_cast<AxisDmaStatus&>(result) = decodeAxisDmaStatus(raw);
    result.tlast_synchronized = (raw & tlast_not_synchronized) == 0U;
    result.decode_error = (raw & decode_error) != 0U;
    result.slave_error = (raw & slave_error) != 0U;
    result.overflow_error = (raw & overflow_error) != 0U;
    result.abort_in_progress = (raw & abort_in_progress) != 0U;
    return result;
}

}  // namespace mrs::fpga
