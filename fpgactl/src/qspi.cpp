/**
 * @file qspi.cpp
 * @brief Commands built on the development library's QSPI and HWICAP drivers.
 */

#include "qspi.h"

#include "common.h"
#include "pcie.h"

#include "mrs_fpga_dev/ip_drivers/hwicap.h"
#include "mrs_fpga_dev/ip_drivers/qspi.h"
#include "mrs_fpga_dev/xdma.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>

#include <unistd.h>

namespace fpgactl {
namespace {

using mrs::fpga::AxiHwIcap;
using mrs::fpga::AxiQuadSpi;
using mrs::fpga::SpiNor;

/** Configuration payload and descriptive fields extracted from an image. */
struct Image {
    std::vector<std::uint8_t> bytes;
    std::map<std::string, std::string> metadata;
};

using QspiOptions = QspiCommand;

std::uint16_t big16(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 2 > data.size()) throw Error("truncated Xilinx .bit header");
    return static_cast<std::uint16_t>(data[offset] << 8U) | data[offset + 1];
}

std::uint32_t big32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 4 > data.size()) throw Error("truncated Xilinx .bit payload length");
    return (static_cast<std::uint32_t>(data[offset]) << 24U) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 8U) |
           data[offset + 3];
}

bool containsSync(const std::vector<std::uint8_t>& bytes) {
    static constexpr std::array<std::uint8_t, 4> sync{0xaa, 0x99, 0x55, 0x66};
    const auto end = bytes.begin() + std::min<std::size_t>(bytes.size(), 4096);
    return std::search(bytes.begin(), end, sync.begin(), sync.end()) != end;
}

Image loadImage(
    const std::filesystem::path& path,
    const std::optional<std::string>& expected_part) {
    const auto raw = readBinary(path);
    Image result;
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (extension == ".bin") {
        result.bytes = raw;
    } else if (extension == ".bit") {
        if (raw.size() < 16) throw Error("Xilinx .bit image is too short");
        std::size_t position = 0;
        position += 2 + big16(raw, position);
        if (big16(raw, position) != 1) {
            throw Error("unexpected Xilinx .bit secondary-header length");
        }
        position += 2;
        const std::map<std::uint8_t, std::string> names{
            {'a', "design"}, {'b', "part"}, {'c', "date"}, {'d', "time"},
        };
        bool found_payload = false;
        while (position < raw.size()) {
            const std::uint8_t tag = raw[position++];
            if (tag == 'e') {
                const std::uint32_t length = big32(raw, position);
                position += 4;
                if (length > raw.size() - position) {
                    throw Error("Xilinx .bit payload is shorter than declared");
                }
                result.bytes.assign(raw.begin() + position, raw.begin() + position + length);
                found_payload = true;
                break;
            }
            const std::uint16_t length = big16(raw, position);
            position += 2;
            if (length > raw.size() - position) throw Error("truncated Xilinx .bit metadata");
            const auto named = names.find(tag);
            if (named != names.end()) {
                std::string value(raw.begin() + position, raw.begin() + position + length);
                while (!value.empty() && value.back() == '\0') value.pop_back();
                result.metadata[named->second] = std::move(value);
            }
            position += length;
        }
        if (!found_payload) throw Error("Xilinx .bit payload field was not found");
    } else {
        throw Error("supported image extensions are .bit and .bin");
    }

    if (!containsSync(result.bytes)) {
        throw Error("Xilinx synchronization word was not found near image start");
    }
    if (result.bytes.empty() ||
        std::all_of(result.bytes.begin(), result.bytes.end(), [](std::uint8_t value) {
            return value == 0xff;
        })) {
        throw Error("refusing an empty or all-0xff image");
    }
    if (expected_part.has_value()) {
        const auto found = result.metadata.find("part");
        if (found == result.metadata.end()) {
            throw Error("--expected-part requires a .bit image with part metadata");
        }
        std::string actual = found->second;
        std::string expected = *expected_part;
        const auto lower = [](unsigned char value) { return static_cast<char>(std::tolower(value)); };
        std::transform(actual.begin(), actual.end(), actual.begin(), lower);
        std::transform(expected.begin(), expected.end(), expected.begin(), lower);
        if (actual.find(expected) == std::string::npos) {
            throw Error("bitstream part does not match --expected-part");
        }
    }
    return result;
}

