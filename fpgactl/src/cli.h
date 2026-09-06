/**
 * @file cli.h
 * @brief Declarative command-line interface for fpgactl.
 */

#pragma once

namespace fpgactl {

/** Build, parse, and dispatch the complete argparse command tree. */
int runCli(int argc, char** argv);

}  // namespace fpgactl
