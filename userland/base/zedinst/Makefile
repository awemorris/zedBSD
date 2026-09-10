# zedBSD installer package; runtime operations are owned by the Noct modules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

ZEDINST_MODULES := admission commands confirmation copy destination discovery files graphicmain graphicui \
 identity install main metadata nativeadmit nativecommand nativediscovery nativeguid \
 nativeinstall nativemeasure nativeplan notify pc98 preflight result rootimage rootview selection source \
 textui transaction treecopy treeprogress wizard workspace
ZEDINST_DATA := $(foreach module,$(ZEDINST_MODULES),\
 /lib/zedinst/$(module).noct=userland/base/zedinst/$(module).noct)
ZEDINST_DATA += $(foreach asset,menu menu-focus review review-focus progress progress-focus,\
 /lib/zedinst/assets/$(asset).bmp=userland/base/zedinst/assets/$(asset).bmp)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,zedinst,zedinst,amd64 pc98,n,package,,y,base,base/zedinst,base/noct,0755,program,$(ZEDINST_DATA),,sbin))

ifdef ZEDBSD_TOPLEVEL_BUILD
ZEDBSD_PACKAGE_INPUTS += $(if $(filter zedinst,$(ZEDBSD_USER_PROGRAMS)),$(BUILD)/bin/zedinst $(BUILD)/bin/zedinst-graphic)
ZEDBSD_PACKAGE_FILES += $(if $(filter zedinst,$(ZEDBSD_USER_PROGRAMS)),--file /sbin/zedinst=$(BUILD)/bin/zedinst --file /sbin/zedinst-graphic=$(BUILD)/bin/zedinst-graphic)

$(BUILD)/bin/zedinst: userland/base/zedinst/zedinst.sh $(BUILD)/bin/zedinst-graphic
	@mkdir -p $(dir $@)
	cp $< $@
	chmod 0755 $@

$(BUILD)/bin/zedinst-graphic: userland/base/zedinst/zedinst-graphic.sh
	@mkdir -p $(dir $@)
	cp $< $@
	chmod 0755 $@
else
.PHONY: install-graphic
install: install-graphic
install-graphic: build
	$(INSTALL) -d "$(DESTDIR)$(ZEDBSD_STANDALONE_BINDIR)"
	$(INSTALL) -m 0755 zedinst-graphic.sh "$(DESTDIR)$(ZEDBSD_STANDALONE_BINDIR)/zedinst-graphic"
endif
