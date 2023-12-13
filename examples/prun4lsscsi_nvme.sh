#!/bin/bash

# This script will only find NVMe devices with lsscsi. There is
# a comment below how to add SCSI devices to lsscsi's output
#
# Clone /sys on the current machine to /tmp/sys suitable for the
#    lsscsi --sysfsroot=/tmp/sys
# or
#    lsscsi --sysroot=/tmp
# invocations which are equivalent.
# Alternatively /tmp/sys could be tar-ed up for later inspection.

set -x #echo on

clone_pseudo_fs -s /sys -d /tmp/sys -E device -E subsystem -E power \
-p /sys/class/nvme -p /sys/class/nvme-subsystem -S

# remove the -S if statistics are not required

# The following commented out line can be added to the above invocation
# to fetch SCSI devices as well as NVMe devices:
# -p /sys/class/scsi_device/ -p /sys/class/scsi_generic/ -p /sys/bus/scsi
