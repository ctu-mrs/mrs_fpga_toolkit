cmake_minimum_required(VERSION 3.16)

# Standalone builds use this default.  Package builds override the cache entry
# with the version read from the exact mrs-fpga-dev artifact they consume.
set(MRS_FPGACTL_VERSION "1.0.0" CACHE STRING "fpgactl release version")
project(
  mrs_fpgactl
  DESCRIPTION "Unified control utility for MRS FPGA devices"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(FetchContent)

find_package(mrs_fpga_dev REQUIRED CONFIG)

# argparse is header-only. Pin the v3.2 release commit so package builds use
# the same parser implementation even when the upstream default branch moves.
set(ARGPARSE_INSTALL OFF CACHE BOOL "" FORCE)
set(ARGPARSE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ARGPARSE_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  argparse
  GIT_REPOSITORY https://github.com/p-ranav/argparse.git
  GIT_TAG 3eda91b2e1ce7d569f84ba295507c4cd8fd96910
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(argparse)

add_executable(fpgactl
  src/main.cpp
  src/cli.cpp
  src/common.cpp
  src/pcie.cpp
  src/board.cpp
  src/jtag.cpp
  src/qspi.cpp)
target_compile_features(fpgactl PRIVATE cxx_std_17)
target_compile_options(fpgactl PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(fpgactl PRIVATE mrs_fpga::mrs_fpga argparse::argparse)
target_compile_definitions(
  fpgactl PRIVATE MRS_FPGACTL_VERSION="${MRS_FPGACTL_VERSION}")

install(TARGETS fpgactl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
