#!/bin/bash

# Clone /sys /dev and /proc on the current machine to similarly named
# directories under /tmp suitable for
#    lsblk --sysroot=/tmp
# Alternatively /tmp/sys /tmp/dev and /tmp/proc could be tar-ed up
# for later inspection.

# A few tricks required for lsblk which needs a clone of /sys , /dev
# and /proc . A dereference of the symlink /proc/self is needed
# as lsblk looks for /proc/self/mountinfo which is a long-ish regular
# hence the '-r 8192'. Even though the target of /proc/self is a PID
# directory (e.g. /proc/1234 ), the dereference is applied before the
# exclude

set -x #echo on

# The '-w 0' (wait for 0 milliseconds) handles waiting reads such as
# /proc/kmsg
# clone_pseudo_fs -s /proc -d /tmp/proc -w 0 -p /proc/self -r 8192
# Here is another approach:
mkdir /tmp/proc
clone_pseudo_fs -s /proc/self -d /tmp/proc/self $@

clone_pseudo_fs -s /dev -d /tmp/dev -w 0 $@

clone_pseudo_fs -s /sys -d /tmp/sys -p /sys/block -p /sys/class/block \
-p /sys/dev/block -E subsystem -E device -S $@

# remove the -S if statistics are not required, or add it as in the
# first two invocations
