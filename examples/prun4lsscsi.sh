#!/bin/bash

# Clone /sys on the current machine to /tmp/sys suitable for the
#    lsscsi --sysfsroot=/tmp/sys
# or
#    lsscsi --sysroot=/tmp
# invocations which are equivalent.
# Alternatively /tmp/sys could be tar-ed up for later inspection.

set -x #echo on

clone_pseudo_fs -s /sys -d /tmp/sys -E device -E subsystem -E power \
-p /sys/class/scsi_device -p /sys/class/scsi_generic -p /sys/bus/scsi \
-p /sys/class/scsi_disk -p /sys/class/scsi_host -p /sys/class/nvme -S

# remove the -S if statistics are not required

# To exclude the NVMe devices, remove the '-p /sys/class/nvme' option.
