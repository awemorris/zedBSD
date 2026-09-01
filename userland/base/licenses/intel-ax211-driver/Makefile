# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

INTEL_AX211_DRIVER_LICENSE_DATA := \
	/usr/share/licenses/intel-ax211-driver/LICENSE=userland/base/licenses/intel-ax211-driver/LICENSE

# This package is selected automatically when the kernel AX211 driver is
# enabled.  It is deliberately hidden from menuconfig: binary redistribution
# of the derived driver requires the complete applicable notices.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,intel-ax211-driver-license,Intel AX211 driver license,\
	amd64,n,data,,n,base,base/licenses/intel-ax211-driver,,,data,\
	$(INTEL_AX211_DRIVER_LICENSE_DATA)))
