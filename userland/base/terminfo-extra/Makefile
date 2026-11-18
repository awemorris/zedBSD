# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ZEDBSD_TERMINFO_EXTRA_DATA := userland/base/terminfo-extra/alacritty.zti \
	userland/base/terminfo-extra/dumb.zti \
	userland/base/terminfo-extra/foot.zti \
	userland/base/terminfo-extra/putty.zti \
	userland/base/terminfo-extra/rxvt-unicode.zti
ZEDBSD_TERMINFO_EXTRA_INSTALL := $(foreach file,\
	$(ZEDBSD_TERMINFO_EXTRA_DATA),/lib/terminfo/$(notdir $(file))=$(file))

$(eval $(call USERLAND_PACKAGE,terminfo-extra,libcurses extra terminal definitions,\
	*,y,data,,y,base,base/terminfo-extra,base/terminfo,,data,\
	$(ZEDBSD_TERMINFO_EXTRA_INSTALL)))
