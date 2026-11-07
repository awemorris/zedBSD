# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

PACKAGE=

build:
	@echo "Building package $(PACKAGE)..."
	cd $(PACKAGE) && make -f build.mk build install
