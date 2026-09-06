/**
 * @file common.h
 * @brief Shared file, value-conversion, and process utilities.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fpgactl {

/** An actionable command, host, image, or hardware error. */
class Error : public std::runtime_error {
public:
    explicit Error(const std::string& message) : std::runtime_error(message) {}
};

/** Result of executing a child without a shell. */
struct ProcessResult {
    int exit_code = -1;
    std::string output;
};

/** Require effective UID zero before a privileged hardware mutation. */
void requireRoot();

/** Parse a C-style base-prefixed unsigned integer and enforce an upper bound. */
std::uint64_t parseUnsigned(
    const std::string& value,
    std::string_view name,
    std::uint64_t maximum = UINT64_MAX);

/** Parse a positive floating-point duration in seconds. */
double parsePositiveSeconds(const std::string& value, std::string_view name);

/** Split a comma/whitespace-separated list, dropping empty elements. */
std::vector<std::string> splitList(const std::string& value);

/** Parse a quote-aware argument string without invoking a shell. */
std::vector<std::string> splitCommandArguments(const std::string& value);

/** Read a text file, returning fallback on an ordinary read failure. */
std::string readText(
    const std::filesystem::path& path,
    const std::string& fallback = "unknown");

/** Read an entire binary file or throw a contextual Error. */
std::vector<std::uint8_t> readBinary(const std::filesystem::path& path);

/** Write an entire binary file, optionally refusing replacement. */
void writeBinary(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes,
    bool overwrite);

/** Resolve the final component of a symlink, or return no value. */
std::optional<std::string> resolvedName(const std::filesystem::path& path);

/** Execute argv directly and combine stdout/stderr into the result. */
ProcessResult runProcess(const std::vector<std::string>& command, bool echo_output = false);

/** Execute argv directly and throw when it exits unsuccessfully. */
void runChecked(const std::vector<std::string>& command);

/** Return whether an executable can be located using PATH. */
bool commandExists(const std::string& executable);

/** Compute a lowercase SHA-256 using the system's core hashing utility. */
std::string sha256File(const std::filesystem::path& path);

/** Quote an argv vector for diagnostics only; it is never evaluated by a shell. */
std::string displayCommand(const std::vector<std::string>& command);

/** Poll a predicate until it succeeds or the specified timeout expires. */
template<typename Predicate>
void waitUntil(
    Predicate predicate,
    std::chrono::milliseconds timeout,
    const std::string& description,
    std::chrono::milliseconds interval = std::chrono::milliseconds(200)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw Error("timeout waiting for " + description);
        }
        std::this_thread::sleep_for(interval);
    }
}

}  // namespace fpgactl
