/**
 * @file device_dna.cpp
 * @brief IP-driver implementation for Device DNA register composition.
 */

#include "mrs_fpga_dev/ip_drivers/device_dna.h"

namespace mrs::fpga {

DeviceDna::DeviceDna(RegisterAccess& registers, std::uint32_t base_address)
    : IpDriverLite(registers, base_address, 0x0c) {}

std::uint64_t DeviceDna::value() const {
    return read64(0x00, 0x08);
}

}  // namespace mrs::fpga