/** Hash an in-memory configuration payload through a securely named file. */
std::string hashBytes(const std::vector<std::uint8_t>& bytes) {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "mrs-fpgactl-hash-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int descriptor = ::mkstemp(name.data());
    if (descriptor < 0) throw Error("cannot create temporary hash input");
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            (void)::close(descriptor);
            (void)::unlink(name.data());
            throw Error("cannot write temporary hash input");
        }
        offset += static_cast<std::size_t>(count);
    }
    (void)::close(descriptor);
    try {
        const std::string digest = sha256File(name.data());
        (void)::unlink(name.data());
        return digest;
    } catch (...) {
        (void)::unlink(name.data());
        throw;
    }
}

std::string formatJedec(const std::vector<std::uint8_t>& id) {
    std::ostringstream output;
    for (std::size_t index = 0; index < id.size(); ++index) {
        if (index != 0) output << ' ';
        output << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<unsigned int>(id[index]);
    }
    return output.str();
}

void displayFlash(SpiNor& flash, bool json) {
    const std::uint8_t status = flash.readStatus();
    std::optional<std::uint16_t> nvcr;
    if (flash.isMicron()) nvcr = flash.readMicronNvcr();
    if (json) {
        std::cout << "{\n"
                  << "  \"jedec_id\": \"" << formatJedec(flash.jedecId()) << "\",\n"
                  << "  \"profile\": \"" << flash.profile().name << "\",\n"
                  << "  \"physical_capacity\": " << flash.profile().physical_capacity << ",\n"
                  << "  \"accessible_capacity\": " << flash.capacity() << ",\n"
                  << "  \"erase_size\": " << flash.profile().erase_size << ",\n"
                  << "  \"page_size\": " << flash.profile().page_size << ",\n"
                  << "  \"status\": " << static_cast<unsigned int>(status) << ",\n"
                  << "  \"block_protected\": "
                  << ((status & flash.profile().protect_mask) != 0U ? "true" : "false");
        if (nvcr.has_value()) {
            std::cout << ",\n  \"nvcr\": " << *nvcr
                      << ",\n  \"boot_mode_safe\": "
                      << (SpiNor::micronBootSafe(*nvcr) ? "true" : "false");
        }
        std::cout << "\n}\n";
        return;
    }
    std::cout << "JEDEC ID:        " << formatJedec(flash.jedecId()) << '\n'
              << "flash:           " << flash.profile().name << '\n'
              << "physical size:   " << flash.profile().physical_capacity / 1048576 << " MiB\n"
              << "accessible size: " << flash.capacity() / 1048576 << " MiB\n"
              << "erase block:     " << flash.profile().erase_size / 1024 << " KiB\n"
              << "program page:    " << flash.profile().page_size << " bytes\n"
              << "status:          0x" << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<unsigned int>(status) << std::dec << std::setfill(' ') << '\n'
              << "write protect:   "
              << ((status & flash.profile().protect_mask) != 0U ? "enabled" : "clear") << '\n';
    if (nvcr.has_value()) {
        std::cout << "Micron NVCR:     0x" << std::hex << std::setw(4) << std::setfill('0')
                  << *nvcr << std::dec << std::setfill(' ') << '\n'
                  << "cold-boot mode:  "
                  << (SpiNor::micronBootSafe(*nvcr) ? "safe" : "unsafe; use repair --yes")
                  << '\n';
    }
}

void requireWritable(SpiNor& flash) {
    const std::uint8_t status = flash.readStatus();
    if ((status & flash.profile().error_mask) != 0U) {
        throw Error("flash has a latched erase/program error");
    }
    if ((status & flash.profile().protect_mask) != 0U) {
        throw Error("flash block-protect bits are set; protection is not altered");
    }
}

