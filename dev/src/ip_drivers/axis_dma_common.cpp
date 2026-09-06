/**
 * @file axis_dma_common.cpp
 * @brief Shared control/status decoding for the streaming DMA IP drivers.
 */

#include "mrs_fpga_dev/ip_drivers/axis_dma_common.h"

namespace mrs::fpga {

AxisDmaStatus decodeAxisDmaStatus(std::uint32_t raw) {
    return {
        (raw & axis_dma_register::busy) != 0U,
        (raw & axis_dma_register::error) != 0U,
        (raw & axis_dma_register::complete) != 0U,
        (raw & axis_dma_register::continuous) != 0U,
        (raw & axis_dma_register::no_increment) == 0U,
        (raw >> 16U) & 0x1fU,
        raw,
    };
}

}  // namespace mrs::fpga
