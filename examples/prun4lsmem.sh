#!/bin/sh
# nothing fancy from bash required so go with a simple shell

# Clone /sys on the current machine to /tmp/sys suitable for
#    lsmem --sysroot=/tmp
# Alternatively /tmp/sys could be tar-ed up for later inspection.

set -x #echo on

clone_pseudo_fs -p /sys/devices/system/memory -e /sys/bus -E subsystem \
-E device -E power -S

# remove the -S if statistics are not required.

# Insert two linefeeds quietly
{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

lsmem --sysroot /tmp
