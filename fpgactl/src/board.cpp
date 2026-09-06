/**
 * @file board.cpp
 * @brief Board-layout interpretation composed from reusable FPGA IP drivers.
 */

#include "board.h"

#include "common.h"

#include "mrs_fpga_dev/ip_drivers/gpio.h"
#include "mrs_fpga_dev/ip_drivers/device_dna.h"
#include "mrs_fpga_dev/ip_drivers/xadc.h"
#include "mrs_fpga_dev/xdma.h"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>

namespace fpgactl {
namespace {

using mrs::fpga::AxiGpio;
using mrs::fpga::AxiGpioChannel;
using mrs::fpga::DeviceDna;
using mrs::fpga::Xadc;

/** One coherent snapshot after assigning board meanings to individual IPs. */
struct BoardSnapshot {
    std::string product_id;
    std::uint32_t version = 0;
    std::uint64_t device_dna = 0;
    double temperature_celsius = 0.0;
    double vccint_volts = 0.0;
    double vccaux_volts = 0.0;
};

/** Convert the board's MSB-first channel-1 identifier into printable text. */
std::string decodeProductId(std::uint32_t value) {
    std::string result{
        static_cast<char>((value >> 24U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>(value & 0xffU),
    };
    const auto terminator = result.find('\0');
    if (terminator != std::string::npos) result.resize(terminator);
    return result;
}

/** Escape a product identifier for a syntactically valid JSON string. */
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
                if (character < 0x20U || character >= 0x7fU) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<unsigned int>(character) << std::dec;
                } else {
                    output << character;
                }
        }
    }
    return output.str();
}

/**
 * Assign image-specific board meanings while delegating register mechanics to
 * representative IP drivers.  Individual commands read only the IP registers
 * they need; the show command deliberately collects the complete snapshot.
 */
class BoardAccess {
public:
    BoardAccess(mrs::fpga::RegisterAccess& registers, BoardLayout layout)
        : gpio_(registers, layout.gpio_base, true),
          dna_(registers, layout.dna_base),
          xadc_(registers, layout.xadc_base) {}

    std::string productId() const {
        return decodeProductId(gpio_.readData(AxiGpioChannel::channel1));
    }

    std::uint32_t version() const {
        return gpio_.readData(AxiGpioChannel::channel2);
    }

    std::uint64_t deviceDna() const { return dna_.value(); }
    double temperatureCelsius() const { return xadc_.temperatureCelsius(); }
    double vccintVolts() const { return xadc_.vccintVolts(); }
    double vccauxVolts() const { return xadc_.vccauxVolts(); }

    BoardSnapshot snapshot() const {
        return {
            productId(),
            version(),
            deviceDna(),
            temperatureCelsius(),
            vccintVolts(),
            vccauxVolts(),
        };
    }

private:
    AxiGpio gpio_;
    DeviceDna dna_;
    Xadc xadc_;
};

}  // namespace

int runBoard(const BoardCommand& command) {
    const std::string device = command.device.value_or(
        "/dev/xdma" + std::to_string(command.device_index) + "_user");
    mrs::fpga::XDMAUser registers(device);
    const BoardAccess board(registers, command.layout);

    if (command.action == BoardAction::name) {
        std::cout << board.productId() << '\n';
    } else if (command.action == BoardAction::version) {
        std::cout << "0x" << std::hex << std::setw(8) << std::setfill('0')
                  << board.version() << std::dec << '\n';
    } else if (command.action == BoardAction::dna) {
        std::cout << "0x" << std::hex << std::setw(16) << std::setfill('0')
                  << board.deviceDna() << std::dec << '\n';
    } else if (command.action == BoardAction::temperature) {
        std::cout << std::fixed << std::setprecision(3)
                  << board.temperatureCelsius() << " C\n";
    } else if (command.action == BoardAction::voltages) {
        std::cout << std::fixed << std::setprecision(4)
                  << "VCCINT: " << board.vccintVolts() << " V\n"
                  << "VCCAUX: " << board.vccauxVolts() << " V\n";
    } else if (command.action == BoardAction::show && command.json) {
        const BoardSnapshot snapshot = board.snapshot();
        std::cout << "{\n"
                  << "  \"product_id\": \"" << jsonEscape(snapshot.product_id) << "\",\n"
                  << "  \"version\": " << snapshot.version << ",\n"
                  << "  \"device_dna\": \"0x" << std::hex << std::setw(16)
                  << std::setfill('0') << snapshot.device_dna << std::dec << "\",\n"
                  << "  \"temperature_celsius\": " << std::fixed << std::setprecision(6)
                  << snapshot.temperature_celsius << ",\n"
                  << "  \"vccint_volts\": " << snapshot.vccint_volts << ",\n"
                  << "  \"vccaux_volts\": " << snapshot.vccaux_volts << "\n}\n";
    } else if (command.action == BoardAction::show) {
        const BoardSnapshot snapshot = board.snapshot();
        std::cout << "product:      " << snapshot.product_id << '\n'
                  << "version:      0x" << std::hex << std::setw(8) << std::setfill('0')
                  << snapshot.version << '\n'
                  << "device DNA:   0x" << std::setw(16) << snapshot.device_dna << std::dec << '\n'
                  << std::fixed << std::setprecision(3)
                  << "temperature:  " << snapshot.temperature_celsius << " C\n"
                  << std::setprecision(4)
                  << "VCCINT:       " << snapshot.vccint_volts << " V\n"
                  << "VCCAUX:       " << snapshot.vccaux_volts << " V\n";
    }
    return 0;
}

}  // namespace fpgactl
