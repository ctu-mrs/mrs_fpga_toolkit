/**
 * @file xadc.h
 * @brief AMD XADC Wizard sensor-register driver and unit conversion.
 */

#pragma once

#include "mrs_fpga_dev/export.h"
#include "mrs_fpga_dev/ip_driver_lite.h"

#include <cstdint>

namespace mrs::fpga {

/**
 * Read the standard on-chip sensor registers exposed by XADC Wizard.
 *
 * Register locations and 12-bit MSB-justified sample encoding follow AMD
 * XADC Wizard Product Guide PG091 v3.3. The conversion equations are the
 * documented 7-series XADC equations for temperature and supply voltages.
 */
class MRS_FPGA_API Xadc final : public IpDriverLite {
public:
    /** Default base of the complete XADC Wizard AXI4-Lite register bank. */
    static constexpr std::uint32_t default_base_address = 0x3000;

    /** Construct an XADC driver over the complete PG091 register bank. */
    explicit Xadc(
        RegisterAccess& registers,
        std::uint32_t base_address = default_base_address);

    /** Return the latest on-chip temperature in degrees Celsius. */
    double temperatureCelsius() const;

    /** Return the latest VCCINT supply measurement in volts. */
    double vccintVolts() const;

    /** Return the latest VCCAUX supply measurement in volts. */
    double vccauxVolts() const;

private:
    /** Extract the 12-bit MSB-justified ADC code from an AXI register word. */
    static double sample(std::uint32_t raw);
};

}  // namespace mrs::fpga
