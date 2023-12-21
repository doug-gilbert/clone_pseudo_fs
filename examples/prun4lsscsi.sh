#!/bin/bash

# Clone /sys and /dev on the current machine to /tmp/sys and /tmp/dev
# suitable for this invocation:
#    lsscsi --sysroot=/tmp
#
# Root permissions are needed to clone the char and block nodes in
# /dev so that those nodes appear in /tmp/dev

set -x #echo on

clone_pseudo_fs -s /sys -d /tmp/sys -E device -E subsystem -E power \
-p /sys/class/scsi_device -p /sys/class/scsi_generic -p /sys/bus/scsi \
-p /sys/class/scsi_disk -p /sys/class/scsi_host \
-p /sys/class/nvme -p /sys/class/nvme-subsystem -S

# remove the -S if statistics are not required

# To exclude the NVMe devices, remove the '-p /sys/class/nvme' and
# '-p /sys/devices/virtual/nvme-subsystem' options.

# Now clone /dev to /tmp/dev . Note that nodes like /dev/null will not
# be cloned to /tmp/dev/null unless root permission are active.
clone_pseudo_fs -s /dev -d /tmp/dev -w 0 -S

# If root permission are not given, the output of this invocation:
#    lsscsi --sysroot=/tmp;
# [0:0:0:0]    disk    Linux    scsi_debug       0191  -        
# [N:0:1:1]    disk    SKHynix_HFS512GDE999999__1      -        
# [N:1:1:1]    disk    Linux__1                        - 
#
# compare that with this invocation:
#    lsscsi --sysroot=/tmp;
# [0:0:0:0]    disk    Linux    scsi_debug       0191  /dev/sda 
# [N:0:1:1]    disk    SKHynix_HFS512GDE999999__1      /dev/nvme0n1        
# [N:1:1:1]    disk    Linux__1                        /dev/nvme1n1
#
# Using this invocation:
#    tar czf dev_non_root.tar.gz /dev
# May work well enough to be useful. The device nodes in the /dev
# directory are recorded in dev_non_root.tar.gz tarball. To extract
# them will require root permissions. The "well enough" warning is
# because many alternate names for those device nodes are in
# subdirectories that cannot be read by non-root users.
