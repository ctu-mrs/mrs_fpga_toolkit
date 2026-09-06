/**
 * @file main.cpp
 * @brief Process entry point for the unified FPGA control utility.
 */

#include "cli.h"
#include "common.h"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    try {
        return fpgactl::runCli(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
