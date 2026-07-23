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

Linux XVC Server Application for Xilinx XVC Over PCIe
=====================================================

September 10, 2018

Contents
========

- Building and Running
- Connecting Vivado to the xvcserver Application

Building and Running
====================

Make sure the XVC enabled PCIe device and XVC enabled PCIe driver was properly 
installed before attempting to run the xvcserver application. The xvcserver 
should be run on the same machine where the device and driver were installed.

To build and run this application:

1. Make sure the firewall settings on the system expose the port that will be
   used to connect to Vivado. For this example port 10200 will be 
   used. Please see the OS help pages for ifnormation regarding the firewall
   port settings for your OS.
 
2. Move the source files to the directory of your choice.  For example, use 
   /home/username/xil_xvc or /usr/local/src/xil_xvc.

3. Change to the directory containing the application source files:

      # cd /xvcserver/

4. Compile the driver module:

      # make

   Use XVCPCIE_VERSION to specify build version in Xilinx banner during xvc_pcie startup:
      # make XVCPCIE_VERSION=2018.3

5. Start the application:

      # ./bin/xvc_pcie -s TCP::10200

   After the application is started you will not see any print until Vivado 
   attempts to connect to the xvcserver application.

   Once Vivado has connected to the xvcserver application you should see the 
   following message get printed by xvc_server.

      "Opening /dev/xil_xvc/cfg_ioc0"

   If the xvc_pcie_user_config.h in the driver has been modified the character
   file name may have changed depending on the settings.  To target a different
   character use the -d <character file path> option.
   
   Example:

      # ./bin/xvc_pcie -s TCP::10200 -d /dev/xil_xvc/cfg_ioc0_tree1
 
Connecting Vivado to the xvcserver Application
==============================================

Vivado can be running on the computer running the xvcserver application or it 
can be running remotely on another computer that is connected on the network, 
but the xvcserver port must be accessible to the machine running Vivado.

1. Launch Vivado.

2. Select "Open HW Manager".

3. Start Vivado hw_server using the following command in the Vivado .tcl console:

      # connect_hw_server

4. Connect to the Xilinx Virtual Cable server application using the following 
   command in the Vivado .tcl console.

      # open_hw_target -xvc_url <HostName or IP Address>:<port number>

   The Vivado Hardware panel will be populated with a debug bridge instance.

   Note: At this point the xvcserver application should acknowledging the Vivado 
   connection by printing:

      "Opening /dev/xil_xvc/cfg_ioc0"

5. Select the debug bridge instance from the Vivado Hardware panel.

6. In the Hardware Devcie Properties window select the appropriate "Probes file" by:
      - clicking on the "..." icon next to the "Probes File" entry, 
      - selecting the appropriate probes file, and
      - and clicking "OK".

    This will also refresh the hardware device and it will now show the debug 
    cores present in the design and Vivado debug can be performed.

