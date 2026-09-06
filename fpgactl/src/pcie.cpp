/**
 * @file pcie.cpp
 * @brief Sysfs-based, board-independent PCIe and XDMA lifecycle control.
 */

#include "pcie.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fpgactl {
namespace {

const std::filesystem::path sysfs_pci = "/sys/bus/pci";
const std::filesystem::path sysfs_xdma = "/sys/class/xdma";
const std::filesystem::path device_directory = "/dev";
const std::filesystem::path runtime_state = "/run/mrs_fpgactl";
const std::regex bdf_pattern(
    R"(^(?:([0-9a-fA-F]{4}):)?([0-9a-fA-F]{2}):([0-9a-fA-F]{2})\.([0-7])$)");

/** Selected PCIe control fields decoded from conventional capability space. */
struct PcieControl {
    std::size_t capability_offset = 0;
    std::uint16_t device_control = 0;
    unsigned int mps_bytes = 0;
    unsigned int mrrs_bytes = 0;
};

/** One nonempty PCI BAR resource. */
struct Resource {
    std::size_t bar = 0;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::uint64_t size = 0;
    std::uint64_t flags = 0;
};

/** Inventory record assembled entirely from sysfs. */
struct Endpoint {
    std::string bdf;
    std::string vendor;
    std::string device;
    std::string subsystem_vendor;
    std::string subsystem_device;
    std::string device_class;
    std::string revision;
    std::optional<std::string> driver;
    std::optional<std::string> driver_module;
    std::optional<std::string> iommu_group;
    std::string numa_node;
    std::string current_link_speed;
    std::string current_link_width;
    std::string max_link_speed;
    std::string max_link_width;
    std::vector<Resource> resources;
    std::vector<int> xdma_indices;
    std::optional<PcieControl> pcie;
    std::string pcie_error;
};

std::filesystem::path endpointPath(const std::string& bdf) {
    return sysfs_pci / "devices" / normalizeBdf(bdf);
}

std::filesystem::path savedBdfPath(int device_index) {
    return runtime_state / ("xdma" + std::to_string(device_index) + ".bdf");
}

std::optional<std::uint64_t> readHex(const std::filesystem::path& path) {
    const std::string value = readText(path, "");
    if (value.empty()) {
        return std::nullopt;
    }
    try {
        return parseUnsigned(value, "sysfs hexadecimal value");
    } catch (const Error&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> readConfig(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw Error("cannot open PCI configuration space: " + path.string());
    }
    std::vector<std::uint8_t> result(4096);
    input.read(reinterpret_cast<char*>(result.data()), result.size());
    result.resize(static_cast<std::size_t>(input.gcount()));
    return result;
}

std::uint16_t little16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 2 > bytes.size()) {
        throw Error("PCI configuration space is truncated");
    }
    return static_cast<std::uint16_t>(bytes[offset]) |
           (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

PcieControl decodePcieControl(const std::vector<std::uint8_t>& config) {
    if (config.size() < 0x40 || (little16(config, 0x06) & 0x10U) == 0U) {
        throw Error("PCI endpoint has no readable conventional capability list");
    }
    std::size_t pointer = config[0x34] & 0xfcU;
    std::set<std::size_t> visited;
    while (pointer != 0) {
        if (pointer + 2 > config.size()) {
            throw Error(
                "PCI configuration access ended before capability offset 0x" +
                [&] {
                    std::ostringstream output;
                    output << std::hex << pointer;
                    return output.str();
                }() +
                "; elevated privilege may be required to read beyond the standard header");
        }
        if (!visited.insert(pointer).second) {
            throw Error("malformed PCI capability list");
        }
        if (config[pointer] == 0x10) {
            const std::uint16_t control = little16(config, pointer + 8);
            return {
                pointer,
                control,
                128U << ((control >> 5U) & 0x7U),
                128U << ((control >> 12U) & 0x7U),
            };
        }
        pointer = config[pointer + 1] & 0xfcU;
    }
    throw Error("PCI Express capability was not found");
}

std::vector<Resource> parseResources(const std::filesystem::path& endpoint) {
    std::vector<Resource> result;
    std::istringstream input(readText(endpoint / "resource", ""));
    std::string line;
    std::size_t index = 0;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::uint64_t start = 0;
        std::uint64_t end = 0;
        std::uint64_t flags = 0;
        if ((fields >> std::hex >> start >> end >> flags) && start != 0 && end >= start) {
            result.push_back({index, start, end, end - start + 1, flags});
        }
        ++index;
    }
    return result;
}

std::vector<std::string> xdmaNodes(int index) {
    std::vector<std::string> result;
    const std::string prefix = "xdma" + std::to_string(index) + "_";
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(device_directory, error)) {
        if (entry.path().filename().string().rfind(prefix, 0) == 0) {
            result.push_back(entry.path().string());
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<int> xdmaIndicesForBdf(const std::string& bdf) {
    std::set<int> indices;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(sysfs_xdma, error)) {
        std::smatch match;
        const std::string name = entry.path().filename().string();
        if (!std::regex_search(name, match, std::regex(R"(^xdma([0-9]+)_)") )) {
            continue;
        }
        const auto target = resolvedName(entry.path() / "device");
        if (target.has_value() && *target == normalizeBdf(bdf)) {
            indices.insert(std::stoi(match[1].str()));
        }
    }
    return {indices.begin(), indices.end()};
}

std::vector<std::string> driverBoundBdfs(const std::string& module) {
    std::vector<std::string> result;
    const auto directory = sysfs_pci / "drivers" / module;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (std::regex_match(entry.path().filename().string(), bdf_pattern)) {
            result.push_back(entry.path().filename().string());
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> matchingEndpoints(const PcieOptions& options) {
    std::vector<std::string> result;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(sysfs_pci / "devices", error)) {
        const std::string name = entry.path().filename().string();
        if (!std::regex_match(name, bdf_pattern)) {
            continue;
        }
        const auto vendor = readHex(entry.path() / "vendor");
        const auto device = readHex(entry.path() / "device");
        if (!options.include_all &&
            ((options.vendor_id.has_value() && vendor != options.vendor_id) ||
             (options.device_id.has_value() && device != options.device_id))) {
            continue;
        }
        result.push_back(name);
    }
    std::sort(result.begin(), result.end());
    return result;
}

Endpoint endpointDetails(const std::string& input_bdf) {
    Endpoint result;
    result.bdf = normalizeBdf(input_bdf);
    const auto path = endpointPath(result.bdf);
    if (!std::filesystem::is_directory(path)) {
        throw Error("PCIe endpoint does not exist: " + result.bdf);
    }
    result.vendor = readText(path / "vendor");
    result.device = readText(path / "device");
    result.subsystem_vendor = readText(path / "subsystem_vendor");
    result.subsystem_device = readText(path / "subsystem_device");
    result.device_class = readText(path / "class");
    result.revision = readText(path / "revision");
    result.driver = resolvedName(path / "driver");
    result.driver_module = resolvedName(path / "driver" / "module");
    result.iommu_group = resolvedName(path / "iommu_group");
    result.numa_node = readText(path / "numa_node");
    result.current_link_speed = readText(path / "current_link_speed");
    result.current_link_width = readText(path / "current_link_width");
    result.max_link_speed = readText(path / "max_link_speed");
    result.max_link_width = readText(path / "max_link_width");
    result.resources = parseResources(path);
    result.xdma_indices = xdmaIndicesForBdf(result.bdf);
    try {
        result.pcie = decodePcieControl(readConfig(path / "config"));
    } catch (const Error& error) {
        result.pcie_error = error.what();
    }
    return result;
}

std::string formatSize(std::uint64_t value) {
    static const std::array<const char*, 4> units{"B", "KiB", "MiB", "GiB"};
    double number = static_cast<double>(value);
    for (const char* unit : units) {
        if (number < 1024.0 || unit == units.back()) {
            std::ostringstream output;
            output << std::fixed << std::setprecision(number == std::floor(number) ? 0 : 1)
                   << number << ' ' << unit;
            return output.str();
        }
        number /= 1024.0;
    }
    return std::to_string(value) + " B";
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<unsigned int>(character) << std::dec;
                } else {
                    output << character;
                }
        }
    }
    return output.str();
}

