#!/bin/bash

# Clone /sys on the current machine to /tmp/sys suitable for
#    lsucpd --sysfsroot=/tmp/sys
# or tar-ing up /tmp/sys for later inspection.

clone_pseudo_fs -s /sys -d /tmp/sys -E device -E subsystem \
-p /sys/class/typec/ -p /sys/class/usb_power_delivery/ -S

# remove the -S if statistics are not required
