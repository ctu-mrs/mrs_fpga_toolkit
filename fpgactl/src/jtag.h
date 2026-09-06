/**
 * @file jtag.h
 * @brief Physical USB-JTAG command dispatcher.
 */

#pragma once

#include "pcie.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fpgactl {

/** Read-only inspection or one of the two programming paths. */
enum class JtagAction {
    show,
    program_direct,
    program_flash,
};

/** Fully parsed USB-JTAG transport, target, and operation parameters. */
struct JtagCommand {
    JtagAction action = JtagAction::show;
    std::optional<std::string> image;
    std::string loader = "openFPGALoader";
    std::string cable = "digilent_hs2";
    unsigned int frequency = 15'000'000;
    std::optional<std::string> serial;
    std::vector<std::string> loader_arguments;
    std::optional<std::string> file_type;
    std::uint64_t offset = 0;
    std::string fpga_part = "xc7a200tfbg484";
    std::optional<std::uint32_t> expected_idcode = 0x03636093;
    std::uint32_t idcode_mask = 0x0fffffff;
    bool confirmed = false;
    bool reload = false;
    PcieOptions pcie;
};

/** Execute one parsed JTAG command. */
int runJtag(const JtagCommand& command);

}  // namespace fpgactl
