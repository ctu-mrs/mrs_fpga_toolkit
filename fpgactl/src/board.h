/**
 * @file board.h
 * @brief Command interface for image-specific board identity and telemetry.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace fpgactl {

/** Supported board identity and telemetry views. */
enum class BoardAction {
    show,
    name,
    version,
    dna,
    temperature,
    voltages,
};

/** Register-bank placement used by the supported FPGA image layout. */
struct BoardLayout {
    std::uint32_t gpio_base = 0x0000;
    std::uint32_t dna_base = 0x1000;
    std::uint32_t xadc_base = 0x3000;
};

/** Fully parsed parameters for one board command. */
struct BoardCommand {
    BoardAction action = BoardAction::show;
    std::optional<std::string> device;
    int device_index = 0;
    BoardLayout layout;
    bool json = false;
};

/** Execute one parsed board identity or telemetry command. */
int runBoard(const BoardCommand& command);

}  // namespace fpgactl
