# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# pgrep and pkill select processes by their name or command line, user,
# group, parent, process group or session: pgrep writes their IDs and pkill
# signals them.  The two programs share everything but their main file.
#

PROGS := pgrep pkill

pgrep_SRCS := pgrep.c match.c system.c
pgrep_CLASS := basic

pkill_SRCS := pkill.c match.c system.c
pkill_CLASS := basic

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk
