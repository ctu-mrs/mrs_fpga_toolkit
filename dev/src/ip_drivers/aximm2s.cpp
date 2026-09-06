/**
 * @file aximm2s.cpp
 * @brief IP-driver implementation for aligned, deadline-bounded DDR-to-C2H transfers.
 */

#include "mrs_fpga_dev/ip_drivers/aximm2s.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace mrs::fpga {
namespace reg = axis_dma_register;

AxiMm2s::AxiMm2s(
    RegisterAccess& registers,
    XDMAStream& stream,
    std::uint32_t base_address,
    std::size_t transport_beat_bytes)
    : IpDriverLite(registers, base_address, 0x20),
      stream_(stream),
      transport_beat_bytes_(transport_beat_bytes) {
    if (!stream_.hasC2H()) {
        throw std::invalid_argument("aximm2s requires an open C2H stream");
    }
    if (transport_beat_bytes_ < sizeof(std::uint32_t) ||
        (transport_beat_bytes_ % sizeof(std::uint32_t)) != 0U) {
        throw std::invalid_argument(
            "aximm2s transport beat must be a positive multiple of 32 bits");
    }
}

std::vector<std::uint8_t> AxiMm2s::read(
    std::uint64_t address,
    std::size_t byte_count,
    std::chrono::milliseconds timeout,
    std::size_t max_bytes_per_read,
    AxiMm2sStatistics* statistics) {
    if (timeout.count() <= 0) {
        throw std::invalid_argument("aximm2s timeout must be positive");
    }
    if (byte_count == 0) {
        if (statistics != nullptr) {
            *statistics = {};
        }
        return {};
    }
    const std::size_t logical_bytes = byte_count;
    if (logical_bytes > std::numeric_limits<std::size_t>::max() - transport_beat_bytes_ + 1U) {
        throw std::length_error("aximm2s aligned transfer size overflows");
    }
    const std::size_t transport_bytes =
        ((logical_bytes + transport_beat_bytes_ - 1U) / transport_beat_bytes_) *
        transport_beat_bytes_;
    if (transport_bytes > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("aximm2s transfer exceeds its 32-bit byte-count register");
    }

    if (max_bytes_per_read >
        std::numeric_limits<std::size_t>::max() - transport_beat_bytes_ + 1U) {
        throw std::length_error("aximm2s maximum read size overflows during alignment");
    }
    const std::size_t chunk_bytes = std::max(
        transport_beat_bytes_,
        ((max_bytes_per_read + transport_beat_bytes_ - 1U) / transport_beat_bytes_) *
            transport_beat_bytes_);
    std::vector<std::uint8_t> result(transport_bytes);
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    write64(reg::address_low, address);
    write64(reg::length_low, transport_bytes);
    write32(reg::control, reg::start);

    AxiMm2sStatistics measured;
    measured.logical_bytes = logical_bytes;
    measured.transport_bytes = transport_bytes;
    measured.smallest_read = std::numeric_limits<std::size_t>::max();
    auto* output = result.data();
    std::size_t completed = 0;
    while (completed < transport_bytes) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            throw XDMATimeout("aximm2s deadline expired while collecting C2H data");
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int timeout_ms = static_cast<int>(std::clamp<long long>(
            remaining.count() + 1, 1, std::numeric_limits<int>::max()));
        const std::size_t requested = std::min(chunk_bytes, transport_bytes - completed);
        const auto read_started = std::chrono::steady_clock::now();
        const ssize_t count = stream_.readC2H(output + completed, requested, timeout_ms);
        measured.blocked_time += std::chrono::steady_clock::now() - read_started;
        if (count == 0) {
            continue;
        }
        if (count < 0 || static_cast<std::size_t>(count) > requested) {
            throw std::runtime_error("aximm2s C2H returned an invalid fragment size");
        }
        const std::size_t returned = static_cast<std::size_t>(count);
        if (returned < requested) {
            ++measured.short_reads;
        }
        measured.smallest_read = std::min(measured.smallest_read, returned);
        measured.largest_read = std::max(measured.largest_read, returned);
        ++measured.read_calls;
        completed += returned;
    }

    std::uint32_t final_control = 0;
    if (!waitMasked(reg::control, reg::busy, 0, deadline, std::chrono::milliseconds(1),
                    &final_control)) {
        throw XDMATimeout("aximm2s did not complete before its transfer deadline");
    }
    const AxisDmaStatus final_status = status();
    if (final_status.error || !final_status.complete) {
        throw std::runtime_error(
            "aximm2s completed with control/status 0x" + std::to_string(final_control));
    }

    if (measured.read_calls == 0) {
        measured.smallest_read = 0;
    }
    result.resize(byte_count);
    if (statistics != nullptr) {
        *statistics = measured;
    }
    return result;
}

std::vector<std::uint64_t> AxiMm2s::readWords(
    std::uint64_t address,
    std::size_t word_count,
    std::chrono::milliseconds timeout,
    std::size_t max_words_per_read,
    AxiMm2sStatistics* statistics) {
    if (word_count > std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t) ||
        max_words_per_read > std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t)) {
        throw std::length_error("aximm2s word count exceeds the host size range");
    }
    std::vector<std::uint8_t> bytes = read(
        address,
        word_count * sizeof(std::uint64_t),
        timeout,
        max_words_per_read * sizeof(std::uint64_t),
        statistics);
    std::vector<std::uint64_t> words(word_count);
    if (!bytes.empty()) {
        std::memcpy(words.data(), bytes.data(), bytes.size());
    }
    return words;
}

AxisDmaStatus AxiMm2s::status() const {
    return decodeAxisDmaStatus(read32(reg::control));
}

std::size_t AxiMm2s::transportBeatBytes() const noexcept {
    return transport_beat_bytes_;
}

}  // namespace mrs::fpga
