/**
 * @file axis_dma_common.h
 * @brief Shared wb2axip AXI stream/memory DMA register definitions.
 */

#pragma once

#include "mrs_fpga_dev/export.h"

#include <cstdint>

namespace mrs::fpga {

/** Decoded control/status word common to aximm2s and axis2mm. */
struct AxisDmaStatus {
    bool busy = false;
    bool error = false;
    bool complete = false;
    bool continuous = false;
    bool increment = true;
    std::uint32_t fifo_depth_log2 = 0;
    std::uint32_t raw = 0;
};

/** Decode the control/status layout shared by both streaming DMA directions. */
MRS_FPGA_API AxisDmaStatus decodeAxisDmaStatus(std::uint32_t raw);

namespace axis_dma_register {
inline constexpr std::uint32_t control = 0x00;
inline constexpr std::uint32_t address_low = 0x08;
inline constexpr std::uint32_t address_high = 0x0c;
inline constexpr std::uint32_t length_low = 0x18;
inline constexpr std::uint32_t length_high = 0x1c;
inline constexpr std::uint32_t start = 1U << 31U;
inline constexpr std::uint32_t busy = 1U << 31U;
inline constexpr std::uint32_t error = 1U << 30U;
inline constexpr std::uint32_t complete = 1U << 29U;
inline constexpr std::uint32_t continuous = 1U << 28U;
inline constexpr std::uint32_t no_increment = 1U << 27U;
}  // namespace axis_dma_register

}  // namespace mrs::fpga
