#!/bin/bash

# Clone /sys and current machine to /tmp/sys suitable for the
#    lsucpd --sysfsroot=/tmp/sys

clone_pseudo_fs -s /sys -d /tmp/sys -E device -E subsystem \
-p /sys/class/typec/ -p /sys/class/usb_power_delivery/ -S

# remove the -S if statistics are not required
