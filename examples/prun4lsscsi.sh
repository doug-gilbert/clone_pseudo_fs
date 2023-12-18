#!/bin/bash

# Clone /sys and /dev on the current machine to /tmp/sys and /tmp/dev
# suitable for this invocation:
#    lsscsi --sysroot=/tmp
#
# Root permissions are needed to clone the char and block node in
# /dev so that those nodes appear in /tmp/dev

set -x #echo on

clone_pseudo_fs -s /sys -d /tmp/sys -E device -E subsystem -E power \
-p /sys/class/scsi_device -p /sys/class/scsi_generic -p /sys/bus/scsi \
-p /sys/class/scsi_disk -p /sys/class/scsi_host \
-p /sys/class/nvme -p /sys/class/nvme-subsystem -S

# remove the -S if statistics are not required

# To exclude the NVMe devices, remove the '-p /sys/class/nvme' and
# '-p /sys/devices/virtual/nvme-subsystem' options.

clone_pseudo_fs -s /dev -d /tmp/dev -w 0 $@
