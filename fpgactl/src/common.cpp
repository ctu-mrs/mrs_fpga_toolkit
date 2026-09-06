/**
 * @file common.cpp
 * @brief Safe implementation of shared host-side utilities.
 */

#include "common.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fpgactl {

void requireRoot() {
    if (::geteuid() != 0) {
        throw Error("this operation must be run as root");
    }
}

std::uint64_t parseUnsigned(
    const std::string& value,
    std::string_view name,
    std::uint64_t maximum) {
    if (value.empty() || value.front() == '-') {
        throw Error("invalid " + std::string(name) + ": " + value);
    }
    std::size_t consumed = 0;
    unsigned long long parsed = 0;
    try {
        parsed = std::stoull(value, &consumed, 0);
    } catch (const std::exception&) {
        throw Error("invalid " + std::string(name) + ": " + value);
    }
    if (consumed != value.size() || parsed > maximum) {
        throw Error(std::string(name) + " is outside its accepted range: " + value);
    }
    return static_cast<std::uint64_t>(parsed);
}

double parsePositiveSeconds(const std::string& value, std::string_view name) {
    std::size_t consumed = 0;
    double result = 0.0;
    try {
        result = std::stod(value, &consumed);
    } catch (const std::exception&) {
        throw Error("invalid " + std::string(name) + ": " + value);
    }
    if (consumed != value.size() || !std::isfinite(result) || result <= 0.0) {
        throw Error(std::string(name) + " must be a positive finite number");
    }
    return result;
}

std::vector<std::string> splitList(const std::string& value) {
    std::vector<std::string> result;
    std::string item;
    for (const char character : value) {
        if (character == ',' || std::isspace(static_cast<unsigned char>(character))) {
            if (!item.empty()) {
                result.push_back(std::move(item));
                item.clear();
            }
        } else {
            item.push_back(character);
        }
    }
    if (!item.empty()) {
        result.push_back(std::move(item));
    }
    return result;
}

std::vector<std::string> splitCommandArguments(const std::string& value) {
    std::vector<std::string> result;
    std::string word;
    char quote = '\0';
    bool escaped = false;
    bool started = false;
    for (const char character : value) {
        if (escaped) {
            word.push_back(character);
            escaped = false;
            started = true;
        } else if (character == '\\' && quote != '\'') {
            escaped = true;
            started = true;
        } else if (quote != '\0') {
            if (character == quote) {
                quote = '\0';
            } else {
                word.push_back(character);
            }
            started = true;
        } else if (character == '\'' || character == '"') {
            quote = character;
            started = true;
        } else if (std::isspace(static_cast<unsigned char>(character))) {
            if (started) {
                result.push_back(std::move(word));
                word.clear();
                started = false;
            }
        } else {
            word.push_back(character);
            started = true;
        }
    }
    if (escaped || quote != '\0') {
        throw Error("unterminated quote or escape in command arguments");
    }
    if (started) {
        result.push_back(std::move(word));
    }
    return result;
}

std::string readText(const std::filesystem::path& path, const std::string& fallback) {
    std::ifstream input(path);
    if (!input) {
        return fallback;
    }
    std::ostringstream output;
    output << input.rdbuf();
    std::string result = output.str();
    while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back()))) {
        result.pop_back();
    }
    return result;
}

std::vector<std::uint8_t> readBinary(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw Error("cannot open file for reading: " + path.string());
    }
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length < 0) {
        throw Error("cannot determine file length: " + path.string());
    }
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> result(static_cast<std::size_t>(length));
    if (!result.empty()) {
        input.read(reinterpret_cast<char*>(result.data()), length);
    }
    if (!input) {
        throw Error("failed while reading file: " + path.string());
    }
    return result;
}

void writeBinary(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes,
    bool overwrite) {
    if (!overwrite && std::filesystem::exists(path)) {
        throw Error("output exists: " + path.string() + "; use --force to overwrite");
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw Error("cannot open file for writing: " + path.string());
    }
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    if (!output) {
        throw Error("failed while writing file: " + path.string());
    }
}

