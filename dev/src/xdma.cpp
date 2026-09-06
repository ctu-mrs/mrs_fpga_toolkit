/**
 * @file xdma.cpp
 * @brief Linux implementation of checked XDMA character-device access.
 */

#include "mrs_fpga_dev/xdma.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <climits>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace mrs::fpga {
namespace {

/** Render errno immediately so a later cleanup call cannot obscure it. */
std::runtime_error systemError(const std::string& operation, int error_number = errno) {
    return std::runtime_error(
        operation + ": errno=" + std::to_string(error_number) + " (" +
        std::string(::strerror(error_number)) + ")");
}

/** Render a register offset in a compact, fixed-width diagnostic form. */
std::string hexAddress(std::uint32_t address) {
    std::ostringstream output;
    output << std::hex << address;
    return output.str();
}

/**
 * Interrupt a potentially blocking XDMA syscall when its deadline expires.
 *
 * The upstream XDMA character driver can block inside read/write despite an
 * outer poll.  A Linux per-thread POSIX timer therefore delivers SIGUSR2 to
 * the calling thread.  The signal handler is deliberately empty apart from a
 * `sig_atomic_t` flag; its purpose is to make the syscall return EINTR.
 */
class DriverDeadlineInterrupt {
public:
    explicit DriverDeadlineInterrupt(int timeout_ms) {
        fired_ = 0;
        if (timeout_ms < 0) {
            return;
        }

        sigset_t timeout_set;
        ::sigemptyset(&timeout_set);
        ::sigaddset(&timeout_set, SIGUSR2);
        sigset_t previous_mask;
        const int block_error = ::pthread_sigmask(SIG_BLOCK, &timeout_set, &previous_mask);
        if (block_error != 0) {
            throw systemError("failed to block the XDMA deadline signal", block_error);
        }
        if (::sigismember(&previous_mask, SIGUSR2) == 1) {
            (void)::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
            throw std::runtime_error(
                "SIGUSR2 is blocked on the caller thread; an XDMA syscall "
                "deadline cannot interrupt the driver");
        }

        try {
            drain(timeout_set, true);
            static std::once_flag install_once;
            std::call_once(install_once, [] {
                struct sigaction action {};
                action.sa_handler = &DriverDeadlineInterrupt::handleSignal;
                ::sigemptyset(&action.sa_mask);
                if (::sigaction(SIGUSR2, &action, nullptr) != 0) {
                    throw systemError("failed to install the XDMA deadline handler");
                }
            });

            struct sigevent event {};
            event.sigev_notify = SIGEV_THREAD_ID;
            event.sigev_signo = SIGUSR2;
            event._sigev_un._tid = static_cast<pid_t>(::syscall(SYS_gettid));
            if (::timer_create(CLOCK_MONOTONIC, &event, &timer_) != 0) {
                throw systemError("failed to create an XDMA deadline timer");
            }
            timer_created_ = true;

            const std::uint64_t timeout_ns = std::max<std::uint64_t>(
                1, static_cast<std::uint64_t>(timeout_ms) * 1'000'000ULL);
            struct itimerspec specification {};
            specification.it_value.tv_sec = static_cast<time_t>(timeout_ns / 1'000'000'000ULL);
            specification.it_value.tv_nsec = static_cast<long>(timeout_ns % 1'000'000'000ULL);
            // Repeating notifications close the tiny race between the first
            // notification and entry into a blocking driver syscall.
            specification.it_interval.tv_nsec = 1'000'000L;
            if (::timer_settime(timer_, 0, &specification, nullptr) != 0) {
                throw systemError("failed to arm an XDMA deadline timer");
            }
        } catch (...) {
            disarm();
            drain(timeout_set, false);
            (void)::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
            throw;
        }

        const int restore_error = ::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
        if (restore_error != 0) {
            disarm();
            drain(timeout_set, false);
            (void)::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
            throw systemError("failed to restore the XDMA caller signal mask", restore_error);
        }
    }

    ~DriverDeadlineInterrupt() {
        if (!timer_created_) {
            return;
        }
        sigset_t timeout_set;
        ::sigemptyset(&timeout_set);
        ::sigaddset(&timeout_set, SIGUSR2);
        sigset_t previous_mask;
        const int block_error = ::pthread_sigmask(SIG_BLOCK, &timeout_set, &previous_mask);
        disarm();
        if (block_error == 0) {
            drain(timeout_set, false);
            fired_ = 0;
            (void)::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
        }
    }

    DriverDeadlineInterrupt(const DriverDeadlineInterrupt&) = delete;
    DriverDeadlineInterrupt& operator=(const DriverDeadlineInterrupt&) = delete;

    bool fired() const noexcept { return fired_ != 0; }

private:
    static void handleSignal(int) { fired_ = 1; }

    static void drain(const sigset_t& timeout_set, bool throw_on_error) {
        const struct timespec no_wait {};
        while (true) {
            const int signal_number = ::sigtimedwait(&timeout_set, nullptr, &no_wait);
            if (signal_number == SIGUSR2 || (signal_number < 0 && errno == EINTR)) {
                continue;
            }
            if (signal_number < 0 && errno == EAGAIN) {
                return;
            }
            if (!throw_on_error) {
                return;
            }
            throw systemError("failed to drain an XDMA deadline notification");
        }
    }

    void disarm() noexcept {
        if (!timer_created_) {
            return;
        }
        struct itimerspec disabled {};
        (void)::timer_settime(timer_, 0, &disabled, nullptr);
        (void)::timer_delete(timer_);
        timer_created_ = false;
    }

    inline static thread_local volatile sig_atomic_t fired_ = 0;
    timer_t timer_{};
    bool timer_created_ = false;
};

/** Convert a remaining steady-clock duration to a positive millisecond timeout. */
int remainingMilliseconds(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return static_cast<int>(std::clamp<long long>(remaining.count() + 1, 1, INT_MAX));
}

/** Build the conventional user BAR path after validating its numeric index. */
std::string indexedUserPath(int device_index) {
    if (device_index < 0) {
        throw std::invalid_argument("XDMA device index must be nonnegative");
    }
    return "/dev/xdma" + std::to_string(device_index) + "_user";
}

}  // namespace

XDMATimeout::XDMATimeout(const std::string& message) : std::runtime_error(message) {}

XDMAUser::XDMAUser(int device_index) : XDMAUser(indexedUserPath(device_index)) {}

XDMAUser::XDMAUser(std::string device_path) : path_(std::move(device_path)) {
    if (path_.empty()) {
        throw std::invalid_argument("XDMA user device path must not be empty");
    }
    fd_ = ::open(path_.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        throw systemError("failed to open " + path_);
    }
}

XDMAUser::~XDMAUser() { close(); }

XDMAUser::XDMAUser(XDMAUser&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), path_(std::move(other.path_)) {}

XDMAUser& XDMAUser::operator=(XDMAUser&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
        path_ = std::move(other.path_);
    }
    return *this;
}

std::uint32_t XDMAUser::readReg(std::uint32_t address) const {
    if ((address & 0x3U) != 0U) {
        throw std::invalid_argument("unaligned XDMA register read at 0x" + hexAddress(address));
    }
    std::uint32_t value = 0;
    ssize_t result;
    do {
        result = ::pread(fd_, &value, sizeof(value), static_cast<off_t>(address));
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        throw systemError("failed to read XDMA register 0x" + hexAddress(address));
    }
    if (result != static_cast<ssize_t>(sizeof(value))) {
        throw std::runtime_error(
            "short XDMA register read at 0x" + hexAddress(address) + ": expected 4 bytes, got " +
            std::to_string(result));
    }
    return value;
}

void XDMAUser::writeReg(std::uint32_t address, std::uint32_t value) {
    if ((address & 0x3U) != 0U) {
        throw std::invalid_argument("unaligned XDMA register write at 0x" + hexAddress(address));
    }
    ssize_t result;
    do {
        result = ::pwrite(fd_, &value, sizeof(value), static_cast<off_t>(address));
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        throw systemError("failed to write XDMA register 0x" + hexAddress(address));
    }
    if (result != static_cast<ssize_t>(sizeof(value))) {
        throw std::runtime_error(
            "short XDMA register write at 0x" + hexAddress(address) +
            ": expected 4 bytes, got " + std::to_string(result));
    }
}

std::uint32_t XDMAUser::readBlockReg(
    std::uint32_t base, std::uint32_t offset) const {
    if (base > std::numeric_limits<std::uint32_t>::max() - offset) {
        throw std::out_of_range("XDMA block-relative read address overflows 32 bits");
    }
    return readReg(base + offset);
}

void XDMAUser::writeBlockReg(
    std::uint32_t base, std::uint32_t offset, std::uint32_t value) {
    if (base > std::numeric_limits<std::uint32_t>::max() - offset) {
        throw std::out_of_range("XDMA block-relative write address overflows 32 bits");
    }
    writeReg(base + offset, value);
}

const std::string& XDMAUser::path() const noexcept { return path_; }
bool XDMAUser::isOpen() const noexcept { return fd_ >= 0; }

void XDMAUser::close() noexcept {
    if (fd_ >= 0) {
        (void)::close(fd_);
        fd_ = -1;
    }
}

XDMAStream::XDMAStream(int device_index, int h2c_channel, int c2h_channel)
    : XDMAStream(device_index, h2c_channel, c2h_channel, false) {}

XDMAStream::XDMAStream(
    int device_index,
    int h2c_channel,
    int c2h_channel,
    bool c2h_eop_flush)
    : c2h_eop_flush_(c2h_eop_flush && c2h_channel >= 0) {
    if (device_index < 0 || h2c_channel < -1 || c2h_channel < -1) {
        throw std::invalid_argument("XDMA device and channel indices must be nonnegative or -1");
    }
    if (h2c_channel >= 0) {
        h2c_path_ = "/dev/xdma" + std::to_string(device_index) + "_h2c_" +
                    std::to_string(h2c_channel);
        fd_h2c_ = ::open(h2c_path_.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_h2c_ < 0) {
            throw systemError("failed to open " + h2c_path_);
        }
    }
    if (c2h_channel >= 0) {
        c2h_path_ = "/dev/xdma" + std::to_string(device_index) + "_c2h_" +
                    std::to_string(c2h_channel);
        const int flags = O_RDONLY | O_NONBLOCK | O_CLOEXEC |
                          (c2h_eop_flush_ ? O_TRUNC : 0);
        fd_c2h_ = ::open(c2h_path_.c_str(), flags);
        if (fd_c2h_ < 0) {
            const auto error = systemError("failed to open " + c2h_path_);
            closeStreams();
            throw error;
        }
    }
}

XDMAStream::~XDMAStream() { closeStreams(); }

XDMAStream::XDMAStream(XDMAStream&& other) noexcept
    : fd_h2c_(std::exchange(other.fd_h2c_, -1)),
      fd_c2h_(std::exchange(other.fd_c2h_, -1)),
      h2c_path_(std::move(other.h2c_path_)),
      c2h_path_(std::move(other.c2h_path_)),
      c2h_eop_flush_(std::exchange(other.c2h_eop_flush_, false)) {}

XDMAStream& XDMAStream::operator=(XDMAStream&& other) noexcept {
    if (this != &other) {
        closeStreams();
        fd_h2c_ = std::exchange(other.fd_h2c_, -1);
        fd_c2h_ = std::exchange(other.fd_c2h_, -1);
        h2c_path_ = std::move(other.h2c_path_);
        c2h_path_ = std::move(other.c2h_path_);
        c2h_eop_flush_ = std::exchange(other.c2h_eop_flush_, false);
    }
    return *this;
}

void XDMAStream::closeStreams() noexcept {
    if (fd_h2c_ >= 0) {
        (void)::close(fd_h2c_);
        fd_h2c_ = -1;
    }
    if (fd_c2h_ >= 0) {
        (void)::close(fd_c2h_);
        fd_c2h_ = -1;
    }
}

ssize_t XDMAStream::writeH2C(const void* data, std::size_t size, int timeout_ms) {
    if (fd_h2c_ < 0) {
        throw std::runtime_error("H2C stream is not open");
    }
    if (size != 0 && data == nullptr) {
        throw std::invalid_argument("H2C data pointer is null for a nonempty transfer");
    }
    if (size > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
        throw std::length_error("H2C transfer exceeds the operating-system byte-count range");
    }

    DriverDeadlineInterrupt deadline(timeout_ms);
    if (deadline.fired()) {
        throw XDMATimeout("timed out before submitting the H2C stream write");
    }
    const ssize_t written = ::write(fd_h2c_, data, size);
    if (written < 0) {
        if (errno == EINTR) {
            if (deadline.fired()) {
                throw XDMATimeout(
                    "timed out writing H2C; the interrupted packet may be partial and must not be retried");
            }
            throw std::runtime_error(
                "H2C write was interrupted; packet completion is unknown and retry is unsafe");
        }
        throw systemError("H2C write failed");
    }
    if (deadline.fired()) {
        throw XDMATimeout(
            "H2C write reached its deadline while returning; packet completion is unknown");
    }
    if (static_cast<std::size_t>(written) != size) {
        throw std::runtime_error(
            "H2C short write: requested " + std::to_string(size) + ", wrote " +
            std::to_string(written) + "; splitting would change the packet boundary");
    }
    return written;
}

ssize_t XDMAStream::readC2H(void* buffer, std::size_t size, int timeout_ms) {
    if (fd_c2h_ < 0) {
        throw std::runtime_error("C2H stream is not open");
    }
    if (size != 0 && buffer == nullptr) {
        throw std::invalid_argument("C2H destination is null for a nonempty transfer");
    }
    if (size > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
        throw std::length_error("C2H transfer exceeds the operating-system byte-count range");
    }
    DriverDeadlineInterrupt deadline(timeout_ms);
    while (true) {
        if (deadline.fired()) {
            throw XDMATimeout("timed out before submitting the C2H stream read");
        }
        const ssize_t result = ::read(fd_c2h_, buffer, size);
        if (result >= 0) {
            if (deadline.fired()) {
                throw XDMATimeout("C2H read reached its deadline while returning");
            }
            return result;
        }
        if (errno == EINTR) {
            if (deadline.fired()) {
                throw XDMATimeout("timed out reading the C2H stream");
            }
            throw std::runtime_error("C2H read was interrupted before its deadline");
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            throw systemError("C2H read failed");
        }

        struct pollfd descriptor {};
        descriptor.fd = fd_c2h_;
        descriptor.events = POLLIN;
        const int poll_result = ::poll(&descriptor, 1, timeout_ms);
        if (poll_result < 0) {
            if (errno == EINTR && deadline.fired()) {
                throw XDMATimeout("timed out polling the C2H stream");
            }
            throw systemError("C2H poll failed");
        }
        if (deadline.fired()) {
            throw XDMATimeout("timed out polling the C2H stream");
        }
        if (poll_result == 0) {
            return 0;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            throw std::runtime_error(
                "C2H poll reported device error flags 0x" +
                hexAddress(static_cast<std::uint32_t>(descriptor.revents)));
        }
    }
}

void XDMAStream::readExact(void* buffer, std::size_t size, int timeout_ms) {
    if (timeout_ms < 0) {
        throw std::invalid_argument("readExact requires a finite nonnegative timeout");
    }
    if (size != 0 && buffer == nullptr) {
        throw std::invalid_argument("exact C2H destination is null for a nonempty transfer");
    }
    auto* destination = static_cast<std::uint8_t*>(buffer);
    std::size_t completed = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (completed < size) {
        const int remaining = remainingMilliseconds(deadline);
        if (remaining == 0) {
            throw XDMATimeout(
                "timed out after reading " + std::to_string(completed) + " of " +
                std::to_string(size) + " C2H bytes");
        }
        const ssize_t count = readC2H(destination + completed, size - completed, remaining);
        if (count > 0) {
            completed += static_cast<std::size_t>(count);
        }
    }
}

bool XDMAStream::hasH2C() const noexcept { return fd_h2c_ >= 0; }
bool XDMAStream::hasC2H() const noexcept { return fd_c2h_ >= 0; }
bool XDMAStream::c2hEopFlushEnabled() const noexcept { return c2h_eop_flush_; }

}  // namespace mrs::fpga
