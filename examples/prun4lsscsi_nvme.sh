#!/bin/bash

# This script will only find NVMe devices with lsscsi. There is
# a comment below how to add SCSI devices to lsscsi's output
#
# Clone /sys and /dev on the current machine to /tmp/sys and /tmp/dev
# suitable for this invocation:
#    lsscsi --sysroot=/tmp
#
# Root permissions are needed to clone the char and block node in
# /dev so that those nodes appear in /tmp/dev

set -x #echo on

clone_pseudo_fs -s /sys -d /tmp/sys -E device -E subsystem -E power \
-p /sys/class/nvme -p /sys/class/nvme-subsystem -S

# remove the -S if statistics are not required

# The following commented out line can be added to the above invocation
# to fetch SCSI devices as well as NVMe devices:
# -p /sys/class/scsi_device/ -p /sys/class/scsi_generic/ -p /sys/bus/scsi


clone_pseudo_fs -s /dev -d /tmp/dev -w 0 $@
