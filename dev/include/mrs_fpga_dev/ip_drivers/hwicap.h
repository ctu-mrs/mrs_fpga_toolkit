/**
 * @file hwicap.h
 * @brief Safe AXI HWICAP control for warm boot from configuration flash.
 */

#pragma once

#include "mrs_fpga_dev/export.h"
#include "mrs_fpga_dev/ip_driver_lite.h"

#include <chrono>
#include <cstdint>

namespace mrs::fpga {

/**
 * Validated AXI HWICAP access for an atomic 7-series SPI IPROG command.
 *
 * Register offsets, DONE/EOS status, write-FIFO vacancy, and control bits
 * follow AMD AXI HWICAP Product Guide PG134 v3.0. The configuration packet
 * sequence and WBSTAR address encoding follow the 7 Series Configuration User
 * Guide UG470. The default vacancy of 63 corresponds to a core customized
 * with a 64-word write FIFO; another synthesized depth can be supplied to the
 * constructor.
 */
class MRS_FPGA_API AxiHwIcap final : public IpDriverLite {
public:
    static constexpr std::uint32_t default_base_address = 0x30000;
    static constexpr std::uint32_t default_fifo_vacancy = 63;
    /** Largest aligned byte address encodable by 7-series 24-bit SPI mode. */
    static constexpr std::uint32_t maximum_flash_address = 0x00ffff00;

    AxiHwIcap(
        RegisterAccess& registers,
        std::uint32_t base_address = default_base_address,
        std::chrono::milliseconds timeout = std::chrono::seconds(1),
        std::uint32_t expected_fifo_vacancy = default_fifo_vacancy);

    /** Check DONE/EOS, reserved control bits, and FIFO vacancy. */
    void validate() const;

    /**
     * Start IPROG at a 256-byte-aligned 24-bit SPI flash byte address.
     *
     * UG470 maps byte-address bits 23:8 to WBSTAR[15:0] in 24-bit SPI address
     * mode. Consequently the target image must begin with the required 256
     * dummy bytes and the low address byte cannot be selected independently.
     *
     * A successful call intentionally makes the current PCIe BAR disappear;
     * callers must recover the endpoint through PCIe configuration space.
     */
    void triggerIprog(std::uint32_t flash_address);

private:
    void requireReady() const;
    void waitIdle() const;
    void reset();

    std::chrono::milliseconds timeout_;
    std::uint32_t expected_fifo_vacancy_;
};

}  // namespace mrs::fpga
