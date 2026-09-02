# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include $(dir $(lastword $(MAKEFILE_LIST)))../../package.mk

RTL8822B_TABLES_LICENSE_DATA := \
	/usr/share/licenses/rtl8822b-tables/LICENSE=userland/base/licenses/rtl8822b-tables/LICENSE

# This package is selected automatically when the kernel RTL8822BU driver is
# enabled.  It is deliberately hidden from menuconfig: a binary containing the
# imported table data must also contain the applicable BSD-3-Clause notice.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,rtl8822b-tables-license,RTL8822B table license,\
	i386 pc98 amd64,n,data,,n,base,base/licenses/rtl8822b-tables,,,data,\
	$(RTL8822B_TABLES_LICENSE_DATA)))
