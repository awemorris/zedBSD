# zedBSD installer package; runtime operations are owned by the Noct modules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

ZEDINST_MODULES := admission commands confirmation copy destination discovery files \
 identity install main metadata nativeadmit nativeplan preflight rootimage rootview selection source transaction treecopy treeprogress workspace
ZEDINST_DATA := $(foreach module,$(ZEDINST_MODULES),\
 /lib/zedinst/$(module).noct=userland/base/zedinst/$(module).noct)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,zedinst,zedinst,amd64,n,package,,y,base,base/zedinst,base/noct,0755,program,$(ZEDINST_DATA)))

ifdef ZEDBSD_TOPLEVEL_BUILD
ZEDBSD_PACKAGE_INPUTS += $(if $(filter zedinst,$(ZEDBSD_USER_PROGRAMS)),$(BUILD)/bin/zedinst)
ZEDBSD_PACKAGE_FILES += $(if $(filter zedinst,$(ZEDBSD_USER_PROGRAMS)),--file /bin/zedinst=$(BUILD)/bin/zedinst)

$(BUILD)/bin/zedinst: userland/base/zedinst/zedinst.sh
	@mkdir -p $(dir $@)
	cp $< $@
	chmod 0755 $@
endif
