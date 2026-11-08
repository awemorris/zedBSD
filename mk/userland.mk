# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Userland Build Rules
#

ifeq ($(strip $(CONFIG_USERLAND_PROGRAMS)),)
CONFIG_USERLAND_PROGRAMS := $(USERLAND_PROGRAMS)
endif

override CONFIG_USERLAND_PROGRAMS := $(sort $(CONFIG_USERLAND_PROGRAMS) sh init cron getty syslogd networkd mount fsck sysctl net)

ALL_PROGRAM_MAKEFILES := $(sort \
	$(wildcard userland/base/*/Makefile) \
	$(wildcard userland/base/*/*/Makefile) \
	$(wildcard userland/desktop/*/Makefile) \
	$(wildcard userland/desktop/*/*/Makefile) \
	$(wildcard userland/packages/*/*/Makefile) \
	$(wildcard userland/firmware/*/Makefile) \
	$(wildcard userland/x11/*/Makefile) \
	$(wildcard userland/tests/*/Makefile))
ALL_PROGRAM_MAKEFILES := $(filter-out %/build-%/Makefile,$(ALL_PROGRAM_MAKEFILES))

pkg_aliases = $(strip $(1) \
	$(if $(filter ime,$(1)),keiland-ime) \
	$(if $(filter dict,$(1)),ime-dict-ja) \
	$(if $(filter skk-dict,$(1)),ime-dict-skk) \
	$(if $(filter libGL,$(1)),libgl) \
	$(if $(filter libwayland,$(1)),libwayland-client) \
	$(if $(filter preview,$(1)),keiland-preview) \
	$(if $(filter printd,$(1)),keiland-printd) \
	$(if $(filter installer,$(1)),install-admin) \
	$(if $(filter wallpapers,$(1)),wayland) \
	$(1)-firmware $(1)-test)

SELECTED_PROGRAM_MAKEFILES := $(foreach mf, $(ALL_PROGRAM_MAKEFILES), \
	$(if $(filter $(call pkg_aliases,$(notdir $(patsubst %/,%,$(dir $(mf))))), $(CONFIG_USERLAND_PROGRAMS)), \
		$(mf)))

PROGRAM_DIRS := $(dir $(SELECTED_PROGRAM_MAKEFILES))
ALL_PROGRAM_DIRS := $(dir $(ALL_PROGRAM_MAKEFILES))

PROGRAM_STAMPS := $(foreach d,$(PROGRAM_DIRS),$(BUILD)/packages/$(notdir $(patsubst %/,%,$(d)))/.stamp)

define PROGRAM_RULE
_CHILD_DIRS_$(1) := $$(filter $(1)%/,$$(filter-out $(1),$$(ALL_PROGRAM_DIRS)))
_CHILD_SRCS_$(1) := $$(foreach c,$$(_CHILD_DIRS_$(1)),$$(wildcard $$(c)* $$(c)*/* $$(c)*/*/*) $$(patsubst %/,%,$$(c)))
_ALL_SRCS_$(1) := $$(wildcard $(1)* $(1)*/* $(1)*/*/*)
_DIR_NAMES_$(1) := $$(patsubst %/.,%,$$(wildcard $$(addsuffix /.,$$(_ALL_SRCS_$(1)))))
_PKG_SRCS_$(1) := $$(filter-out $$(_CHILD_SRCS_$(1)) $$(_DIR_NAMES_$(1)),$$(_ALL_SRCS_$(1)))

$$(BUILD)/packages/$(notdir $(patsubst %/,%,$(1)))/.stamp: $$(_PKG_SRCS_$(1)) userland/base/package.mk
	@case "$(1)" in \
		userland/packages/*) \
			$$(MAKE) -C $(1) ;; \
		*) \
			$$(MAKE) -C $(1) CC="$$(CC)" CPPFLAGS="$$(USER_CPPFLAGS)" CFLAGS="$$(USER_CFLAGS)" AS="$$(AS)" LD="$$(LD)" RANLIB="$$(RANLIB)" NM="$$(NM)" OBJCOPY="$$(OBJCOPY)" OBJDUMP="$$(OBJDUMP)" READELF="$$(READELF)" STRIP="$$(STRIP)" ;; \
	esac
	@mkdir -p $$(dir $$@) && touch $$@

.PHONY: $(1) $(patsubst %/,%,$(1))
$(1) $(patsubst %/,%,$(1)):
	@case "$(1)" in \
		userland/packages/*) \
			$$(MAKE) -C $(1) ;; \
		*) \
			$$(MAKE) -C $(1) CC="$$(CC)" CPPFLAGS="$$(USER_CPPFLAGS)" CFLAGS="$$(USER_CFLAGS)" AS="$$(AS)" LD="$$(LD)" RANLIB="$$(RANLIB)" NM="$$(NM)" OBJCOPY="$$(OBJCOPY)" OBJDUMP="$$(OBJDUMP)" READELF="$$(READELF)" STRIP="$$(STRIP)" ;; \
	esac
endef

$(foreach d,$(ALL_PROGRAM_DIRS),$(eval $(call PROGRAM_RULE,$(d))))

.PHONY: packages

programs: libc $(PROGRAM_STAMPS)


#
# Special Dependency Rules
#

# Base dependencies
$(BUILD)/packages/sh/.stamp $(BUILD)/packages/net/.stamp: $(BUILD)/packages/libedit/.stamp
$(BUILD)/packages/curses/.stamp $(BUILD)/packages/vi/.stamp $(BUILD)/packages/terminfo-extra/.stamp: $(BUILD)/packages/terminfo/.stamp
$(BUILD)/packages/fidoctl/.stamp $(BUILD)/packages/ssh/.stamp $(BUILD)/packages/libssl-compat/.stamp: $(BUILD)/packages/libcrypto-compat/.stamp
$(BUILD)/packages/passkey-fido2/.stamp: $(BUILD)/packages/passkey/.stamp $(BUILD)/packages/libcrypto-compat/.stamp
$(BUILD)/packages/noct/.stamp: $(BUILD)/packages/libegl/.stamp $(BUILD)/packages/libglesv2/.stamp

# Library dependencies
$(BUILD)/packages/libvulkan/.stamp: $(BUILD)/packages/libwayland/.stamp
$(BUILD)/packages/libpng-compat/.stamp: $(BUILD)/packages/libz-compat/.stamp
$(BUILD)/packages/libpdf/.stamp: $(BUILD)/packages/libz-compat/.stamp $(BUILD)/packages/libjpeg-compat/.stamp $(BUILD)/packages/libtruetype/.stamp
$(BUILD)/packages/libkeiland/.stamp: $(BUILD)/packages/libwayland/.stamp $(BUILD)/packages/libtruetype/.stamp $(BUILD)/packages/libvulkan/.stamp $(BUILD)/packages/libpng-compat/.stamp $(BUILD)/packages/libz-compat/.stamp
$(BUILD)/packages/libegl/.stamp: $(BUILD)/packages/libvulkan/.stamp $(BUILD)/packages/libwayland/.stamp $(BUILD)/packages/libwayland-egl/.stamp
$(BUILD)/packages/libglesv2/.stamp: $(BUILD)/packages/libegl/.stamp
$(BUILD)/packages/libGL/.stamp: $(BUILD)/packages/libegl/.stamp
$(BUILD)/packages/libbrowser/.stamp: $(BUILD)/packages/libtruetype/.stamp $(BUILD)/packages/libvulkan/.stamp $(BUILD)/packages/libmedia/.stamp $(BUILD)/packages/libjpeg-compat/.stamp $(BUILD)/packages/libpng-compat/.stamp $(BUILD)/packages/libz-compat/.stamp $(BUILD)/packages/libgif-compat/.stamp $(BUILD)/packages/libwebp-compat/.stamp
$(BUILD)/packages/libGLX/.stamp: $(BUILD)/packages/libGL/.stamp $(BUILD)/packages/libegl/.stamp

# Desktop app dependencies
KEILAND_COMMON_DEPS := $(BUILD)/packages/libvulkan/.stamp $(BUILD)/packages/libwayland/.stamp $(BUILD)/packages/libkeiland/.stamp $(BUILD)/packages/libtruetype/.stamp
$(BUILD)/packages/settings/.stamp:    $(KEILAND_COMMON_DEPS) $(BUILD)/packages/preview/.stamp
$(BUILD)/packages/terminal/.stamp:    $(KEILAND_COMMON_DEPS)
$(BUILD)/packages/files/.stamp:       $(KEILAND_COMMON_DEPS) $(BUILD)/packages/preview/.stamp
$(BUILD)/packages/browser/.stamp:     $(KEILAND_COMMON_DEPS) $(BUILD)/packages/libbrowser/.stamp $(BUILD)/packages/libz-compat/.stamp
$(BUILD)/packages/imageview/.stamp:   $(KEILAND_COMMON_DEPS) $(BUILD)/packages/libz-compat/.stamp $(BUILD)/packages/libpng-compat/.stamp $(BUILD)/packages/libjpeg-compat/.stamp $(BUILD)/packages/libgif-compat/.stamp
$(BUILD)/packages/textedit/.stamp:    $(KEILAND_COMMON_DEPS) $(BUILD)/packages/libpng-compat/.stamp $(BUILD)/packages/libz-compat/.stamp
$(BUILD)/packages/pdfviewer/.stamp:   $(KEILAND_COMMON_DEPS) $(BUILD)/packages/libpdf/.stamp $(BUILD)/packages/libz-compat/.stamp
$(BUILD)/packages/notes/.stamp:       $(KEILAND_COMMON_DEPS) $(BUILD)/packages/libpdf/.stamp $(BUILD)/packages/libz-compat/.stamp $(BUILD)/packages/libpng-compat/.stamp $(BUILD)/packages/libjpeg-compat/.stamp $(BUILD)/packages/libgif-compat/.stamp
$(BUILD)/packages/photos/.stamp:      $(KEILAND_COMMON_DEPS) $(BUILD)/packages/mediastorage/.stamp $(BUILD)/packages/preview/.stamp $(BUILD)/packages/libz-compat/.stamp
$(BUILD)/packages/music/.stamp:       $(KEILAND_COMMON_DEPS) $(BUILD)/packages/libmedia/.stamp $(BUILD)/packages/libz-compat/.stamp $(BUILD)/packages/libpng-compat/.stamp $(BUILD)/packages/libjpeg-compat/.stamp $(BUILD)/packages/libgif-compat/.stamp
$(BUILD)/packages/videoplayer/.stamp: $(KEILAND_COMMON_DEPS) $(BUILD)/packages/libmedia/.stamp
$(BUILD)/packages/calendar/.stamp:    $(KEILAND_COMMON_DEPS)
$(BUILD)/packages/mailer/.stamp:      $(KEILAND_COMMON_DEPS)
$(BUILD)/packages/monitor/.stamp:     $(KEILAND_COMMON_DEPS)
$(BUILD)/packages/phone/.stamp:       $(KEILAND_COMMON_DEPS) $(BUILD)/packages/preview/.stamp $(BUILD)/packages/libz-compat/.stamp
$(BUILD)/packages/mediastorage/.stamp: $(BUILD)/packages/libkeiland/.stamp $(BUILD)/packages/libwayland/.stamp $(BUILD)/packages/preview/.stamp
$(BUILD)/packages/ime/.stamp:         $(BUILD)/packages/libwayland/.stamp $(BUILD)/packages/libkeiland/.stamp
$(BUILD)/packages/sessiond/.stamp:    $(BUILD)/packages/passkey/.stamp
$(BUILD)/packages/xserver/.stamp:     $(KEILAND_COMMON_DEPS) $(BUILD)/packages/libGL/.stamp
$(BUILD)/packages/wayland/.stamp:     $(BUILD)/packages/libvulkan/.stamp $(BUILD)/packages/libtruetype/.stamp $(BUILD)/packages/libkeiland/.stamp $(BUILD)/packages/sessiond/.stamp $(BUILD)/packages/mediastorage/.stamp $(BUILD)/packages/libpng-compat/.stamp $(BUILD)/packages/libz-compat/.stamp $(BUILD)/packages/libjpeg-compat/.stamp

# Test dependencies
$(BUILD)/packages/kuidemo/.stamp:       $(KEILAND_COMMON_DEPS)
$(BUILD)/packages/wltest/.stamp:        $(BUILD)/packages/libvulkan/.stamp $(BUILD)/packages/libwayland/.stamp
$(BUILD)/packages/mview/.stamp:         $(BUILD)/packages/libvulkan/.stamp $(BUILD)/packages/libwayland/.stamp
$(BUILD)/packages/wlshm/.stamp:         $(BUILD)/packages/libwayland/.stamp $(BUILD)/packages/libkeiland/.stamp
$(BUILD)/packages/printtest/.stamp:     $(BUILD)/packages/libwayland/.stamp $(BUILD)/packages/libkeiland/.stamp
$(BUILD)/packages/egltest/.stamp:       $(BUILD)/packages/libegl/.stamp $(BUILD)/packages/libglesv2/.stamp $(BUILD)/packages/libwayland-egl/.stamp $(BUILD)/packages/libwayland/.stamp
$(BUILD)/packages/gpudemo/.stamp:       $(BUILD)/packages/libegl/.stamp $(BUILD)/packages/libglesv2/.stamp
$(BUILD)/packages/glescompute/.stamp:   $(BUILD)/packages/libegl/.stamp $(BUILD)/packages/libglesv2/.stamp
$(BUILD)/packages/vkdemo/.stamp:        $(BUILD)/packages/libvulkan/.stamp
$(BUILD)/packages/vkcompute/.stamp:     $(BUILD)/packages/libvulkan/.stamp
$(BUILD)/packages/vkimage/.stamp:       $(BUILD)/packages/libvulkan/.stamp
$(BUILD)/packages/vkstage/.stamp:       $(BUILD)/packages/libvulkan/.stamp
$(BUILD)/packages/vkmatrix/.stamp:      $(BUILD)/packages/libvulkan/.stamp
$(BUILD)/packages/vkreset/.stamp:       $(BUILD)/packages/libvulkan/.stamp $(BUILD)/packages/vkcompute/.stamp
$(BUILD)/packages/gpu-fence/.stamp:     $(BUILD)/packages/libvulkan/.stamp
$(BUILD)/packages/acquire-fence/.stamp: $(BUILD)/packages/libvulkan/.stamp $(BUILD)/packages/libwayland/.stamp
