/**
 * @file xdma.h
 * @brief RAII access to Xilinx XDMA user and streaming device nodes.
 *
 * The classes in this header provide checked 32-bit AXI-Lite accesses and
 * packet-oriented host-to-card (H2C) and card-to-host (C2H) transfers.  They
 * own their file descriptors, cannot be copied, and may be moved safely.
 */

#pragma once

#include "mrs_fpga_dev/export.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/types.h>

namespace mrs::fpga {

/** Abstract 32-bit register interface implemented by XDMAUser. */
class MRS_FPGA_API RegisterAccess {
public:
    virtual ~RegisterAccess() = default;

    /** Read one naturally aligned little-endian register. */
    virtual std::uint32_t readReg(std::uint32_t address) const = 0;

    /** Write one naturally aligned little-endian register. */
    virtual void writeReg(std::uint32_t address, std::uint32_t value) = 0;
};

/** Raised when an XDMA operation reaches its caller-supplied deadline. */
class MRS_FPGA_API XDMATimeout : public std::runtime_error {
public:
    explicit XDMATimeout(const std::string& message);
};

/**
 * Checked access to `/dev/xdmaN_user`.
 *
 * Register methods reject unaligned accesses before issuing a system call.
 * This object is safe to use from multiple threads because positional I/O
 * does not share or mutate a file offset.
 */
class MRS_FPGA_API XDMAUser final : public RegisterAccess {
public:
    explicit XDMAUser(int device_index = 0);
    explicit XDMAUser(std::string device_path);
    ~XDMAUser() override;

    XDMAUser(const XDMAUser&) = delete;
    XDMAUser& operator=(const XDMAUser&) = delete;
    XDMAUser(XDMAUser&& other) noexcept;
    XDMAUser& operator=(XDMAUser&& other) noexcept;

    std::uint32_t readReg(std::uint32_t address) const override;
    void writeReg(std::uint32_t address, std::uint32_t value) override;

    /** Convenience access relative to an IP block's AXI-Lite base. */
    std::uint32_t readBlockReg(std::uint32_t base, std::uint32_t offset) const;

    /** Convenience write relative to an IP block's AXI-Lite base. */
    void writeBlockReg(
        std::uint32_t base, std::uint32_t offset, std::uint32_t value);

    /** Return the opened character-device path for diagnostics. */
    const std::string& path() const noexcept;

    /** Compatibility-friendly open-state query. */
    bool isOpen() const noexcept;

private:
    void close() noexcept;

    int fd_ = -1;
    std::string path_;
};

/**
 * An optional pair of XDMA streaming channels.
 *
 * Pass `-1` for a direction that is not needed.  H2C writes are intentionally
 * required to complete in one driver call: splitting a packet can alter TLAST
 * semantics.  C2H reads may be short and callers can assemble a larger
 * logical transfer with readExact().
 */
class MRS_FPGA_API XDMAStream final {
public:
    /**
     * Open selected stream channels.
     *
     * Setting c2h_eop_flush opens the upstream XDMA C2H character device with
     * O_TRUNC. The AMD/Xilinx driver interprets that flag as stream EOP flush
     * mode, so a read may complete at TLAST before filling the supplied host
     * buffer. It has no effect when no C2H channel is requested.
     */
    explicit XDMAStream(
        int device_index = 0, int h2c_channel = -1, int c2h_channel = -1);

    /** Open selected channels and explicitly choose the C2H EOP-flush mode. */
    XDMAStream(
        int device_index,
        int h2c_channel,
        int c2h_channel,
        bool c2h_eop_flush);
    ~XDMAStream();

    XDMAStream(const XDMAStream&) = delete;
    XDMAStream& operator=(const XDMAStream&) = delete;
    XDMAStream(XDMAStream&& other) noexcept;
    XDMAStream& operator=(XDMAStream&& other) noexcept;

    /** Close both directions.  Calling this method repeatedly is harmless. */
    void closeStreams() noexcept;

    /** Write one complete H2C packet, optionally bounded by timeout_ms. */
    ssize_t writeH2C(const void* data, std::size_t size, int timeout_ms = -1);

    /** Type-safe vector overload for writeH2C(). */
    template<typename T>
    ssize_t writeH2C(const std::vector<T>& data, int timeout_ms = -1) {
        if (data.size() > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::length_error("typed H2C transfer byte count overflows");
        }
        return writeH2C(data.data(), data.size() * sizeof(T), timeout_ms);
    }

    /** Read one available C2H fragment, returning zero when polling expires. */
    ssize_t readC2H(void* buffer, std::size_t size, int timeout_ms = -1);

    /** Read exactly size bytes before one end-to-end deadline expires. */
    void readExact(void* buffer, std::size_t size, int timeout_ms);

    /** Allocate and fill exactly count values before one end-to-end deadline. */
    template<typename T>
    std::vector<T> readC2H(std::size_t count, int timeout_ms = 2000) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::length_error("typed C2H transfer byte count overflows");
        }
        std::vector<T> result(count);
        readExact(result.data(), result.size() * sizeof(T), timeout_ms);
        return result;
    }

    /** Return whether the corresponding stream direction is open. */
    bool hasH2C() const noexcept;
    bool hasC2H() const noexcept;

    /** Return whether the C2H descriptor was opened in stream EOP-flush mode. */
    bool c2hEopFlushEnabled() const noexcept;

    /** Compatibility-friendly aliases used by existing host applications. */
    bool isH2COpen() const noexcept { return hasH2C(); }
    bool isC2HOpen() const noexcept { return hasC2H(); }

private:
    int fd_h2c_ = -1;
    int fd_c2h_ = -1;
    std::string h2c_path_;
    std::string c2h_path_;
    bool c2h_eop_flush_ = false;
};

}  // namespace mrs::fpga

/**
 * Source-compatible namespace for applications that already use the compact
 * `xdma::XDMAUser` and `xdma::XDMAStream` spellings.
 */
namespace xdma {
using RegisterAccess = ::mrs::fpga::RegisterAccess;
using XDMATimeout = ::mrs::fpga::XDMATimeout;
using XDMAUser = ::mrs::fpga::XDMAUser;
using XDMAStream = ::mrs::fpga::XDMAStream;
}  // namespace xdma
