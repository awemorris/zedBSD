# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include $(dir $(lastword $(MAKEFILE_LIST)))../../package.mk

LLVM_RUNTIME_LICENSE_DATA := \
	/usr/share/licenses/llvm/LICENSE.TXT=build/llvm/share/licenses/llvm/LICENSE.TXT

# Every maintained x86 image links LLVM compiler-rt builtins. Keep the Apache
# 2.0 WITH LLVM-exception notice in the distributed root filesystem while the
# same notice also travels with the host-tool cache itself.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,llvm-runtime-license,LLVM runtime license,\
	i386 pc98 amd64,n,data,,n,base,base/licenses/llvm-runtime,,,data,\
	$(LLVM_RUNTIME_LICENSE_DATA)))