void programAndVerify(
    SpiNor& flash,
    const std::vector<std::uint8_t>& image,
    std::uint64_t image_offset,
    bool rewrite_all) {
    flash.checkRange(image_offset, image.size());
    requireWritable(flash);
    const std::uint64_t erase_size = flash.profile().erase_size;
    const std::uint64_t first = image_offset / erase_size;
    const std::uint64_t last = (image_offset + image.size() + erase_size - 1) / erase_size;
    std::size_t changed = 0;
    const auto started = std::chrono::steady_clock::now();
    for (std::uint64_t sector = first; sector < last; ++sector) {
        const std::uint64_t address = sector * erase_size;
        const std::uint64_t start = std::max(address, image_offset);
        const std::uint64_t end = std::min(address + erase_size, image_offset + image.size());
        const std::size_t destination_start = start - address;
        const std::size_t destination_end = end - address;
        const bool fully_covered = destination_start == 0 && destination_end == erase_size;
        std::vector<std::uint8_t> current;
        std::vector<std::uint8_t> desired;
        if (rewrite_all && fully_covered) {
            desired.assign(image.begin() + (start - image_offset), image.begin() + (end - image_offset));
        } else {
            current = flash.read(address, erase_size);
            desired = current;
            std::copy(
                image.begin() + (start - image_offset),
                image.begin() + (end - image_offset),
                desired.begin() + destination_start);
        }
        std::cout << '[' << (sector - first + 1) << '/' << (last - first) << "] 0x"
                  << std::hex << std::setw(8) << std::setfill('0') << address
                  << std::dec << std::setfill(' ');
        if (!rewrite_all && current == desired) {
            std::cout << ": unchanged\n";
            continue;
        }
        std::cout << ": erase/program\n";
        ++changed;
        flash.eraseSector(address);
        for (std::size_t page = 0; page < desired.size(); page += flash.profile().page_size) {
            const auto page_end = desired.begin() +
                std::min(desired.size(), page + flash.profile().page_size);
            if (std::any_of(
                    desired.begin() + page, page_end,
                    [](std::uint8_t value) { return value != 0xff; })) {
                flash.program(address + page, {desired.begin() + page, page_end});
            }
        }
    }
    std::cout << "final byte-for-byte verification\n";
    const auto actual = flash.read(image_offset, image.size());
    if (actual != image) {
        const auto mismatch = std::mismatch(actual.begin(), actual.end(), image.begin());
        throw Error(
            "verification failed at flash offset " +
            std::to_string(image_offset + std::distance(actual.begin(), mismatch.first)));
    }
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    std::cout << "changed sectors: " << changed << '/' << (last - first) << '\n'
              << "verified SHA256: " << hashBytes(actual) << '\n'
              << "total elapsed:   " << std::fixed << std::setprecision(1)
              << elapsed << " s\n";
}

void reloadFromFlash(QspiOptions& options) {
    if (!options.confirmed) throw Error("refusing whole-FPGA reload without --yes");
    auto bdf = findCurrentBdf(options.pcie, true);
    if (!bdf.has_value()) bdf = readSavedBdf(options.pcie.device_index);
    const std::string device = options.device.value_or(
        "/dev/xdma" + std::to_string(options.pcie.device_index) + "_user");
    if (!bdf.has_value() || !std::filesystem::exists(device)) {
        bdf = recoverAfterReconfiguration(bdf, options.pcie.bdf.has_value(), options.pcie);
    }
    if (!std::filesystem::exists(device)) {
        throw Error("XDMA recovery did not create the selected user BAR device");
    }
    rememberBdf(options.pcie.device_index, *bdf);
    if (!options.boot_address.has_value() && options.offset > UINT32_MAX) {
        throw Error("flash offset cannot be represented as a HWICAP boot address");
    }
    const std::uint32_t boot_address =
        options.boot_address.value_or(static_cast<std::uint32_t>(options.offset));
    std::cout << "issuing IPROG from flash address 0x" << std::hex << std::setw(8)
              << std::setfill('0') << boot_address << std::dec << std::setfill(' ') << '\n';
    {
        mrs::fpga::XDMAUser registers(device);
        AxiHwIcap icap(
            registers, options.hwicap_base, options.hwicap_timeout, options.hwicap_vacancy);
        icap.validate();
        std::cout.flush();
        icap.triggerIprog(boot_address);
    }
    reloadAfterIprog(*bdf, options.pcie);
    std::cout << "FPGA reloaded from persistent flash and PCIe/XDMA recovered\n";
}

}  // namespace

