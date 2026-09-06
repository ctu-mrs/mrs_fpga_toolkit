# MRS FPGA Toolkit

The repository produces three Debian packages:

- `mrs-fpga-dkms` installs the Xilinx XDMA kernel module and device rules.
- `mrs-fpga-dev` installs the C++ shared library and public headers.
- `mrs-fpgactl` installs the a `fpgactl` executable (depends on `mrs-fpga-dev`).

## Development library

Public headers are under `include/mrs_fpga_dev/`:

```text
mrs_fpga_dev/
├── xdma.h                       XDMA user and H2C/C2H transport
├── ip_driver_lite.h             AXI-Lite driver base class
└── ip_drivers/
    ├── gpio.h                   AXI GPIO driver
    ├── axis2mm.h                AXI stream-to-memory DMA driver
    ├── aximm2s.h                AXI memory-to-stream DMA driver
    ├── device_dna.h             FPGA Device DNA driver
    ├── hwicap.h                 FPGA IPROG driver
    ├── qspi.h                   AXI Quad SPI driver
    └── xadc.h                   AXI XADC driver
```

For details about the AMD IPs:

- `gpio.h` follows [AXI GPIO PG144](https://docs.amd.com/r/en-US/pg144-axi-gpio).
- `xadc.h` follows [XADC Wizard PG091](https://docs.amd.com/v/u/en-US/pg091-xadc-wiz).
- `qspi.h` follows [AXI Quad SPI PG153](https://docs.amd.com/r/en-US/pg153-axi-quad-spi).
- `hwicap.h` follows [AXI HWICAP PG134](https://docs.amd.com/r/en-US/pg134-axi-hwicap)
  and the 7-series IPROG/WBSTAR sequence in
  [UG470](https://docs.amd.com/v/u/en-US/ug470_7Series_Config).
- `device_dna.h` reads the 57-bit 7-series `DNA_PORT` value described by UG470.
- `aximm2s.h` and `axis2mm.h` - see [wb2axip repo](https://github.com/ZipCPU/wb2axip)

User projects should use this CMake target:

```cmake
find_package(mrs_fpga_dev 1.0 REQUIRED CONFIG)
target_link_libraries(my_application PRIVATE mrs_fpga::mrs_fpga)
```

And include the libraries in user code like:

```cpp
#include <mrs_fpga_dev/xdma.h>
#include <mrs_fpga_dev/ip_driver_lite.h>
```

## `fpgactl` utility

Use `fpgactl --help`, `fpgactl xdma --help`, or `fpgactl jtag --help` for details.
