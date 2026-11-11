include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The system's GPU compute demonstration (WS101, the scene S13): Noct's program run on the CPU and on the GPU, and the script
# that compares them.  It needs noct built with ZEDBSD_NOCT_ACCEL := y (the configuration selects noct; this package
# does not require it, so that an image given its noct from elsewhere does not build one), libegl and libglesv2.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
GPUDEMO_DATA := \
	/usr/share/gpudemo/mix.nct=userland/tests/gpudemo/mix.nct \
	/usr/share/gpudemo/s13.sh=userland/tests/gpudemo/s13.sh
$(eval $(call USERLAND_PACKAGE,gpudemo,GPU compute demonstration,x86_64,n,package,,y,tests,tests/gpudemo,desktop/libegl desktop/libglesv2,,data,$(GPUDEMO_DATA),,))