int runQspi(QspiCommand options) {
    const bool persistent_action = options.action == QspiAction::program ||
        options.action == QspiAction::repair || options.action == QspiAction::reload;
    if (persistent_action && !options.confirmed) {
        throw Error("refusing the requested persistent operation without --yes");
    }
    if (options.action == QspiAction::reload) {
        requireRoot();
        reloadFromFlash(options);
        return 0;
    }

    requireRoot();
    const std::string device = options.device.value_or(
        "/dev/xdma" + std::to_string(options.pcie.device_index) + "_user");
    std::optional<Image> image;
    if (options.action == QspiAction::verify || options.action == QspiAction::program) {
        if (!options.operand.has_value()) throw Error("the selected QSPI action requires an image");
        image = loadImage(*options.operand, options.expected_part);
        std::cout << "image:           " << std::filesystem::absolute(*options.operand) << '\n'
                  << "image bytes:     " << image->bytes.size() << '\n'
                  << "image SHA256:    " << hashBytes(image->bytes) << '\n';
        for (const auto& field : image->metadata) {
            std::cout << "bit " << std::left << std::setw(12) << (field.first + ':')
                      << field.second << std::right << '\n';
        }
    }

    {
        mrs::fpga::XDMAUser registers(device);
        AxiQuadSpi controller(registers, options.qspi_base, options.spi_timeout);
        SpiNor flash(controller);
        displayFlash(flash, options.json);
        if (options.action == QspiAction::show) return 0;
        if (options.action == QspiAction::repair) {
            const auto values = flash.repairMicronBootMode();
            std::cout << "Micron NVCR: 0x" << std::hex << values.first
                      << " -> 0x" << values.second << std::dec << '\n';
            return 0;
        }
        if (options.action == QspiAction::read) {
            if (!options.operand.has_value()) throw Error("qspi read requires an output file");
            flash.checkRange(options.offset, 0);
            const std::uint64_t length = options.length.value_or(flash.capacity() - options.offset);
            if (length > std::numeric_limits<std::size_t>::max()) {
                throw Error("read is too large for this host");
            }
            const auto started = std::chrono::steady_clock::now();
            const auto bytes = flash.read(options.offset, static_cast<std::size_t>(length));
            writeBinary(*options.operand, bytes, options.force);
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            std::cout << "read file:       " << std::filesystem::absolute(*options.operand) << '\n'
                      << "read bytes:      " << bytes.size() << '\n'
                      << "read SHA256:     " << hashBytes(bytes) << '\n'
                      << "read throughput: " << std::fixed << std::setprecision(3)
                      << bytes.size() / std::max(elapsed, 1e-9) / 1e6 << " MB/s\n";
            return 0;
        }
        if (options.action == QspiAction::verify) {
            const auto started = std::chrono::steady_clock::now();
            const auto actual = flash.read(options.offset, image->bytes.size());
            if (actual != image->bytes) {
                const auto mismatch = std::mismatch(actual.begin(), actual.end(), image->bytes.begin());
                throw Error(
                    "flash validation failed at offset " +
                    std::to_string(options.offset + std::distance(actual.begin(), mismatch.first)));
            }
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            std::cout << "validated bytes:  " << actual.size() << '\n'
                      << "validated SHA256: " << hashBytes(actual) << '\n'
                      << "read throughput:  " << std::fixed << std::setprecision(3)
                      << actual.size() / std::max(elapsed, 1e-9) / 1e6 << " MB/s\n"
                      << "flash validation passed\n";
            return 0;
        }
        if (options.action != QspiAction::program) {
            throw Error("unsupported QSPI action");
        }
        programAndVerify(flash, image->bytes, options.offset, options.rewrite_all);
    }
    std::cout << "QSPI erase/program and final verification completed successfully\n";
    if (options.reload_after) {
        if (!options.boot_address.has_value()) {
            if (options.offset > UINT32_MAX) {
                throw Error("image offset cannot be represented as a boot address");
            }
            options.boot_address = static_cast<std::uint32_t>(options.offset);
        }
        reloadFromFlash(options);
    } else {
        std::cout << "the running FPGA image is unchanged; add --reload to boot it\n";
    }
    return 0;
}

}  // namespace fpgactl
