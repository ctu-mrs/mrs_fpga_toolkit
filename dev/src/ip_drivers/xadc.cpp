/**
 * @file xadc.cpp
 * @brief IP-driver implementation for XADC samples and engineering units.
 */

#include "mrs_fpga_dev/ip_drivers/xadc.h"

namespace mrs::fpga {
namespace {

// PG091 maps XADC hard-macro register 0x00 (temperature) into the AXI4-Lite
// window at +0x200. Each following hard-macro register is spaced by four
// bytes even though the conversion result occupies only bits 15:4.
constexpr std::uint32_t temperature = 0x200;
constexpr std::uint32_t vccint = 0x204;
constexpr std::uint32_t vccaux = 0x208;

}  // namespace

Xadc::Xadc(RegisterAccess& registers, std::uint32_t base_address)
    : IpDriverLite(registers, base_address, 0x300) {}

double Xadc::sample(std::uint32_t raw) {
    return static_cast<double>((raw >> 4U) & 0x0fffU);
}

double Xadc::temperatureCelsius() const {
    return sample(read32(temperature)) * 503.975 / 4096.0 - 273.15;
}

double Xadc::vccintVolts() const {
    return sample(read32(vccint)) * 3.0 / 4096.0;
}

double Xadc::vccauxVolts() const {
    return sample(read32(vccaux)) * 3.0 / 4096.0;
}

}  // namespace mrs::fpga
