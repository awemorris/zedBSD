include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The test of the devices' suspend and resume (ws052-p004): asks /dev/system for KERN_SYSTEM_SLEEP in its devices
# mode and prints the outcome.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
SLEEPCTL_SOURCES := userland/tests/sleepctl/main.c
$(eval $(call USERLAND_PACKAGE,sleepctl,device suspend test,*,n,basic,$(SLEEPCTL_SOURCES),y,tests,tests/sleepctl,,0755,program,,,bin))
