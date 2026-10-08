/**
 * @file jtag.cpp
 * @brief Verified SRAM and persistent-flash programming through USB-JTAG.
 */

#include "jtag.h"

#include "common.h"
#include "pcie.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>

#include <unistd.h>

namespace fpgactl {
namespace {

using JtagOptions = JtagCommand;

/** Known flash capacities used to reject an oversized image before erasing. */
struct JtagFlashProfile {
    std::string board;
    std::string model;
    std::uint64_t capacity = 0;
};

/**
 * Unique scratch directory whose contents are removed on every exit path.
 *
 * mkdtemp performs the create-and-select operation atomically, avoiding name
 * collisions between simultaneous fpgactl invocations.  Cleanup is best
 * effort because an already completed hardware operation must not be reported
 * as failed solely due to removal of a one-byte probe file.
 */
class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const std::string& prefix) {
        std::string pattern =
            (std::filesystem::temp_directory_path() / (prefix + "-XXXXXX")).string();
        char* created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw Error("cannot create temporary JTAG directory");
        }
        path_ = created;
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::vector<std::string> loaderBaseCommand(const JtagOptions& options) {
    std::vector<std::string> result{
        options.loader,
        "--cable", options.cable,
        "--freq", std::to_string(options.frequency),
    };
    if (options.serial.has_value()) {
        result.insert(result.end(), {"--ftdi-serial", *options.serial});
    }
    result.insert(
        result.end(), options.loader_arguments.begin(), options.loader_arguments.end());
    return result;
}

std::uint32_t detectTarget(const JtagOptions& options) {
    auto command = loaderBaseCommand(options);
    command.push_back("--detect");
    const ProcessResult result = runProcess(command, true);
    if (result.exit_code != 0) {
        throw Error("physical USB-JTAG detection failed with status " +
                    std::to_string(result.exit_code));
    }
    const std::regex pattern(R"(\bidcode\s+(0x[0-9a-f]+)\b)", std::regex::icase);
    std::smatch match;
    if (!std::regex_search(result.output, match, pattern)) {
        throw Error("JTAG detection did not report an IDCODE");
    }
    const auto idcode = static_cast<std::uint32_t>(
        parseUnsigned(match[1].str(), "detected IDCODE", UINT32_MAX));
    std::cout << "detected IDCODE: 0x" << std::hex << std::setw(8) << std::setfill('0')
              << idcode << std::dec << std::setfill(' ') << '\n';
    if (options.expected_idcode.has_value()) {
        if ((idcode & options.idcode_mask) !=
            (*options.expected_idcode & options.idcode_mask)) {
            throw Error("detected IDCODE does not match the expected target");
        }
        std::cout << "IDCODE check:    passed (mask 0x" << std::hex
                  << options.idcode_mask << std::dec << ")\n";
    } else {
        std::cout << "IDCODE check:    disabled\n";
    }
    return idcode;
}

void reportTemperature(const JtagOptions& options) {
    auto command = loaderBaseCommand(options);
    command.push_back("--read_xadc");
    const ProcessResult result = runProcess(command);
    const std::regex pattern(
        R"("temp"\s*:\s*(-?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?))");
    std::smatch match;
    if (result.exit_code != 0 || !std::regex_search(result.output, match, pattern)) {
        std::cerr << "warning: XADC temperature was unavailable through JTAG\n";
        return;
    }
    std::cout << "XADC temperature: " << std::fixed << std::setprecision(3)
              << std::stod(match[1].str()) << " C\n";
}

std::optional<std::string> parseJedec(const std::string& output) {
    std::string lowered = output;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (lowered.find("s25fl256s") != std::string::npos) return "01 02 19";
    if (lowered.find("mt25ql512") != std::string::npos ||
        lowered.find("n25q512") != std::string::npos) return "20 ba 20";
    const std::regex detail(
        R"(Jedec ID\s*:\s*([0-9a-fA-F]{2})[\s\S]*memory type\s*:\s*([0-9a-fA-F]{2})[\s\S]*memory capacity\s*:\s*([0-9a-fA-F]{2}))",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_search(output, match, detail)) return std::nullopt;
    std::string result = match[1].str() + " " + match[2].str() + " " + match[3].str();
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

JtagFlashProfile detectFlash(const JtagOptions& options) {
    TemporaryDirectory temporary("mrs-fpgactl-jtag");
    const auto probe = temporary.path() / "probe.bin";
    auto command = loaderBaseCommand(options);
    command.insert(command.end(), {
        "--fpga-part", options.fpga_part,
        "--offset", "0",
        "--dump-flash",
        "--file-size", "1",
        probe.string(),
    });
    const ProcessResult result = runProcess(command, true);
    if (result.exit_code != 0) {
        throw Error("read-only SPI-over-JTAG flash detection failed with status " +
                    std::to_string(result.exit_code));
    }
    const auto jedec = parseJedec(result.output);
    if (!jedec.has_value()) {
        throw Error("SPI-over-JTAG probe did not report a usable JEDEC ID");
    }
    JtagFlashProfile profile;
    if (*jedec == "01 02 19") {
        profile = {"NiteFury", "Spansion/Cypress S25FL256S", 32ULL * 1024 * 1024};
    } else if (*jedec == "20 ba 20") {
        profile = {"Aller", "Micron MT25QL512", 64ULL * 1024 * 1024};
    } else {
        throw Error("unsupported board flash JEDEC ID " + *jedec);
    }
    std::cout << "flash JEDEC ID:   " << *jedec << '\n'
              << "flash:            " << profile.model << '\n'
              << "board profile:    " << profile.board << '\n'
              << "flash capacity:   " << profile.capacity / (1024 * 1024) << " MiB\n";
    return profile;
}

std::string inferImageType(
    const std::filesystem::path& image,
    const std::optional<std::string>& override) {
    if (override.has_value()) return *override;
    std::string extension = image.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (extension == ".bit") return "bit";
    if (extension == ".bin") return "bin";
    throw Error("cannot infer image type; use --file-type bit or --file-type bin");
}

/** Reject incomplete uploads before the loader can touch FPGA configuration. */
void validateBitImage(const std::filesystem::path& image) {
    const auto bytes = readBinary(image);
    std::size_t position = 0;
    const auto readLength = [&](std::size_t count) {
        if (count > bytes.size() - position) throw Error("truncated bitstream header");
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < count; ++i) value = (value << 8) | bytes[position++];
        return value;
    };
    const auto skip = [&](std::size_t count) {
        if (count > bytes.size() - position) throw Error("truncated bitstream header");
        position += count;
    };
    skip(readLength(2));
    if (readLength(2) != 1) throw Error("invalid bitstream field marker length");
    for (char field : std::string("abcde")) {
        if (position == bytes.size() || bytes[position++] != field) {
            throw Error("invalid bitstream header field");
        }
        const auto count = readLength(field == 'e' ? 4 : 2);
        if (field == 'e') {
            if (count == 0 || count != bytes.size() - position) {
                throw Error("bitstream payload length does not match the complete image file");
            }
            return;
        }
        skip(count);
    }
}

}  // namespace

