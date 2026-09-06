/**
 * @file export.h
 * @brief Shared-library symbol visibility declarations.
 */

#pragma once

#if defined(_WIN32)
#  if defined(MRS_FPGA_BUILDING_LIBRARY)
#    define MRS_FPGA_API __declspec(dllexport)
#  else
#    define MRS_FPGA_API __declspec(dllimport)
#  endif
#else
#  define MRS_FPGA_API __attribute__((visibility("default")))
#endif
