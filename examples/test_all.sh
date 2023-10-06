#!/bin/bash

while true; do
    read -p "About to remove /tmp/sys, /tmp/proc and /tmp/dev; continue? " yn
    case $yn in
        [Yy]* ) break;;
        [Nn]* ) exit;;
        * ) echo "Please answer yes or no.";;
    esac
done

set -x #echo on

echo ""

rm -rf /tmp/sys
rm -rf /tmp/proc
rm -rf /tmp/dev

# Clone /sys on the current machine to /tmp/sys suitable for the
#    lsscsi --sysfsroot=/tmp/sys
# or
#    lsscsi --sysroot=/tmp
# invocations which are equivalent.
# Alternatively /tmp/sys could be tar-ed up for later inspection.

echo ""

clone_pseudo_fs -s /sys -d /tmp/sys -E device -E subsystem \
-p /sys/class/scsi_device/ -p /sys/class/scsi_generic/ -p /sys/bus/scsi \
-p /sys/class/nvme -S $@

echo ""

lsscsi --sysfsroot=/tmp/sys

echo ""
rm -rf /tmp/sys

echo ""

clone_pseudo_fs -s /sys -d /tmp/sys -E device -E subsystem \
-p /sys/class/nvme -S $@

echo ""

lsscsi --sysfsroot=/tmp/sys

echo ""
rm -rf /tmp/sys
echo ""

clone_pseudo_fs -s /proc -d /tmp/proc -p /proc/self -r 8192

clone_pseudo_fs -s /dev -d /tmp/dev -w 0 $@

clone_pseudo_fs -s /sys -d /tmp/sys -p /sys/block -p /sys/class/block \
-p /sys/dev/block/ -E subsystem -E device -S $@

echo ""

lsblk --sysroot=/tmp
echo ""

rm -rf /tmp/sys
rm -rf /tmp/proc
rm -rf /tmp/dev

echo ""

clone_pseudo_fs -p /sys/devices/system/memory -e /sys/bus -E subsystem \
-E device -S $@

echo ""

lsmem --sysroot=/tmp

echo ""

clone_pseudo_fs -s /sys -d /tmp/sys -E device -E subsystem \
-p /sys/class/typec/ -p /sys/class/usb_power_delivery/ -S $@

echo ""

lsucpd --sysfsroot=/tmp/sys -c

echo ""