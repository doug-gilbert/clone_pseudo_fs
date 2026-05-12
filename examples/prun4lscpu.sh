#!/bin/sh
rm -rf /tmp/sys /tmp/dev /tmp/proc/

# Clone /sys on the current machine to /tmp/sys suitable for
#    lscpu --sysroot=/tmp
# Alternatively /tmp/sys could be tar-ed up for later inspection.

set -x #echo on

clone_pseudo_fs -p /sys/devices/system/cpu -E subsystem -E device \
-E power -S "$@"

# Insert two linefeeds quietly
{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

# remove the -S if statistics are not required.

# One regular file is needed from /proc so just copy it normally
## mkdir /tmp/proc
## cp -p /proc/cpuinfo /tmp/proc
# or
# The '-w 0' (wait for 0 milliseconds) handles waiting reads such as
# /proc/kmsg . The -e '/proc/[0-9]*' option is not needed but speeds
# the operation considerably.

clone_pseudo_fs -s /proc -d /tmp/proc -w 0 -e '/proc/[0-9]*' \
-p /proc/cpuinfo -r 65536 "$@"

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

lscpu --sysroot /tmp
