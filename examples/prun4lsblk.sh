#!/bin/sh
# nothing fancy from bash required so go with a simple shell

# Clone /sys /dev and /proc on the current machine to similarly named
# directories under /tmp suitable for
#    lsblk --sysroot=/tmp
# Alternatively /tmp/sys /tmp/dev and /tmp/proc could be tar-ed up
# for later inspection.

# A few tricks required for lsblk which needs a clone of /sys , /dev
# and /proc . A dereference of the symlink /proc/self is needed
# as lsblk looks for /proc/self/mountinfo which is a long-ish regular
# file hence the '-r 8192'. Even though the target of /proc/self is a PID
# directory (e.g. /proc/1234 ), the dereference is applied before the
# exclude

set -x #echo on

# The '-w 0' (wait for 0 milliseconds) handles waiting reads such as
# /proc/kmsg
# clone_pseudo_fs -s /proc -d /tmp/proc -w 0 -p /proc/self -r 8192
# Here is another approach:
mkdir /tmp/proc

# Insert two linefeeds quietly
{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

clone_pseudo_fs -s /proc/self -d /tmp/proc/self -r 8192 -S

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

clone_pseudo_fs -s /dev -d /tmp/dev -w 0 -S

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

clone_pseudo_fs -s /sys -d /tmp/sys -p /sys/block -p /sys/class/block \
-p /sys/dev/block -E subsystem -E device -S -S

# remove the -S option if statistics are not required.

# -E is the short form of the --excl-fn= option
# -p is the short form of the --prune= option
# -S is the short form of the --statistics (or --stats) option

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

# This should now work ...
lsblk --sysroot /tmp

# ... but in Ubuntu 26.04 LTS apparmor breaks lsblk! The security folks would
# argue but they break the lsblk --sysroot option _without_ explaining why
# (e.g. in 'man lsblk'). Rather than smash apparmor (e.g. giving 'apparmor=0'
# to the kernel invocation). Here is a simple, finely tuned approach:

{ set +x; } 2>/dev/null ; echo "" ; echo "" ; set -x

cp /usr/bin/lsblk /tmp
/tmp/lsblk --sysroot /tmp