void printEndpoint(const Endpoint& endpoint) {
    std::cout << endpoint.bdf << " vendor=" << endpoint.vendor
              << " device=" << endpoint.device << " class=" << endpoint.device_class
              << " revision=" << endpoint.revision << '\n';
    std::cout << "  subsystem: " << endpoint.subsystem_vendor << ':' << endpoint.subsystem_device
              << "  driver: " << endpoint.driver.value_or("unbound")
              << "  module: " << endpoint.driver_module.value_or("none") << '\n';
    std::cout << "  link: " << endpoint.current_link_speed << " x" << endpoint.current_link_width
              << " (max " << endpoint.max_link_speed << " x" << endpoint.max_link_width << ")\n";
    std::cout << "  NUMA: " << endpoint.numa_node
              << "  IOMMU group: " << endpoint.iommu_group.value_or("none") << '\n';
    if (endpoint.pcie.has_value()) {
        std::cout << "  PCIe control: MPS=" << endpoint.pcie->mps_bytes
                  << " B MRRS=" << endpoint.pcie->mrrs_bytes << " B DevCtl=0x"
                  << std::hex << std::setw(4) << std::setfill('0')
                  << endpoint.pcie->device_control << std::dec << std::setfill(' ') << '\n';
    } else {
        std::cout << "  PCIe control: unavailable (" << endpoint.pcie_error << ")\n";
    }
    for (const auto& resource : endpoint.resources) {
        std::cout << "  BAR" << resource.bar << ": 0x" << std::hex << resource.start
                  << "-0x" << resource.end << std::dec << " (" << formatSize(resource.size)
                  << ") flags=0x" << std::hex << resource.flags << std::dec << '\n';
    }
    if (endpoint.xdma_indices.empty()) {
        std::cout << "  XDMA: no class devices mapped\n";
    }
    for (const int index : endpoint.xdma_indices) {
        const auto nodes = xdmaNodes(index);
        std::cout << "  XDMA index " << index << ':';
        for (const auto& node : nodes) {
            std::cout << ' ' << node;
        }
        if (nodes.empty()) {
            std::cout << " no nodes";
        }
        std::cout << '\n';
    }
}

