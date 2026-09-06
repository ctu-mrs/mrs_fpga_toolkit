/**
 * @file qspi.h
 * @brief XDMA/AXI Quad SPI command dispatcher.
 */

#pragma once

#include "pcie.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace fpgactl {

/** Supported persistent-flash and warm-boot operations. */
enum class QspiAction {
    show,
    read,
    verify,
    program,
    repair,
    reload,
};

/** Fully parsed parameters for one QSPI command. */
struct QspiCommand {
    QspiAction action = QspiAction::show;
    std::optional<std::string> operand;
    std::optional<std::string> device;
    std::uint32_t qspi_base = 0x10000;
    std::chrono::milliseconds spi_timeout{1000};
    std::uint32_t hwicap_base = 0x30000;
    std::chrono::milliseconds hwicap_timeout{1000};
    std::uint32_t hwicap_vacancy = 63;
    std::uint64_t offset = 0;
    std::optional<std::uint64_t> length;
    std::optional<std::uint32_t> boot_address;
    std::optional<std::string> expected_part;
    bool confirmed = false;
    bool rewrite_all = false;
    bool force = false;
    bool json = false;
    bool reload_after = false;
    PcieOptions pcie;
};

/** Execute one parsed QSPI command. */
int runQspi(QspiCommand command);

}  // namespace fpgactl
