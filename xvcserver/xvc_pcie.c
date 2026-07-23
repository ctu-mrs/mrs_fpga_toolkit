/*********************************************************************
 * Copyright (c) 2017 Xilinx, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **********************************************************************/

/*
 * PCIe xvcserver
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <assert.h>
#include "xvcserver.h"
#include "xvc_pcie_ioctl.h"
#include <unistd.h>

 #include <sys/time.h>

#ifndef _WINDOWS
// TODO: Windows build support
//    sys/ioctl.h is linux only header file
//    it is included for ioctl
#include <sys/ioctl.h>
 #include <errno.h>
#endif

#define BYTE_ALIGN(a) ((a + 7) / 8)
#define MIN(a, b) (a < b ? a : b)
#define MAX(a, b) (a > b ? a : b)

typedef struct {
  int fd;       // open file descriptor 
} pcie_reg_t;

enum xvc_algo_type {
  XVC_ALGO_NULL,
  XVC_ALGO_CFG,
  XVC_ALGO_BAR
};

LoggingMode log_mode = LOG_MODE_DEFAULT;

char dev_node [64];

static const char * usage_text[] = {
  "Usage:\n Name      Description",
  "-------------------------------",
  "[-help]    Show help information",
  "[-s]       Socket listening port and protocol.  Default: TCP::10200",
  "[-d]       Driver character file path. Defualt: /dev/xil_xvc/cfg_ioc0",
  "[-test]    Enable performing xvc loop back test (true/false). Default: true",
  "[-verbose] Show additional messages during execution",
  "[-quiet]   Disable logging all non-error messages during execution",
  "\n",
  NULL
};

static const char * time_stamp = __TIME__;
static const char * date_stamp = __DATE__;

struct xil_xvc_properties xvc_driver_props;

static unsigned findMaxBytes(unsigned *nbits_list)
{
  unsigned max_bits = 0;
  while(*nbits_list)
  {
    max_bits = MAX(max_bits, *nbits_list);
    ++nbits_list;
  }

  return BYTE_ALIGN(max_bits);
}

static void rotatePattern(unsigned char *pattern)
{
  unsigned char c;
  unsigned pat_nbytes = strlen((const char *)pattern);

  c = pattern[0];
  memmove(pattern, pattern + 1, pat_nbytes - 1);

  pattern[pat_nbytes - 1] = c;
}

static int open_port(void *client_data) {
   pcie_reg_t* pcie = (pcie_reg_t*)client_data;
   
   if (log_mode == LOG_MODE_VERBOSE)
     fprintf(stdout, "INFO: Opening %s.\n", dev_node);

   pcie->fd = open(dev_node, O_RDWR | O_SYNC);
   if (pcie->fd < 1) {
      fprintf(stderr,"ERROR: Failed to Open Device\n");
      return (-1);
   }

   return (0);
}

static void close_port(void *client_data) {
   pcie_reg_t* pcie = (pcie_reg_t*)client_data;
   if (pcie->fd >= 1) {
      close(pcie->fd);
   }
}

static void set_tck(void *client_data, unsigned long nsperiod, unsigned long *result) {
    *result = nsperiod;
}

static void shift_tms_tdi(
    void *client_data,
    unsigned long bitcount,
    unsigned char *tms_buf,
    unsigned char *tdi_buf,
    unsigned char *tdo_buf) {

    pcie_reg_t* pcie = (pcie_reg_t*)client_data;
    struct timeval stop, start;

    if (log_mode == LOG_MODE_VERBOSE) {
        gettimeofday(&start, NULL);
    }

    struct xil_xvc_ioc xvc_ioc;

    xvc_ioc.opcode = 0x01; // 0x01 for normal, 0x02 for bypass
    xvc_ioc.length = bitcount;
    xvc_ioc.tms_buf = tms_buf;
    xvc_ioc.tdi_buf = tdi_buf;
    xvc_ioc.tdo_buf = tdo_buf;

    int ret = ioctl(pcie->fd, XDMA_IOCXVC, &xvc_ioc);
    if (ret < 0)
    {
        int errsv = errno;
        fprintf(stderr, "IOC Error %d\n", errsv);
    }

    if (log_mode == LOG_MODE_VERBOSE) {
        gettimeofday(&stop, NULL);
        fprintf(stdout, "IOC shift internal took %lu u-seconds with %lu bits. Return value %d\n", stop.tv_usec - start.tv_usec, bitcount, ret);
    }
}

static int read_driver_properties(void *client_data) {
  int ret = 0;
  //pcie_reg_t* pcie;

  if (open_port(client_data) < 0) {
    fprintf(stderr, "ERROR: Opening JTAG port failed\n");
    ret = -1;
    return ret;
  }

  //pcie = (pcie_reg_t*) client_data;
  //ret = ioctl(pcie->fd, XDMA_RDXVC_PROPS, &xvc_driver_props);
  ret = 0;
  xvc_driver_props.bar_index = 0;
  xvc_driver_props.bar_offset = 0x40000;
  xvc_driver_props.config_vsec_id = 0x10EE;
  xvc_driver_props.config_vsec_rev = 0x0001;
  xvc_driver_props.xvc_algo_type = XVC_ALGO_BAR;

  if (ret < 0)
  {
    if (log_mode == LOG_MODE_VERBOSE)
      fprintf(stderr, "WARNING: Could not read driver XVC properties. Returned error - %s\n", strerror(errno));
    return ret;
  } 
  close_port(client_data);
  ret = 0;

  return ret;
}

static void display_driver_properties(void) {
  fprintf(stdout, "INFO: XVC PCIe Driver character file - %s\n", dev_node);
  if (xvc_driver_props.xvc_algo_type == XVC_ALGO_CFG) {
    fprintf(stdout, "INFO: XVC PCIe Driver configured to communicate with Debug Bridge IP in PCIe mode (PCIe CONFIG space).\n");
    fprintf(stdout, "INFO: PCIe XVC VSEC ID=0x%04X and PCIe XVC VSEC Rev ID=0x%04X\n", xvc_driver_props.config_vsec_id, xvc_driver_props.config_vsec_rev);
  } else if (xvc_driver_props.xvc_algo_type == XVC_ALGO_BAR) {
    fprintf(stdout, "INFO: XVC PCIe Driver configured to communicate with Debug Bridge IP in AXI mode (PCIe BAR space).\n");
    fprintf(stdout, "INFO: PCIe BAR index=0x%04X and PCIe BAR offset=0x%04X\n", xvc_driver_props.bar_index, xvc_driver_props.bar_offset);
  } else {
    fprintf(stderr, "WARNING: XVC PCIe driver configured in invalid mode. XVC_ALGO_TYPE=%d\n", xvc_driver_props.xvc_algo_type);
  }
}

static void display_banner(int display_props) {
    fprintf(stdout, "\nDescription:\n");
    fprintf(stdout, "Xilinx xvc_pcie v%s\n", XVCPCIE_VERSION);
    fprintf(stdout, "Build date : %s-%s\n", date_stamp, time_stamp);
    fprintf(stdout, "Copyright 1986-2018 Xilinx, Inc. All Rights Reserved.\n\n");
    if (display_props)
      display_driver_properties();
}

static inline void print_usage(void) {
    const char ** p = usage_text;
    while (*p != NULL) {
        fprintf(stdout, "%s\n", *p++);
    }
}

static void display_help(void) {
    fprintf(stdout, "\nDescription:\n");
    fprintf(stdout, "Xilinx xvc_pcie v%s\n", XVCPCIE_VERSION);
    fprintf(stdout, "Build date : %s-%s\n", date_stamp, time_stamp);
    fprintf(stdout, "Copyright 1986-2018 Xilinx, Inc. All Rights Reserved.\n\n");
    fprintf(stdout, "Syntax:\nxvc_pcie [-help] [-s <arg>] [-d <arg>] [-test <arg>] [-verbose] [-quiet]\n\n");
    print_usage();    
}

static int test_xvc_driver(int fd, struct xil_xvc_ioc *xvc_ioc, unsigned cmd_nbits, unsigned char *pattern)
{
  unsigned char *tms_buf = xvc_ioc->tms_buf;
  unsigned char *tdi_buf = xvc_ioc->tdi_buf;
  unsigned char *tdo_buf = xvc_ioc->tdo_buf;
  struct timeval stop, start;
  long delta_us;
  double mbps;
  unsigned cmd_nbytes;
  unsigned pat_nbytes;
  unsigned fill_nbytes;
  unsigned vb = 0;

  cmd_nbytes = BYTE_ALIGN(cmd_nbits);
  pat_nbytes = strlen((const char *)pattern);

  // setup tms_buf
  memset(tms_buf, 0xff, cmd_nbytes);

  // copy pattern into tdi_buf
  fill_nbytes = 0;
  while (fill_nbytes < cmd_nbytes)
  {
    unsigned nbytes = MIN(pat_nbytes, cmd_nbytes);
    memcpy(tdi_buf + fill_nbytes, pattern, nbytes);
    fill_nbytes += nbytes;
  }

  // reset tdo_buf
  memset(tdo_buf, 0, cmd_nbytes);

  // set up ioctl codes
  xvc_ioc->opcode = 0x02; // 0x01 for normal, 0x02 for bypass
  xvc_ioc->length = cmd_nbits;

  // start timer
    gettimeofday(&start, NULL);

  // run the test
  int ret = ioctl(fd, XDMA_IOCXVC, xvc_ioc);
  if (ret < 0)
  {
    fprintf(stderr, "Could not run the command test bitlen %d\n", cmd_nbits);
    fprintf(stderr, "Error: %s\n", strerror(errno));
    return -1;
  }

  // stop timer
    gettimeofday(&stop, NULL);

    if (stop.tv_usec < start.tv_usec)
      delta_us = 1000000 - start.tv_usec + stop.tv_usec;
    else
      delta_us = stop.tv_usec - start.tv_usec;
    delta_us += 1000000 * (stop.tv_sec - start.tv_sec);

    mbps = (double) cmd_nbits / (double)(delta_us);

  // verify tdo
  while (vb < cmd_nbits)
  {
    unsigned nbits = MIN(cmd_nbits - vb, 8);
    unsigned mask = 0xFF;
    unsigned index = vb / 8;

    mask >>= (8 - nbits);
    if ((tdi_buf[index] & mask) != (tdo_buf[index] & mask))
    {
      fprintf(stderr, "Loopback test length: %d, pattern %s FAILURE\n", cmd_nbits, pattern);
      fprintf(stderr, "\tByte %d did not match (0x%02X != 0x%02X mask 0x%02X), pattern %s\n", 
              index, tdi_buf[index] & mask, tdo_buf[index] & mask, mask, pattern);
      return -1;
    }
    vb += nbits;
  }

  if(log_mode == LOG_MODE_VERBOSE)
    fprintf(stdout, "INFO: Loopback test length: %d bits, %ld us, %.2f Mbps SUCCESS\n", cmd_nbits, delta_us, mbps);

  return 0;
}

static int perform_loopback_test(void *client_data) {
  int ret = 0;
  pcie_reg_t* pcie;
  struct xil_xvc_ioc xvc_ioc;
  unsigned char pattern[] = "abcdefgHIJKLMOP";

  // unsigned test_lens[] = {1, 4, 6, 12, 24, 32, 64, 89, 144, 233, 
  //  377, 610, 987, 1597, 2584, 4096, 0x2000, 0x800000, 0};
  unsigned test_lens[] = {32, 0};
  unsigned test_index = 0;
  unsigned max_nbytes;

  max_nbytes = findMaxBytes(test_lens);

  // set up buffers with the maximum size to be tested
  xvc_ioc.tms_buf = (unsigned char *) malloc(max_nbytes);
  xvc_ioc.tdi_buf = (unsigned char *) malloc(max_nbytes);
  xvc_ioc.tdo_buf = (unsigned char *) malloc(max_nbytes);
  if (!xvc_ioc.tms_buf || !xvc_ioc.tdi_buf || !xvc_ioc.tdo_buf)
  {
    fprintf(stderr, "Could not allocate %d bytes for buffers\n", max_nbytes);
    fprintf(stderr, "Error: %s\n", strerror(errno));
    return 0;
  }

  if (open_port(client_data) < 0) {
    fprintf(stderr, "ERROR: Opening JTAG port failed\n");
    ret = -1;
    return ret;
  }

  pcie = (pcie_reg_t*) client_data;

    // run tests
  while (test_lens[test_index])
  {
    if (test_xvc_driver(pcie->fd, &xvc_ioc, test_lens[test_index], pattern) < 0) {
      fprintf(stderr, "ERROR: XVC PCIE Driver Loopback test failed. Error: %s\n", strerror(errno));
      ret = -1;
      return ret;
    }
    ++test_index;
    rotatePattern(pattern);
  }

  // if we get this far we must be good
  if (log_mode != LOG_MODE_QUIET)
    fprintf(stdout, "INFO: XVC PCIE Driver Loopback test successful.\n");
  ret = 0;

  close_port(client_data);
  return ret;
}

XvcServerHandlers handlers = {
    open_port,
    close_port,
    set_tck,
    shift_tms_tdi,
    NULL,
    NULL,
    NULL,
    NULL
};

int main(int argc, char **argv) {
    const char * url = "tcp::10200";
    strncpy(dev_node, "/dev/xil_xvc/cfg_ioc0", sizeof(dev_node) - 1);
    pcie_reg_t pcie_reg;
    char loopback_arg[8];
    int enable_loopback_test = 1;
    int i = 1;
    int display_props = 1;
    int quiet = 0;
    int verbose = 0;


    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "option -s requires an argument\n");
                return ERROR_INVALID_ARGUMENT;
            }
            url = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "option -d requires an argument\n");
                return ERROR_INVALID_ARGUMENT;
            }
            strncpy(dev_node, argv[++i], sizeof(dev_node) - 1);
        } else if (strcmp(argv[i], "-verbose") == 0) {
            verbose = 1;
            if (quiet) {
              fprintf(stderr, "Using option -verbose along with -quiet is not supported.\n");
              return ERROR_INVALID_ARGUMENT;
            }
            log_mode = LOG_MODE_VERBOSE;
        } else if (strcmp(argv[i], "-test") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "option -test requires an argument true/false\n");
                return ERROR_INVALID_ARGUMENT;
            }
            strncpy(loopback_arg, argv[++i], sizeof(loopback_arg) - 1);
            if (strcmp(loopback_arg, "false") == 0) {
              enable_loopback_test = 0;
            } else if (strcmp(loopback_arg, "true") == 0) {
              enable_loopback_test = 1;
            } else {
                fprintf(stderr, "option -test requires an argument true/false\n");
                return ERROR_INVALID_ARGUMENT; 
            }
        } else if (strcmp(argv[i], "-quiet") == 0) {
            quiet = 1;
            if (verbose) {
              fprintf(stderr, "Using option -verbose along with -quiet is not supported.\n");
              return ERROR_INVALID_ARGUMENT;
            }
            log_mode = LOG_MODE_QUIET;
        } else if (strcmp(argv[i], "-help") == 0 ) {
            display_help();
            return ERROR_INVALID_ARGUMENT;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            display_help();
            return ERROR_INVALID_ARGUMENT;
        }
        i++;
    }

    // Read XVC PCIe driver user configuration properties
    if (read_driver_properties(&pcie_reg) < 0) {
        // Disable displaying driver properties in xvc_pcie banner
        display_props = 0;
    }

    if (log_mode != LOG_MODE_QUIET)
      display_banner(display_props);

    if (enable_loopback_test == 1) {
      if (perform_loopback_test(&pcie_reg) < 0) {
        fprintf(stderr, "Exiting xvc_pcie application.\n");
        return ERROR_LOOPBACK_TEST_FAILED;
      }
    }

    if (log_mode != LOG_MODE_QUIET) {
      fprintf(stdout, "\nINFO: xvc_pcie application started\n");
      fprintf(stdout, "INFO: Use Ctrl-C to exit xvc_pcie application\n\n");
    }
    return xvcserver_start(url, &pcie_reg, &handlers, log_mode);
}