void printEndpointJson(const Endpoint& endpoint, unsigned int indent) {
    const std::string pad(indent, ' ');
    const std::string child(indent + 2, ' ');
    std::cout << pad << "{\n"
              << child << "\"bdf\": \"" << jsonEscape(endpoint.bdf) << "\",\n"
              << child << "\"vendor\": \"" << jsonEscape(endpoint.vendor) << "\",\n"
              << child << "\"device\": \"" << jsonEscape(endpoint.device) << "\",\n"
              << child << "\"class\": \"" << jsonEscape(endpoint.device_class) << "\",\n"
              << child << "\"driver\": ";
    if (endpoint.driver.has_value()) {
        std::cout << '"' << jsonEscape(*endpoint.driver) << '"';
    } else {
        std::cout << "null";
    }
    std::cout << ",\n" << child << "\"link_speed\": \""
              << jsonEscape(endpoint.current_link_speed) << "\",\n"
              << child << "\"link_width\": \"" << jsonEscape(endpoint.current_link_width)
              << "\",\n" << child << "\"xdma_indices\": [";
    for (std::size_t index = 0; index < endpoint.xdma_indices.size(); ++index) {
        if (index != 0) std::cout << ", ";
        std::cout << endpoint.xdma_indices[index];
    }
    std::cout << "],\n" << child << "\"mps_bytes\": ";
    if (endpoint.pcie.has_value()) std::cout << endpoint.pcie->mps_bytes; else std::cout << "null";
    std::cout << ",\n" << child << "\"mrrs_bytes\": ";
    if (endpoint.pcie.has_value()) std::cout << endpoint.pcie->mrrs_bytes; else std::cout << "null";
    std::cout << '\n' << pad << '}';
}

void writeOne(const std::filesystem::path& path) {
    std::ofstream output(path);
    if (!output || !(output << "1\n")) {
        throw Error("cannot write sysfs control: " + path.string());
    }
}

void unloadModule(const std::string& module) {
    if (std::filesystem::is_directory(std::filesystem::path("/sys/module") / module)) {
        std::cout << "unloading " << module << '\n';
        runChecked({"modprobe", "-r", module});
    }
}

void loadModule(const PcieOptions& options) {
    std::cout << "loading " << options.module << '\n';
    std::vector<std::string> command{"modprobe", options.module};
    command.insert(command.end(), options.module_arguments.begin(), options.module_arguments.end());
    runChecked(command);
}