std::optional<std::string> resolvedName(const std::filesystem::path& path) {
    std::error_code error;
    const auto resolved = std::filesystem::canonical(path, error);
    if (error) {
        return std::nullopt;
    }
    return resolved.filename().string();
}

ProcessResult runProcess(const std::vector<std::string>& command, bool echo_output) {
    if (command.empty()) {
        throw Error("cannot execute an empty command");
    }
    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) {
        throw Error("cannot create child output pipe: " + std::string(::strerror(errno)));
    }
    const pid_t child = ::fork();
    if (child < 0) {
        const int saved_errno = errno;
        (void)::close(pipe_fds[0]);
        (void)::close(pipe_fds[1]);
        throw Error("cannot create child process: " + std::string(::strerror(saved_errno)));
    }
    if (child == 0) {
        (void)::close(pipe_fds[0]);
        (void)::dup2(pipe_fds[1], STDOUT_FILENO);
        (void)::dup2(pipe_fds[1], STDERR_FILENO);
        (void)::close(pipe_fds[1]);
        std::vector<char*> argv;
        argv.reserve(command.size() + 1);
        for (const auto& item : command) {
            argv.push_back(const_cast<char*>(item.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(argv.front(), argv.data());
        const std::string message =
            "cannot execute " + command.front() + ": " + std::string(::strerror(errno)) + "\n";
        const ssize_t ignored = ::write(STDERR_FILENO, message.data(), message.size());
        (void)ignored;
        _exit(127);
    }

    (void)::close(pipe_fds[1]);
    ProcessResult result;
    char buffer[4096];
    while (true) {
        const ssize_t count = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (count > 0) {
            result.output.append(buffer, static_cast<std::size_t>(count));
            if (echo_output) {
                std::cout.write(buffer, count);
                std::cout.flush();
            }
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            const int saved_errno = errno;
            (void)::close(pipe_fds[0]);
            throw Error("cannot read child output: " + std::string(::strerror(saved_errno)));
        }
    }
    (void)::close(pipe_fds[0]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            throw Error("cannot wait for child process: " + std::string(::strerror(errno)));
        }
    }
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

void runChecked(const std::vector<std::string>& command) {
    const ProcessResult result = runProcess(command, true);
    if (result.exit_code != 0) {
        throw Error(
            "command exited with status " + std::to_string(result.exit_code) + ": " +
            displayCommand(command));
    }
}

bool commandExists(const std::string& executable) {
    if (executable.find('/') != std::string::npos) {
        return ::access(executable.c_str(), X_OK) == 0;
    }
    const char* raw_path = std::getenv("PATH");
    if (raw_path == nullptr) {
        return false;
    }
    std::stringstream paths(raw_path);
    std::string directory;
    while (std::getline(paths, directory, ':')) {
        const auto candidate = std::filesystem::path(directory.empty() ? "." : directory) / executable;
        if (::access(candidate.c_str(), X_OK) == 0) {
            return true;
        }
    }
    return false;
}

std::string sha256File(const std::filesystem::path& path) {
    const ProcessResult result = runProcess({"sha256sum", "--", path.string()});
    if (result.exit_code != 0 || result.output.size() < 64) {
        throw Error("cannot compute SHA-256 for " + path.string());
    }
    const std::string digest = result.output.substr(0, 64);
    if (!std::all_of(digest.begin(), digest.end(), [](unsigned char character) {
            return std::isxdigit(character) != 0;
        })) {
        throw Error("hash utility returned malformed output for " + path.string());
    }
    return digest;
}

std::string displayCommand(const std::vector<std::string>& command) {
    std::ostringstream output;
    bool first = true;
    for (const auto& item : command) {
        if (!first) {
            output << ' ';
        }
        first = false;
        output << std::quoted(item);
    }
    return output.str();
}

}  // namespace fpgactl