int runJtag(const JtagCommand& options) {
    if (options.action == JtagAction::show && options.reload) {
        throw Error("--reload is valid only with jtag program");
    }
    if (options.action == JtagAction::program_flash && !options.confirmed) {
        throw Error("refusing persistent flash programming without --yes");
    }
    if (options.action == JtagAction::program_direct && options.confirmed) {
        throw Error("--yes is only meaningful with --flash");
    }
    if (!commandExists(options.loader)) {
        throw Error("required JTAG loader was not found: " + options.loader);
    }
    requireRoot();
    if (options.action == JtagAction::show) {
        (void)detectTarget(options);
        reportTemperature(options);
        std::cout << "physical USB-JTAG detection passed\n";
        return 0;
    }

    if (!options.image.has_value()) {
        throw Error("JTAG programming requires an image path");
    }
    const bool program_flash = options.action == JtagAction::program_flash;
    const std::filesystem::path image(*options.image);
    if (!std::filesystem::is_regular_file(image)) {
        throw Error("image file does not exist: " + image.string());
    }
    const std::string image_type = inferImageType(image, options.file_type);
    const std::uint64_t image_size = std::filesystem::file_size(image);
    if (image_size == 0) throw Error("image file is empty");
    if (image_type == "bit") validateBitImage(image);
    (void)detectTarget(options);
    reportTemperature(options);
    std::cout << "image:           " << std::filesystem::absolute(image) << '\n'
              << "image type:      " << image_type << '\n'
              << "image bytes:     " << image_size << '\n'
              << "image SHA256:    " << sha256File(image) << '\n'
              << "JTAG cable:      " << options.cable << '\n'
              << "JTAG frequency:  " << options.frequency << " Hz\n";

    std::optional<std::string> selected_bdf;
    if (options.reload) {
        selected_bdf = findCurrentBdf(options.pcie, true);
        if (!selected_bdf.has_value()) selected_bdf = readSavedBdf(options.pcie.device_index);
        if (selected_bdf.has_value() && std::filesystem::exists(
                std::filesystem::path("/sys/bus/pci/devices") / *selected_bdf)) {
            std::cout << "removing live PCI endpoint before whole-device configuration\n";
            removeForReconfiguration(*selected_bdf, options.pcie);
        } else {
            std::cout << "PCI endpoint is absent; recovery will discover it after programming\n";
        }
    }

    if (program_flash) {
        const JtagFlashProfile profile = detectFlash(options);
        if (options.offset > profile.capacity || image_size > profile.capacity - options.offset) {
            throw Error("image exceeds the detected flash capacity at the selected offset");
        }
    } else if (options.offset != 0) {
        throw Error("--offset is valid only with --flash");
    }

    auto command = loaderBaseCommand(options);
    command.insert(command.end(), {"--file-type", image_type});
    if (program_flash) {
        command.insert(command.end(), {
            "--fpga-part", options.fpga_part,
            "--offset", std::to_string(options.offset),
            "--write-flash", "--verify", image.string(),
        });
        std::cout << "programming and verifying board SPI flash through USB-JTAG\n";
    } else {
        command.insert(command.end(), {"--write-sram", image.string()});
        std::cout << "programming volatile FPGA SRAM through USB-JTAG\n";
    }
    const ProcessResult programmed = runProcess(command, true);
    if (programmed.exit_code != 0) {
        throw Error("JTAG programming failed with status " +
                    std::to_string(programmed.exit_code));
    }
    // Some openFPGALoader versions print a parse/program FAIL and return zero.
    // A reported failure must never become a successful fpgactl operation.
    if (std::regex_search(programmed.output,
            std::regex(R"((^|[\r\n])[ \t]*FAIL([\r\n]|$))"))) {
        throw Error("JTAG loader reported FAIL despite exit status zero");
    }

    if (options.reload) {
        const std::string recovered = recoverAfterReconfiguration(
            selected_bdf, options.pcie.bdf.has_value(), options.pcie);
        rememberBdf(options.pcie.device_index, recovered);
        std::cout << (program_flash ? "SPI flash" : "FPGA")
                  << " programmed and PCIe/XDMA recovered successfully\n";
    } else {
        std::cout << (program_flash ? "SPI flash" : "FPGA")
                  << " programmed successfully through USB-JTAG\n";
    }
    return 0;
}

}  // namespace fpgactl