void finishBinding(const std::string& bdf, const PcieOptions& options) {
    const auto path = endpointPath(bdf);
    waitUntil(
        [&] { return std::filesystem::exists(path / "driver"); },
        options.timeout,
        "driver binding at " + bdf);
    const auto driver = resolvedName(path / "driver");
    if (!driver.has_value() || *driver != options.module) {
        throw Error("endpoint is not bound to expected driver " + options.module);
    }
    for (const auto& suffix : options.required_nodes) {
        const auto node = device_directory /
            ("xdma" + std::to_string(options.device_index) + "_" + suffix);
        waitUntil([&] { return std::filesystem::exists(node); }, options.timeout, node.string());
    }
    if (options.device_mode.has_value()) {
        for (const auto& node : xdmaNodes(options.device_index)) {
            if (::chmod(node.c_str(), *options.device_mode) != 0) {
                throw Error("cannot set device mode on " + node + ": " + ::strerror(errno));
            }
        }
    }
    rememberBdf(options.device_index, bdf);
    const Endpoint details = endpointDetails(bdf);
    std::cout << "ready: " << bdf << ", link " << details.current_link_speed
              << " x" << details.current_link_width << '\n';
    std::cout << "nodes:";
    for (const auto& node : xdmaNodes(options.device_index)) std::cout << ' ' << node;
    std::cout << '\n';
}

