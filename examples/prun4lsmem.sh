#!/bin/bash

# Clone /sys on the current machine to /tmp/sys suitable for
#    lsmem --sysroot=/tmp
# Alternatively /tmp/sys could be tar-ed up for later inspection.

set -x #echo on

clone_pseudo_fs -p /sys/devices/system/memory -e /sys/bus -E subsystem \
-E device -E power -S

# remove the -S if statistics are not required.
