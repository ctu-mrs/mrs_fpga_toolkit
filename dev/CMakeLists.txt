cmake_minimum_required(VERSION 3.16)

project(
  mrs_fpga_dev
  VERSION 1.0.0
  DESCRIPTION "MRS C++ userspace support library for FPGA devices"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Threads REQUIRED)

add_library(mrs_fpga SHARED
  src/xdma.cpp
  src/ip_driver_lite.cpp
  src/ip_drivers/gpio.cpp
  src/ip_drivers/axis_dma_common.cpp
  src/ip_drivers/device_dna.cpp
  src/ip_drivers/xadc.cpp
  src/ip_drivers/qspi.cpp
  src/ip_drivers/hwicap.cpp
  src/ip_drivers/aximm2s.cpp
  src/ip_drivers/axis2mm.cpp)
add_library(mrs_fpga::mrs_fpga ALIAS mrs_fpga)

target_compile_features(mrs_fpga PUBLIC cxx_std_17)
target_compile_options(mrs_fpga PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(
  mrs_fpga
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(mrs_fpga PUBLIC Threads::Threads)
set_target_properties(
  mrs_fpga
  PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)
target_compile_definitions(mrs_fpga PRIVATE MRS_FPGA_BUILDING_LIBRARY)

install(
  TARGETS mrs_fpga
  EXPORT mrs_fpga_devTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_package_config_file(
  cmake/mrs_fpga_devConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/mrs_fpga_devConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mrs_fpga_dev)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/mrs_fpga_devConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(
  EXPORT mrs_fpga_devTargets
  FILE mrs_fpga_devTargets.cmake
  NAMESPACE mrs_fpga::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mrs_fpga_dev)
install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/mrs_fpga_devConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/mrs_fpga_devConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mrs_fpga_dev)
