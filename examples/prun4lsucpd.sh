#!/bin/bash

# Clone /sys on the current machine to /tmp/sys suitable for
#    lsucpd --sysfsroot=/tmp/sys
# or tar-ing up /tmp/sys for later inspection.

set -x #echo on

clone_pseudo_fs -s /sys -d /tmp/sys -E device -E subsystem -E power \
-p /sys/class/typec -p /sys/class/usb_power_delivery \
-p /sys/class/power_supply -p  /sys/bus/typec -S

# remove the -S if statistics are not required
