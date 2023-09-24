#!/bin/bash

# Clone /sys on the current machine to /tmp/sys suitable for
#    lscpu --sysroot=/tmp
# Alternatively /tmp/sys could be tar-ed up for later inspection.

clone_pseudo_fs -p /sys/devices/system/cpu -E subsystem -E device -S

# remove the -S if statistics are not required.

# One regular file is needed from /proc so just copy it normally
mkdir /tmp/proc
cp -p /proc/cpuinfo /tmp/proc
