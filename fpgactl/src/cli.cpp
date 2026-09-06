/**
 * @file cli.cpp
 * @brief Declarative argparse command tree and typed command dispatch.
 */

#include "cli.h"

#include "board.h"
#include "common.h"
#include "jtag.h"
#include "pcie.h"
#include "qspi.h"

#include <argparse/argparse.hpp>

#include <chrono>
#include <climits>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef MRS_FPGACTL_VERSION
#define MRS_FPGACTL_VERSION "unknown"
#endif

namespace fpgactl {
namespace {

/** Return an explicitly supplied string option or the command's documented default. */
std::string optionOr(
    const argparse::ArgumentParser& parser,
    const std::string& name,
    const std::string& fallback) {
    return parser.present<std::string>(name).value_or(fallback);
}

/** Convert an optional numeric PCI identifier, accepting "none" as no filter. */
std::optional<std::uint16_t> parseOptionalId(
    const std::optional<std::string>& value, const std::string& name) {
    if (!value.has_value() || value->empty() || *value == "none") {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(parseUnsigned(*value, name, UINT16_MAX));
}

/** Convert a decimal-octal permission string or request preservation. */
std::optional<unsigned int> parseDeviceMode(const std::string& value) {
    if (value == "preserve" || value == "none") {
        return std::nullopt;
    }
    std::size_t consumed = 0;
    unsigned long mode = 0;
    try {
        mode = std::stoul(value, &consumed, 8);
    } catch (const std::exception&) {
        throw Error("device mode must be octal or 'preserve'");
    }
    if (consumed != value.size() || mode > 0777) {
        throw Error("device mode must be between 000 and 777");
    }
    return static_cast<unsigned int>(mode);
}

/** Add reusable PCIe endpoint and XDMA recovery arguments to one leaf command. */
void addPcieArguments(
    argparse::ArgumentParser& parser, bool include_json, bool include_all) {
    parser.add_argument("--bdf")
        .metavar("BDF")
        .help("select a PCI endpoint as [domain:]bus:device.function");
    parser.add_argument("--device-index")
        .metavar("N")
        .nargs(1)
        .help("select the XDMA device index (default: 0)");
    parser.add_argument("--vendor-id")
        .metavar("ID|none")
        .nargs(1)
        .help("filter by PCI vendor ID, or disable with none (default: 0x10ee)");
    parser.add_argument("--device-id")
        .metavar("ID")
        .help("filter by PCI device ID");
    parser.add_argument("--module")
        .metavar("NAME")
        .nargs(1)
        .help("kernel module used by the endpoint (default: xdma)");
    parser.add_argument("--module-args")
        .metavar("STRING")
        .nargs(1)
        .help("quote-aware arguments passed to modprobe (default: none)");
    parser.add_argument("--required-nodes")
        .metavar("LIST")
        .nargs(1)
        .help("comma/space-separated XDMA node suffixes to wait for (default: control)");
    parser.add_argument("--wait")
        .metavar("SECONDS")
        .nargs(1)
        .help("PCIe recovery timeout in seconds (default: 30)");
    parser.add_argument("--device-mode")
        .metavar("OCTAL|preserve")
        .nargs(1)
        .help("permissions applied to XDMA nodes (default: 666)");
    if (include_json) {
        parser.add_argument("--json").flag().help("emit machine-readable JSON");
    }
    if (include_all) {
        parser.add_argument("--all").flag().help("disable PCI vendor/device filtering");
    }
}

/** Materialize strongly typed endpoint options from an argparse leaf parser. */
PcieOptions parsePcieOptions(
    const argparse::ArgumentParser& parser, bool include_json, bool include_all) {
    PcieOptions result;
    result.bdf = parser.present<std::string>("--bdf");
    result.device_index = static_cast<int>(parseUnsigned(
        optionOr(parser, "--device-index", "0"), "device index", INT_MAX));
    result.vendor_id = parseOptionalId(
        optionOr(parser, "--vendor-id", "0x10ee"), "vendor ID");
    result.device_id = parseOptionalId(
        parser.present<std::string>("--device-id"), "device ID");
    result.module = optionOr(parser, "--module", "xdma");
    result.module_arguments = splitCommandArguments(
        optionOr(parser, "--module-args", ""));
    result.required_nodes = splitList(optionOr(parser, "--required-nodes", "control"));
    result.timeout = std::chrono::milliseconds(static_cast<long long>(
        parsePositiveSeconds(optionOr(parser, "--wait", "30"), "wait timeout") * 1000.0));
    result.device_mode = parseDeviceMode(optionOr(parser, "--device-mode", "666"));
    result.json = include_json && parser.get<bool>("--json");
    result.include_all = include_all && parser.get<bool>("--all");
    return result;
}

/** Add image-layout-specific board access arguments to a board leaf command. */
void addBoardArguments(argparse::ArgumentParser& parser, bool include_json) {
    parser.add_argument("--device")
        .metavar("PATH")
        .help("XDMA user BAR device (default: /dev/xdmaN_user)");
    parser.add_argument("--device-index")
        .metavar("N")
        .nargs(1)
        .help("XDMA device index used by the default path (default: 0)");
    parser.add_argument("--gpio-base")
        .metavar("OFFSET")
        .nargs(1)
        .help("dual-channel AXI GPIO base address (default: 0x0)");
    parser.add_argument("--dna-base")
        .metavar("OFFSET")
        .nargs(1)
        .help("Device DNA register wrapper base address (default: 0x1000)");
    parser.add_argument("--xadc-base")
        .metavar("OFFSET")
        .nargs(1)
        .help("XADC Wizard AXI base address (default: 0x3000)");
    if (include_json) {
        parser.add_argument("--json").flag().help("emit machine-readable JSON");
    }
}

/** Convert shared board arguments and the selected view into one command. */
BoardCommand parseBoardCommand(
    const argparse::ArgumentParser& parser, BoardAction action, bool include_json) {
    BoardCommand result;
    result.action = action;
    result.device = parser.present<std::string>("--device");
    result.device_index = static_cast<int>(parseUnsigned(
        optionOr(parser, "--device-index", "0"), "device index", INT_MAX));
    result.layout.gpio_base = static_cast<std::uint32_t>(parseUnsigned(
        optionOr(parser, "--gpio-base", "0x0"), "GPIO base", UINT32_MAX));
    result.layout.dna_base = static_cast<std::uint32_t>(parseUnsigned(
        optionOr(parser, "--dna-base", "0x1000"), "Device DNA base", UINT32_MAX));
    result.layout.xadc_base = static_cast<std::uint32_t>(parseUnsigned(
        optionOr(parser, "--xadc-base", "0x3000"), "XADC base", UINT32_MAX));
    result.json = include_json && parser.get<bool>("--json");
    return result;
}

/** Add AXI Quad SPI transport and device-selection options to a QSPI leaf. */
void addQspiArguments(argparse::ArgumentParser& parser) {
    addPcieArguments(parser, false, false);
    parser.add_argument("--device")
        .metavar("PATH")
        .help("XDMA user BAR device (default: /dev/xdmaN_user)");
    parser.add_argument("--qspi-base")
        .metavar("OFFSET")
        .nargs(1)
        .help("AXI Quad SPI base address (default: 0x10000)");
    parser.add_argument("--spi-timeout")
        .metavar("SECONDS")
        .nargs(1)
        .help("SPI controller and flash-operation timeout (default: 1)");
}

/** Add the AXI HWICAP parameters needed for a warm boot. */
void addHwIcapArguments(argparse::ArgumentParser& parser) {
    parser.add_argument("--hwicap-base")
        .metavar("OFFSET")
        .nargs(1)
        .help("AXI HWICAP base address (default: 0x30000)");
    parser.add_argument("--hwicap-timeout")
        .metavar("SECONDS")
        .nargs(1)
        .help("AXI HWICAP FIFO and completion timeout (default: 1)");
    parser.add_argument("--hwicap-vacancy")
        .metavar("WORDS")
        .nargs(1)
        .help("expected writable FIFO vacancy reported by AXI HWICAP (default: 63)");
    parser.add_argument("--boot-address")
        .metavar("OFFSET")
        .help("flash address written to the WBSTAR register");
}

/** Parse options that every QSPI operation shares. */
QspiCommand parseQspiBase(
    const argparse::ArgumentParser& parser, QspiAction action) {
    QspiCommand result;
    result.action = action;
    result.device = parser.present<std::string>("--device");
    result.qspi_base = static_cast<std::uint32_t>(parseUnsigned(
        optionOr(parser, "--qspi-base", "0x10000"), "QSPI base", UINT32_MAX));
    result.spi_timeout = std::chrono::milliseconds(static_cast<long long>(
        parsePositiveSeconds(
            optionOr(parser, "--spi-timeout", "1"), "SPI timeout") * 1000.0));
    result.pcie = parsePcieOptions(parser, false, false);
    return result;
}

/** Parse the optional warm-boot controller fields of a QSPI command. */
void parseHwIcapOptions(const argparse::ArgumentParser& parser, QspiCommand& result) {
    result.hwicap_base = static_cast<std::uint32_t>(parseUnsigned(
        optionOr(parser, "--hwicap-base", "0x30000"), "HWICAP base", UINT32_MAX));
    result.hwicap_timeout = std::chrono::milliseconds(static_cast<long long>(
        parsePositiveSeconds(
            optionOr(parser, "--hwicap-timeout", "1"), "HWICAP timeout") * 1000.0));
    result.hwicap_vacancy = static_cast<std::uint32_t>(parseUnsigned(
        optionOr(parser, "--hwicap-vacancy", "63"), "HWICAP vacancy", UINT32_MAX));
    if (const auto value = parser.present<std::string>("--boot-address")) {
        result.boot_address = static_cast<std::uint32_t>(
            parseUnsigned(*value, "boot address", UINT32_MAX));
    }
}

/** Add openFPGALoader transport and expected-target arguments. */
void addJtagTargetArguments(argparse::ArgumentParser& parser) {
    parser.add_argument("--loader")
        .metavar("PATH")
        .nargs(1)
        .help("openFPGALoader executable or path (default: openFPGALoader)");
    parser.add_argument("--cable")
        .metavar("NAME")
        .nargs(1)
        .help("openFPGALoader cable name (default: digilent_hs2)");
    parser.add_argument("--frequency")
        .metavar("HZ")
        .nargs(1)
        .help("JTAG clock frequency (default: 15000000)");
    parser.add_argument("--serial")
        .metavar("SERIAL")
        .help("select a cable by FTDI serial number");
    parser.add_argument("--loader-arg")
        .metavar("ARG")
        .append()
        .help("append an argument to openFPGALoader; may be repeated");
    parser.add_argument("--expected-idcode")
        .metavar("ID|none")
        .nargs(1)
        .help("masked JTAG IDCODE expected from the target (default: 0x03636093)");
    parser.add_argument("--idcode-mask")
        .metavar("MASK")
        .nargs(1)
        .help("bit mask used for the IDCODE comparison (default: 0x0fffffff)");
}

/** Parse shared JTAG transport and target verification options. */
JtagCommand parseJtagTarget(const argparse::ArgumentParser& parser) {
    JtagCommand result;
    result.loader = optionOr(parser, "--loader", "openFPGALoader");
    result.cable = optionOr(parser, "--cable", "digilent_hs2");
    result.frequency = static_cast<unsigned int>(parseUnsigned(
        optionOr(parser, "--frequency", "15000000"), "JTAG frequency", UINT_MAX));
    result.serial = parser.present<std::string>("--serial");
    if (const auto values = parser.present<std::vector<std::string>>("--loader-arg")) {
        result.loader_arguments = *values;
    }
    const std::string expected = optionOr(parser, "--expected-idcode", "0x03636093");
    if (expected == "none" || expected == "off" || expected == "disabled") {
        result.expected_idcode.reset();
    } else {
        result.expected_idcode = static_cast<std::uint32_t>(
            parseUnsigned(expected, "expected IDCODE", UINT32_MAX));
    }
    result.idcode_mask = static_cast<std::uint32_t>(parseUnsigned(
        optionOr(parser, "--idcode-mask", "0x0fffffff"), "IDCODE mask", UINT32_MAX));
    return result;
}

/** Print the generated help for an incomplete command family. */
int showHelp(const argparse::ArgumentParser& parser) {
    std::cout << parser;
    return 0;
}

}  // namespace

int runCli(int argc, char** argv) {
    argparse::ArgumentParser program("fpgactl", MRS_FPGACTL_VERSION);
    program.add_description(
        "Inspect, configure, and recover AMD/Xilinx FPGA devices through XDMA or USB-JTAG.");

    argparse::ArgumentParser xdma("xdma", MRS_FPGACTL_VERSION);
    xdma.add_description("PCIe, XDMA, board-register, and AXI-attached flash operations.");
    argparse::ArgumentParser xdma_show("show", MRS_FPGACTL_VERSION);
    xdma_show.add_description("Show the selected PCIe endpoint and its XDMA state.");
    addPcieArguments(xdma_show, true, false);
    argparse::ArgumentParser xdma_list("list", MRS_FPGACTL_VERSION);
    xdma_list.add_description("List PCIe endpoints matching the requested identifiers.");
    addPcieArguments(xdma_list, true, true);
    argparse::ArgumentParser xdma_driver("driver", MRS_FPGACTL_VERSION);
    xdma_driver.add_description("Control XDMA binding and PCIe endpoint recovery.");
    addPcieArguments(xdma_driver, false, false);
    auto& driver_action = xdma_driver.add_mutually_exclusive_group(true);
    driver_action.add_argument("--reload").flag().help(
        "remove and rescan the endpoint, then reload XDMA");
    driver_action.add_argument("--probe").flag().help(
        "rescan and bind XDMA without first removing the endpoint");
    driver_action.add_argument("--driver-only").flag().help(
        "reload only the kernel module");
    driver_action.add_argument("--post-reconfigure").flag().help(
        "recover PCIe and XDMA after whole-FPGA reconfiguration");
    driver_action.add_argument("--remove").flag().help(
        "unload XDMA and remove the endpoint without rescanning");
    argparse::ArgumentParser xdma_mrrs("mrrs", MRS_FPGACTL_VERSION);
    xdma_mrrs.add_description("Set PCIe Maximum Read Request Size with readback verification.");
    addPcieArguments(xdma_mrrs, false, false);
    xdma_mrrs.add_argument("--set")
        .metavar("BYTES")
        .required()
        .choices("128", "256", "512", "1024", "2048", "4096")
        .help("new MRRS value");

    argparse::ArgumentParser board("board", MRS_FPGACTL_VERSION);
    board.add_description("Read image-specific identity and telemetry registers.");
    argparse::ArgumentParser board_show("show", MRS_FPGACTL_VERSION);
    board_show.add_description("Show board identity, Device DNA, and telemetry.");
    addBoardArguments(board_show, true);
    argparse::ArgumentParser board_name("name", MRS_FPGACTL_VERSION);
    board_name.add_description("Print the product identifier from AXI GPIO channel 1.");
    addBoardArguments(board_name, false);
    argparse::ArgumentParser board_version("version", MRS_FPGACTL_VERSION);
    board_version.add_description("Print the image version from AXI GPIO channel 2.");
    addBoardArguments(board_version, false);
    argparse::ArgumentParser board_dna("dna", MRS_FPGACTL_VERSION);
    board_dna.add_description("Print the FPGA Device DNA value.");
    addBoardArguments(board_dna, false);
    argparse::ArgumentParser board_temperature("temperature", MRS_FPGACTL_VERSION);
    board_temperature.add_description("Print the XADC die temperature.");
    addBoardArguments(board_temperature, false);
    argparse::ArgumentParser board_voltages("voltages", MRS_FPGACTL_VERSION);
    board_voltages.add_description("Print the XADC VCCINT and VCCAUX readings.");
    addBoardArguments(board_voltages, false);
    board.add_subparser(board_show);
    board.add_subparser(board_name);
    board.add_subparser(board_version);
    board.add_subparser(board_dna);
    board.add_subparser(board_temperature);
    board.add_subparser(board_voltages);

    argparse::ArgumentParser qspi("qspi", MRS_FPGACTL_VERSION);
    qspi.add_description("Inspect, read, verify, program, and boot AXI-attached SPI NOR flash.");
    argparse::ArgumentParser qspi_show("show", MRS_FPGACTL_VERSION);
    qspi_show.add_description("Show detected flash geometry, status, and boot-mode state.");
    addQspiArguments(qspi_show);
    qspi_show.add_argument("--json").flag().help("emit machine-readable JSON");
    argparse::ArgumentParser qspi_read("read", MRS_FPGACTL_VERSION);
    qspi_read.add_description("Read a flash range into a binary file.");
    addQspiArguments(qspi_read);
    qspi_read.add_argument("output").help("output binary file");
    qspi_read.add_argument("--offset")
        .metavar("OFFSET")
        .nargs(1)
        .help("flash offset (default: 0)");
    qspi_read.add_argument("--length").metavar("BYTES").help("bytes to read; default is to flash end");
    qspi_read.add_argument("--force").flag().help("replace an existing output file");
    argparse::ArgumentParser qspi_verify("verify", MRS_FPGACTL_VERSION);
    qspi_verify.add_description("Compare an image byte-for-byte with flash contents.");
    addQspiArguments(qspi_verify);
    qspi_verify.add_argument("image").help(".bit or .bin image to verify");
    qspi_verify.add_argument("--offset")
        .metavar("OFFSET")
        .nargs(1)
        .help("flash offset (default: 0)");
    qspi_verify.add_argument("--expected-part").metavar("PART").help("required substring of .bit part metadata");
    argparse::ArgumentParser qspi_program("program", MRS_FPGACTL_VERSION);
    qspi_program.add_description("Erase, program, and byte-for-byte verify changed flash sectors.");
    addQspiArguments(qspi_program);
    addHwIcapArguments(qspi_program);
    qspi_program.add_argument("image").help(".bit or .bin image to program");
    qspi_program.add_argument("--offset")
        .metavar("OFFSET")
        .nargs(1)
        .help("flash offset (default: 0)");
    qspi_program.add_argument("--expected-part").metavar("PART").help("required substring of .bit part metadata");
    qspi_program.add_argument("--yes")
        .implicit_value(true)
        .required()
        .help("confirm persistent flash modification");
    qspi_program.add_argument("--rewrite-all").flag().help("rewrite every covered erase sector");
    qspi_program.add_argument("--reload").flag().help("warm-boot the programmed image through AXI HWICAP");
    argparse::ArgumentParser qspi_repair("repair", MRS_FPGACTL_VERSION);
    qspi_repair.add_description("Repair the supported Micron cold-boot configuration field.");
    addQspiArguments(qspi_repair);
    qspi_repair.add_argument("--yes")
        .implicit_value(true)
        .required()
        .help("confirm persistent configuration modification");
    argparse::ArgumentParser qspi_reload("reload", MRS_FPGACTL_VERSION);
    qspi_reload.add_description("Warm-boot an image from flash through AXI HWICAP.");
    addQspiArguments(qspi_reload);
    addHwIcapArguments(qspi_reload);
    qspi_reload.add_argument("--offset")
        .metavar("OFFSET")
        .nargs(1)
        .help("default flash boot address (default: 0)");
    qspi_reload.add_argument("--yes")
        .implicit_value(true)
        .required()
        .help("confirm whole-FPGA reconfiguration");
    qspi.add_subparser(qspi_show);
    qspi.add_subparser(qspi_read);
    qspi.add_subparser(qspi_verify);
    qspi.add_subparser(qspi_program);
    qspi.add_subparser(qspi_repair);
    qspi.add_subparser(qspi_reload);

    xdma.add_subparser(xdma_show);
    xdma.add_subparser(xdma_list);
    xdma.add_subparser(xdma_driver);
    xdma.add_subparser(xdma_mrrs);
    xdma.add_subparser(board);
    xdma.add_subparser(qspi);

    argparse::ArgumentParser jtag("jtag", MRS_FPGACTL_VERSION);
    jtag.add_description("Detect and program FPGA devices through a physical USB-JTAG cable.");
    argparse::ArgumentParser jtag_show("show", MRS_FPGACTL_VERSION);
    jtag_show.add_description("Detect and verify the JTAG target and report its XADC temperature.");
    addJtagTargetArguments(jtag_show);
    argparse::ArgumentParser jtag_program("program", MRS_FPGACTL_VERSION);
    jtag_program.add_description("Program volatile SRAM or persistent SPI flash through JTAG.");
    addJtagTargetArguments(jtag_program);
    addPcieArguments(jtag_program, false, false);
    auto& jtag_destination = jtag_program.add_mutually_exclusive_group(true);
    jtag_destination.add_argument("--direct")
        .metavar("IMAGE")
        .help("program the image directly into volatile FPGA SRAM");
    jtag_destination.add_argument("--flash")
        .metavar("IMAGE")
        .help("program and verify the image in persistent SPI flash");
    jtag_program.add_argument("--file-type")
        .metavar("TYPE")
        .choices("bit", "bin")
        .help("override image type inference");
    jtag_program.add_argument("--offset")
        .metavar("OFFSET")
        .nargs(1)
        .help("SPI flash byte offset; valid with --flash (default: 0)");
    jtag_program.add_argument("--fpga-part")
        .metavar("PART")
        .nargs(1)
        .help("FPGA part passed to the SPI-over-JTAG bridge (default: xc7a200tfbg484)");
    jtag_program.add_argument("--yes").flag().help("confirm persistent flash programming");
    jtag_program.add_argument("--reload").flag().help("recover PCIe and XDMA after programming");
    jtag.add_subparser(jtag_show);
    jtag.add_subparser(jtag_program);

    program.add_subparser(xdma);
    program.add_subparser(jtag);

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& error) {
        std::ostringstream message;
        message << error.what() << '\n' << program;
        throw Error(message.str());
    }

