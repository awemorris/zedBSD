# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# thermal shows and sets the machine's power and thermal state through
# /dev/thermal, and restores the saved strategy at boot (the service).
#

PROG := thermal
SRCS := main.c
CLASS := core
DATA := /etc/service.d/thermal=userland/base/thermal/service
INSTALL_DIR := sbin

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk
