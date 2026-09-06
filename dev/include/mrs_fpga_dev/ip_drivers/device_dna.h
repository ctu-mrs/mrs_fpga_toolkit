/**
 * @file device_dna.h
 * @brief Driver for a 7-series Device DNA AXI register wrapper.
 */

#pragma once

#include "mrs_fpga_dev/export.h"
#include "mrs_fpga_dev/ip_driver_lite.h"

#include <cstdint>

namespace mrs::fpga {

/**
 * Read a 7-series Device DNA value exposed by an AXI register wrapper.
 *
 * AMD UG470 defines the 7-series DNA_PORT value as 57 bits. The FPGA wrapper
 * publishes that value in two read-only 32-bit words at offsets 0x00 and 0x08;
 * this driver composes them into a zero-extended 64-bit host value. The AXI
 * wrapper layout is intentionally distinct from the serial DNA_PORT primitive
 * protocol used inside the FPGA.
 */
class MRS_FPGA_API DeviceDna final : public IpDriverLite {
public:
    static constexpr std::uint32_t default_base_address = 0x1000;

    explicit DeviceDna(
        RegisterAccess& registers,
        std::uint32_t base_address = default_base_address);

    /** Return the FPGA-provided DNA value as an unsigned host integer. */
    std::uint64_t value() const;
};

}  // namespace mrs::fpga
