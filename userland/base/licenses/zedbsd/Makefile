# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include $(dir $(lastword $(MAKEFILE_LIST)))../../package.mk

ZEDBSD_LICENSE_DATA := \
	/usr/share/licenses/zedbsd/LICENSE=LICENSE \
	/usr/share/licenses/libc-regex/LICENSE=src/libc/regex/LICENSE

# The project's own license (ws129-p002), the first entry of the image's list of
# licenses.  The zlib license asks for no notice in binaries; the image carries
# it so that every component's license is in one place.  The C library's
# regular expressions (TRE and musl, BSD-2-Clause and MIT, which do ask for the
# notice in binaries) are in every image's libc, so their notice comes with it.
# Selected for every image and hidden from menuconfig.
$(eval $(call USERLAND_PACKAGE,zedbsd-license,zedBSD license,\
	i386 pc98 amd64 rpi4 rpi5,n,data,,n,base,base/licenses/zedbsd,,,data,\
	$(ZEDBSD_LICENSE_DATA)))
