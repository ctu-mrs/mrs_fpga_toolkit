/**
 * @file pcie.h
 * @brief Generic PCIe discovery and XDMA lifecycle operations.
 */

#pragma once

#include "common.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fpgactl {

/** Common endpoint-selection and XDMA recovery options. */
struct PcieOptions {
    std::optional<std::string> bdf;
    int device_index = 0;
    std::optional<std::uint16_t> vendor_id = 0x10ee;
    std::optional<std::uint16_t> device_id;
    std::string module = "xdma";
    std::vector<std::string> module_arguments;
    std::vector<std::string> required_nodes{"control"};
    std::chrono::milliseconds timeout{30'000};
    std::optional<unsigned int> device_mode = 0666;
    bool json = false;
    bool include_all = false;
};

/** Operations implemented directly by the XDMA/PCIe command family. */
enum class XdmaAction {
    show,
    list,
    driver_reload,
    driver_probe,
    driver_only,
    post_reconfigure,
    remove,
    set_mrrs,
};

/** Normalize `[domain:]bus:device.function` to a lowercase domain-qualified BDF. */
std::string normalizeBdf(const std::string& value);

/** Find an endpoint through explicit selection, XDMA class links, or IDs. */
std::optional<std::string> findCurrentBdf(
    const PcieOptions& options, bool allow_absent_explicit = false);

/** Read the best-effort BDF hint retained across endpoint disappearance. */
std::optional<std::string> readSavedBdf(int device_index);

/** Store a best-effort BDF hint under /run. */
void rememberBdf(int device_index, const std::string& bdf);

/** Remove a live endpoint and unload XDMA before whole-device programming. */
void removeForReconfiguration(const std::string& bdf, const PcieOptions& options);

/** Rescan for an absent endpoint, load XDMA, and wait for required nodes. */
std::string recoverAfterReconfiguration(
    const std::optional<std::string>& preferred_bdf,
    bool strict_bdf,
    const PcieOptions& options);

/** Recover a known BDF after an AXI HWICAP IPROG transition. */
void reloadAfterIprog(const std::string& bdf, const PcieOptions& options);

/** Execute a parsed XDMA/PCIe action. */
int runXdma(
    XdmaAction action,
    const PcieOptions& options,
    std::optional<unsigned int> requested_mrrs = std::nullopt);

}  // namespace fpgactl
