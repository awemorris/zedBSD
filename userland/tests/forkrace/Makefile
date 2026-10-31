include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The fork and posix_spawn race test: forks and spawns while other threads of the process keep faulting, and checks
# that each returns within a limit and that a fork child's memory is whole.  Test images only.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,forkrace,fork and spawn race test,*,n,basic,userland/tests/forkrace/main.c,y,tests,tests/forkrace,,0755,program,,,bin))
