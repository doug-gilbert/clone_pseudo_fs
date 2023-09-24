#!/bin/bash

# Clone /sys on the current machine to /tmp/sys suitable for the
#    lsscsi --sysfsroot=/tmp/sys
# or
#    lsscsi --sysroot=/tmp
# invocations which are equivalent.
# Alternatively /tmp/sys could be tar-ed up for later inspection.

clone_pseudo_fs -s /sys -d /tmp/sys -E device -E subsystem \
-p /sys/class/nvme -S

# -p /sys/class/scsi_device/ -p /sys/class/scsi_generic/ -p /sys/bus/scsi
# remove the -S if statistics are not required
