# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

keiland-linux:
	@KEILAND_ASK=$(KEILAND_ASK) sh mk/tools/keiland-prerequisites.sh check linux
	$(MAKE) -f userland/desktop/keiland-linux.mk all
	@KEILAND_ASK=$(KEILAND_ASK) sh mk/tools/keiland-prerequisites.sh offer-install linux $(MAKE) -f userland/desktop/keiland-linux.mk install $(MAKEOVERRIDES)

keiland-linux-install:
	$(MAKE) -f userland/desktop/keiland-linux.mk install

keiland-freebsd:
	@KEILAND_ASK=$(KEILAND_ASK) sh mk/tools/keiland-prerequisites.sh check freebsd
	$(MAKE) -f userland/desktop/keiland-freebsd.mk all
	@KEILAND_ASK=$(KEILAND_ASK) sh mk/tools/keiland-prerequisites.sh offer-install freebsd $(MAKE) -f userland/desktop/keiland-freebsd.mk install $(MAKEOVERRIDES)

keiland-freebsd-install:
	$(MAKE) -f userland/desktop/keiland-freebsd.mk install

browser-win32:
	$(MAKE) -f apps/browser-win32/Makefile

terminal-win32:
	cmake -S apps/terminal-win32 -B build/mikuterm-x64 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw-x86_64.cmake -DLLVM_MINGW=$$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64
	cmake --build build/mikuterm-x64 --target zip -j4
