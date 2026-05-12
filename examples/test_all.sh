#!/bin/bash

while true; do
    read -r -p "About to remove /tmp/sys, /tmp/proc and /tmp/dev; continue? " yn
    case $yn in
        [Yy]* ) break;;
        [Nn]* ) exit;;
        * ) echo "Please answer yes or no.";;
    esac
done

set -x # echo on

# Insert two linefeeds quietly
{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

rm -rf /tmp/sys
rm -rf /tmp/proc
rm -rf /tmp/dev

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

# Clone /sys on the current machine to /tmp/sys suitable for the
#    lsscsi --sysfsroot=/tmp/sys
# or
#    lsscsi --sysroot=/tmp
# invocations which are equivalent.
# Alternatively /tmp/sys could be tar-ed up for later inspection.

clone_pseudo_fs -s /sys -d /tmp/sys -E device -E subsystem -E power \
-p /sys/class/scsi_device -p /sys/class/scsi_generic -p /sys/bus/scsi \
-p /sys/class/scsi_disk -p /sys/class/nvme -S "$@"

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

lsscsi --sysfsroot=/tmp/sys

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

rm -rf /tmp/sys

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

clone_pseudo_fs -s /sys -d /tmp/sys -E device -E subsystem -E power \
-p /sys/class/nvme -S "$@"

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

lsscsi --sysfsroot=/tmp/sys

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x
rm -rf /tmp/sys
{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

clone_pseudo_fs -s /sys -d /tmp/sys -p /sys/devices/system/cpu -E subsystem \
-E device -E power -S "$@"

clone_pseudo_fs -s /proc -d /tmp/proc -w 0 -e '/proc/[0-9]*' \
-p /proc/cpuinfo -r 65536 "$@"

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

lscpu --sysroot=/tmp --json

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x
rm -rf /tmp/sys
rm -rf /tmp/proc
{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

mkdir /tmp/proc
clone_pseudo_fs -s /proc/self -d /tmp/proc/self -r 8192 "$@"

clone_pseudo_fs -s /dev -d /tmp/dev -w 0 "$@"

clone_pseudo_fs -s /sys -d /tmp/sys -p /sys/block -p /sys/class/block \
-p /sys/dev/block -E subsystem -E device -E power -S "$@"

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

lsblk --sysroot=/tmp

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

rm -rf /tmp/sys
rm -rf /tmp/proc
rm -rf /tmp/dev

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

clone_pseudo_fs -p /sys/devices/system/memory -e /sys/bus -E subsystem \
-E device -E power -S "$@"

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

lsmem --sysroot=/tmp

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

clone_pseudo_fs -s /sys -d /tmp/sys -E device -E subsystem -E power \
-p /sys/class/typec -p /sys/class/usb_power_delivery \
-p /sys/class/power_supply -S "$@"

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

lsucpd --sysfsroot=/tmp/sys -c

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x
