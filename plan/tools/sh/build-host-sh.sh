#!/bin/sh
# ws042: builds userland/base/sh for the host (Linux) so that the difference
# tests run in seconds.  The shell uses only POSIX interfaces apart from the
# zedBSD console ioctl and its own readline, both of which build here too.
#   sh plan/tools/sh/build-host-sh.sh [OUTPUT]     (default build/ws042/host-sh)
set -e
out=${1:-build/ws042/host-sh}
mkdir -p "$(dirname "$out")"
# glibc names fpurge __fpurge, in <stdio_ext.h>.
cc -std=c11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -I. -Iinclude -Iuserland/base/libedit \
	-include stdio_ext.h -Dfpurge=__fpurge \
	userland/base/sh/*.c userland/base/libedit/readline.c -o "$out"
echo "$out"