    if (program.is_subcommand_used(xdma)) {
        if (xdma.is_subcommand_used(xdma_show)) {
            return runXdma(XdmaAction::show, parsePcieOptions(xdma_show, true, false));
        }
        if (xdma.is_subcommand_used(xdma_list)) {
            return runXdma(XdmaAction::list, parsePcieOptions(xdma_list, true, true));
        }
        if (xdma.is_subcommand_used(xdma_driver)) {
            XdmaAction action = XdmaAction::driver_reload;
            if (xdma_driver.get<bool>("--probe")) action = XdmaAction::driver_probe;
            if (xdma_driver.get<bool>("--driver-only")) action = XdmaAction::driver_only;
            if (xdma_driver.get<bool>("--post-reconfigure")) action = XdmaAction::post_reconfigure;
            if (xdma_driver.get<bool>("--remove")) action = XdmaAction::remove;
            return runXdma(action, parsePcieOptions(xdma_driver, false, false));
        }
        if (xdma.is_subcommand_used(xdma_mrrs)) {
            const auto value = static_cast<unsigned int>(parseUnsigned(
                xdma_mrrs.get<std::string>("--set"), "MRRS", 4096));
            return runXdma(
                XdmaAction::set_mrrs,
                parsePcieOptions(xdma_mrrs, false, false),
                value);
        }
        if (xdma.is_subcommand_used(board)) {
            if (board.is_subcommand_used(board_show)) {
                return runBoard(parseBoardCommand(board_show, BoardAction::show, true));
            }
            if (board.is_subcommand_used(board_name)) {
                return runBoard(parseBoardCommand(board_name, BoardAction::name, false));
            }
            if (board.is_subcommand_used(board_version)) {
                return runBoard(parseBoardCommand(board_version, BoardAction::version, false));
            }
            if (board.is_subcommand_used(board_dna)) {
                return runBoard(parseBoardCommand(board_dna, BoardAction::dna, false));
            }
            if (board.is_subcommand_used(board_temperature)) {
                return runBoard(parseBoardCommand(
                    board_temperature, BoardAction::temperature, false));
            }
            if (board.is_subcommand_used(board_voltages)) {
                return runBoard(parseBoardCommand(
                    board_voltages, BoardAction::voltages, false));
            }
            return showHelp(board);
        }
        if (xdma.is_subcommand_used(qspi)) {
            if (qspi.is_subcommand_used(qspi_show)) {
                auto command = parseQspiBase(qspi_show, QspiAction::show);
                command.json = qspi_show.get<bool>("--json");
                return runQspi(std::move(command));
            }
            if (qspi.is_subcommand_used(qspi_read)) {
                auto command = parseQspiBase(qspi_read, QspiAction::read);
                command.operand = qspi_read.get<std::string>("output");
                command.offset = parseUnsigned(
                    optionOr(qspi_read, "--offset", "0"), "flash offset");
                if (const auto value = qspi_read.present<std::string>("--length")) {
                    command.length = parseUnsigned(*value, "read length");
                }
                command.force = qspi_read.get<bool>("--force");
                return runQspi(std::move(command));
            }
            if (qspi.is_subcommand_used(qspi_verify)) {
                auto command = parseQspiBase(qspi_verify, QspiAction::verify);
                command.operand = qspi_verify.get<std::string>("image");
                command.offset = parseUnsigned(
                    optionOr(qspi_verify, "--offset", "0"), "flash offset");
                command.expected_part = qspi_verify.present<std::string>("--expected-part");
                return runQspi(std::move(command));
            }
            if (qspi.is_subcommand_used(qspi_program)) {
                auto command = parseQspiBase(qspi_program, QspiAction::program);
                command.operand = qspi_program.get<std::string>("image");
                command.offset = parseUnsigned(
                    optionOr(qspi_program, "--offset", "0"), "flash offset");
                command.expected_part = qspi_program.present<std::string>("--expected-part");
                command.confirmed = qspi_program.get<bool>("--yes");
                command.rewrite_all = qspi_program.get<bool>("--rewrite-all");
                command.reload_after = qspi_program.get<bool>("--reload");
                parseHwIcapOptions(qspi_program, command);
                return runQspi(std::move(command));
            }
            if (qspi.is_subcommand_used(qspi_repair)) {
                auto command = parseQspiBase(qspi_repair, QspiAction::repair);
                command.confirmed = qspi_repair.get<bool>("--yes");
                return runQspi(std::move(command));
            }
            if (qspi.is_subcommand_used(qspi_reload)) {
                auto command = parseQspiBase(qspi_reload, QspiAction::reload);
                command.offset = parseUnsigned(
                    optionOr(qspi_reload, "--offset", "0"), "flash offset");
                command.confirmed = qspi_reload.get<bool>("--yes");
                parseHwIcapOptions(qspi_reload, command);
                return runQspi(std::move(command));
            }
            return showHelp(qspi);
        }
        return showHelp(xdma);
    }

    if (program.is_subcommand_used(jtag)) {
        if (jtag.is_subcommand_used(jtag_show)) {
            auto command = parseJtagTarget(jtag_show);
            command.action = JtagAction::show;
            return runJtag(command);
        }
        if (jtag.is_subcommand_used(jtag_program)) {
            auto command = parseJtagTarget(jtag_program);
            if (const auto image = jtag_program.present<std::string>("--direct")) {
                command.action = JtagAction::program_direct;
                command.image = *image;
            } else {
                command.action = JtagAction::program_flash;
                command.image = jtag_program.get<std::string>("--flash");
            }
            command.file_type = jtag_program.present<std::string>("--file-type");
            command.offset = parseUnsigned(
                optionOr(jtag_program, "--offset", "0"), "flash offset");
            command.fpga_part = optionOr(
                jtag_program, "--fpga-part", "xc7a200tfbg484");
            command.confirmed = jtag_program.get<bool>("--yes");
            command.reload = jtag_program.get<bool>("--reload");
            command.pcie = parsePcieOptions(jtag_program, false, false);
            return runJtag(command);
        }
        return showHelp(jtag);
    }

    return showHelp(program);
}

}  // namespace fpgactl