void reenumerateKnown(
    const std::string& bdf,
    const PcieOptions& options,
    bool endpoint_may_already_be_absent,
    bool remove_only,
    bool probe_only) {
    const auto endpoint = endpointPath(bdf);
    if (!probe_only) {
        unloadModule(options.module);
    }
    if (!probe_only) {
        const auto remove = endpoint / "remove";
        if (std::filesystem::exists(remove)) {
            std::cout << "removing endpoint " << bdf << '\n';
            writeOne(remove);
            waitUntil(
                [&] { return !std::filesystem::exists(endpoint); },
                options.timeout,
                "removal of " + bdf);
        } else if (!endpoint_may_already_be_absent) {
            throw Error("endpoint cannot be removed: " + bdf);
        } else {
            std::cout << "endpoint is already absent after FPGA reconfiguration\n";
        }
    }
    if (remove_only) {
        std::cout << "endpoint removed; program the FPGA before rescanning\n";
        return;
    }
    std::cout << "rescanning PCIe\n";
    const auto rescan = sysfs_pci / "rescan";
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    while (!std::filesystem::exists(endpoint)) {
        writeOne(rescan);
        if (std::chrono::steady_clock::now() >= deadline) {
            throw Error("timeout waiting for " + bdf + " after PCIe rescan");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    loadModule(options);
    finishBinding(bdf, options);
}

void setMrrs(const std::string& bdf, unsigned int requested) {
    static constexpr std::array<unsigned int, 6> valid{128, 256, 512, 1024, 2048, 4096};
    const auto found = std::find(valid.begin(), valid.end(), requested);
    if (found == valid.end()) {
        throw Error("MRRS must be one of 128, 256, 512, 1024, 2048, or 4096");
    }
    const auto path = endpointPath(bdf) / "config";
    const PcieControl pcie = decodePcieControl(readConfig(path));
    const std::uint16_t encoded = static_cast<std::uint16_t>(std::distance(valid.begin(), found));
    const std::uint16_t updated =
        static_cast<std::uint16_t>((pcie.device_control & ~0x7000U) | (encoded << 12U));
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (descriptor < 0) {
        throw Error("cannot open PCI configuration for writing: " + std::string(::strerror(errno)));
    }
    const off_t offset = static_cast<off_t>(pcie.capability_offset + 8);
    std::uint16_t verified = 0;
    const ssize_t written = ::pwrite(descriptor, &updated, sizeof(updated), offset);
    const ssize_t read = ::pread(descriptor, &verified, sizeof(verified), offset);
    const int saved_errno = errno;
    (void)::close(descriptor);
    if (written != 2 || read != 2) {
        throw Error("short PCI configuration write/readback: " + std::string(::strerror(saved_errno)));
    }
    if ((verified & 0x7000U) != (updated & 0x7000U)) {
        throw Error("PCIe MRRS readback did not match the requested value");
    }
    std::cout << "PCIe MRRS updated: BDF=" << bdf << " bytes="
              << (128U << ((verified >> 12U) & 0x7U)) << " DeviceControl=0x"
              << std::hex << std::setw(4) << std::setfill('0') << pcie.device_control
              << "->0x" << std::setw(4) << verified << std::dec << std::setfill(' ') << '\n';
    if (requested > 1024) {
        std::cout << "warning: validate large MRRS values under sustained DMA load\n";
    }
}

}  // namespace

std::string normalizeBdf(const std::string& value) {
    std::smatch match;
    if (!std::regex_match(value, match, bdf_pattern)) {
        throw Error("invalid PCI BDF: " + value);
    }
    std::string domain = match[1].matched ? match[1].str() : "0000";
    std::string result = domain + ':' + match[2].str() + ':' + match[3].str() + '.' + match[4].str();
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::optional<std::string> findCurrentBdf(
    const PcieOptions& options, bool allow_absent_explicit) {
    if (options.bdf.has_value()) {
        const std::string normalized = normalizeBdf(*options.bdf);
        if (!allow_absent_explicit && !std::filesystem::exists(endpointPath(normalized))) {
            throw Error("PCIe endpoint does not exist: " + normalized);
        }
        return normalized;
    }
    for (const std::string suffix : {"control", "user"}) {
        const auto linked = resolvedName(
            sysfs_xdma /
            ("xdma" + std::to_string(options.device_index) + "_" + suffix) /
            "device");
        if (linked.has_value() && std::regex_match(*linked, bdf_pattern)) {
            return *linked;
        }
    }
    const auto bound = driverBoundBdfs(options.module);
    if (bound.size() == 1) return bound.front();
    if (bound.size() > 1) {
        throw Error("multiple endpoints are bound to " + options.module + "; select --bdf");
    }
    const auto matches = matchingEndpoints(options);
    if (matches.size() == 1) return matches.front();
    if (matches.size() > 1) {
        throw Error("multiple PCIe endpoints match; select --bdf or --device-id");
    }
    return std::nullopt;
}

std::optional<std::string> readSavedBdf(int device_index) {
    const std::string value = readText(savedBdfPath(device_index), "");
    if (value.empty()) return std::nullopt;
    try {
        return normalizeBdf(value);
    } catch (const Error&) {
        return std::nullopt;
    }
}

void rememberBdf(int device_index, const std::string& bdf) {
    std::error_code error;
    std::filesystem::create_directories(runtime_state, error);
    std::ofstream output(savedBdfPath(device_index));
    if (!output || !(output << normalizeBdf(bdf) << '\n')) {
        std::cerr << "warning: could not save runtime BDF hint\n";
    }
}

void removeForReconfiguration(const std::string& bdf, const PcieOptions& options) {
    rememberBdf(options.device_index, bdf);
    reenumerateKnown(bdf, options, false, true, false);
}

std::string recoverAfterReconfiguration(
    const std::optional<std::string>& preferred_bdf,
    bool strict_bdf,
    const PcieOptions& options) {
    std::cout << "recovering PCIe endpoint and XDMA\n";
    unloadModule(options.module);
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    std::optional<std::string> discovered;
    while (!discovered.has_value()) {
        writeOne(sysfs_pci / "rescan");
        if (preferred_bdf.has_value() && std::filesystem::exists(endpointPath(*preferred_bdf))) {
            discovered = normalizeBdf(*preferred_bdf);
        } else if (!strict_bdf) {
            discovered = findCurrentBdf(options, true);
            if (discovered.has_value() && !std::filesystem::exists(endpointPath(*discovered))) {
                discovered.reset();
            }
        }
        if (discovered.has_value()) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            throw Error("timeout waiting for an FPGA PCIe endpoint after repeated rescans");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    std::cout << "discovered endpoint: " << *discovered << '\n';
    loadModule(options);
    finishBinding(*discovered, options);
    return *discovered;
}

void reloadAfterIprog(const std::string& bdf, const PcieOptions& options) {
    reenumerateKnown(bdf, options, true, false, false);
}

int runXdma(
    XdmaAction action,
    const PcieOptions& options,
    std::optional<unsigned int> requested_mrrs) {
    if (action == XdmaAction::show) {
        auto bdf = findCurrentBdf(options, true);
        if (!bdf.has_value()) bdf = readSavedBdf(options.device_index);
        const bool visible = bdf.has_value() && std::filesystem::exists(endpointPath(*bdf));
        if (options.json) {
            std::cout << "{\n  \"endpoint_visible\": " << (visible ? "true" : "false")
                      << ",\n  \"module_loaded\": "
                      << (std::filesystem::is_directory(std::filesystem::path("/sys/module") / options.module)
                              ? "true" : "false")
                      << ",\n  \"endpoint\": ";
            if (visible) printEndpointJson(endpointDetails(*bdf), 2); else std::cout << "null";
            std::cout << "\n}\n";
        } else if (visible) {
            printEndpoint(endpointDetails(*bdf));
            std::cout << "  module " << options.module << ": "
                      << (std::filesystem::is_directory(std::filesystem::path("/sys/module") / options.module)
                              ? "loaded" : "not loaded") << '\n';
        } else {
            std::cout << "PCIe endpoint: not currently visible\n"
                      << "saved BDF hint: " << (bdf.has_value() ? *bdf : "none") << '\n'
                      << "XDMA nodes:";
            for (const auto& node : xdmaNodes(options.device_index)) std::cout << ' ' << node;
            std::cout << "\nrecovery: fpgactl xdma driver --probe or --reload\n";
        }
        return 0;
    }

    if (action == XdmaAction::list) {
        const auto matches = matchingEndpoints(options);
        if (options.json) std::cout << "[\n";
        for (std::size_t index = 0; index < matches.size(); ++index) {
            if (options.json) {
                printEndpointJson(endpointDetails(matches[index]), 2);
                std::cout << (index + 1 == matches.size() ? "\n" : ",\n");
            } else {
                if (index != 0) std::cout << '\n';
                printEndpoint(endpointDetails(matches[index]));
            }
        }
        if (options.json) std::cout << "]\n";
        if (!options.json && matches.empty()) std::cout << "no matching PCIe endpoints\n";
        return 0;
    }

    if (action == XdmaAction::set_mrrs) {
        if (!requested_mrrs.has_value()) {
            throw Error("the XDMA MRRS action requires a byte count");
        }
        requireRoot();
        const auto bdf = findCurrentBdf(options);
        if (!bdf.has_value()) throw Error("cannot find a matching PCIe endpoint");
        rememberBdf(options.device_index, *bdf);
        setMrrs(*bdf, *requested_mrrs);
        return 0;
    }

    requireRoot();
    auto current = findCurrentBdf(options, true);
    if (!current.has_value()) current = readSavedBdf(options.device_index);

    if (action == XdmaAction::driver_only) {
        unloadModule(options.module);
        loadModule(options);
        if (current.has_value() && std::filesystem::exists(endpointPath(*current))) {
            finishBinding(*current, options);
        } else {
            std::cout << "XDMA module reloaded; no selected endpoint is currently visible\n";
        }
        return 0;
    }
    if (action == XdmaAction::remove) {
        if (!current.has_value() || !std::filesystem::exists(endpointPath(*current))) {
            throw Error("cannot remove an endpoint that is not visible");
        }
        removeForReconfiguration(*current, options);
        return 0;
    }
    if (action == XdmaAction::post_reconfigure) {
        if (!current.has_value()) {
            (void)recoverAfterReconfiguration(std::nullopt, options.bdf.has_value(), options);
        } else {
            reloadAfterIprog(*current, options);
        }
        return 0;
    }
    if (action == XdmaAction::driver_probe) {
        if (current.has_value() && std::filesystem::exists(endpointPath(*current))) {
            reenumerateKnown(*current, options, false, false, true);
        } else {
            (void)recoverAfterReconfiguration(current, options.bdf.has_value(), options);
        }
        return 0;
    }
    if (action == XdmaAction::driver_reload) {
        if (current.has_value() && std::filesystem::exists(endpointPath(*current))) {
            reenumerateKnown(*current, options, false, false, false);
        } else {
            (void)recoverAfterReconfiguration(current, options.bdf.has_value(), options);
        }
        return 0;
    }
    throw Error("unsupported XDMA action");
}

}  // namespace fpgactl
