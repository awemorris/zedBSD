# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Userland Build Rules
#

include mk/config.mk
include mk/platform-$(PLATFORM)/defaults.mk
include $(ZEDBSD_CONFIGMK)

ifeq ($(strip $(CONFIG_USERLAND_PROGRAMS)),)
CONFIG_USERLAND_PROGRAMS := $(USERLAND_PROGRAMS)
endif

# The programs every image has.  networkd runs ifconfig, route and dhcpc
# to configure an interface, so they come with it.
override CONFIG_USERLAND_PROGRAMS := $(sort $(CONFIG_USERLAND_PROGRAMS) sh init cron getty syslogd networkd mount fsck sysctl net ifconfig route dhcpc)

# The packages whose programs are linked into the single program are
# selected as well, and the package that links it comes with them.
ifneq ($(strip $(CONFIG_USERLAND_SINGLEBINARY)),)
override CONFIG_USERLAND_PROGRAMS := $(sort $(CONFIG_USERLAND_PROGRAMS) $(CONFIG_USERLAND_SINGLEBINARY) crunch)
endif

ALL_PROGRAM_MAKEFILES := $(sort \
	$(wildcard userland/firmware/*/Makefile) \
	$(wildcard userland/base/*/Makefile) \
	$(wildcard userland/base/*/*/Makefile) \
	$(wildcard userland/desktop/*/Makefile) \
	$(wildcard userland/desktop/*/*/Makefile) \
	$(wildcard userland/packages/*/*/Makefile) \
	$(wildcard userland/x11/*/Makefile) \
	$(wildcard userland/tests/*/Makefile))
ALL_PROGRAM_MAKEFILES := $(filter-out %/build-%/Makefile,$(ALL_PROGRAM_MAKEFILES))

pkg_aliases = $(strip $(1) \
	$(1)-firmware $(1)-test)

SELECTED_PROGRAM_MAKEFILES := $(foreach mf, $(ALL_PROGRAM_MAKEFILES), \
	$(if $(filter $(call pkg_aliases,$(notdir $(patsubst %/,%,$(dir $(mf))))), $(CONFIG_USERLAND_PROGRAMS)), \
		$(mf)))

PROGRAM_DIRS := $(dir $(SELECTED_PROGRAM_MAKEFILES))
ALL_PROGRAM_DIRS := $(dir $(ALL_PROGRAM_MAKEFILES))

PROGRAM_STAMPS := $(foreach d,$(PROGRAM_DIRS),$(BUILD)/packages/$(notdir $(patsubst %/,%,$(d)))/.stamp)

# What every nested make is told, so that it reads the same configuration
# file and builds for the same platform as this one (a platform entered at
# the prompt or a CONFIGMK= given on the command line would not reach it),
# and puts what it makes in the same build directory.  The directory is
# absolute: a nested make runs in its package's directory, where a BUILD=
# given on the command line as a relative path would name a directory in
# the source tree.
SUBMAKE_CONFIG := PLATFORM="$(PLATFORM)" STANDALONE_CONFIG="$(ZEDBSD_CONFIGMK)" BUILD="$(abspath $(BUILD))" \
	STANDALONE_STATIC="$(CONFIG_USERLAND_STATIC)" STANDALONE_SINGLEBINARY="$(strip $(CONFIG_USERLAND_SINGLEBINARY))"

# What a package built from an upstream release (userland/packages/) is told
# besides: the programs this build selects, the platform's defaults among
# them.  Its lists of what it puts in the root name a file only when its
# package is selected (userland/packages/external.mk).
EXTERNAL_SUBMAKE_CONFIG = USER_PROGRAMS="$(CONFIG_USERLAND_PROGRAMS)"

# "yes | make firmware-licenses" settles the license of every selected
# firmware package before a build (CI and scripts).  Each package asks in its
# own make, one after another, so the answers on the standard input reach it.
FIRMWARE_PROGRAM_DIRS := $(filter userland/firmware/%,$(PROGRAM_DIRS))

.PHONY: firmware-licenses
firmware-licenses:
	@for d in $(FIRMWARE_PROGRAM_DIRS); do \
		t=$$(sed -n 's/^\([a-z0-9-]*-license\):.*/\1/p' "$$d/Makefile" | head -n 1); \
		test -n "$$t" || continue; \
		$(MAKE) -C "$$d" $(SUBMAKE_CONFIG) "$$t" || exit 1; \
	done

# The configuration the packages were made with: the configuration file and
# the variables given on the command line.  A package reads the configuration
# itself (a value such as ZEDBSD_TEST_SCREEN_CAPTURE chooses its sources),
# so a change of it runs every package's make again, which then makes
# again only what the change touches.  The stamp is written every time and
# replaced only when the configuration changes.
PACKAGE_CONFIG_STAMP := $(BUILD)/.package-config

.PHONY: FORCE_PACKAGE_CONFIG
FORCE_PACKAGE_CONFIG:
$(PACKAGE_CONFIG_STAMP): FORCE_PACKAGE_CONFIG
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(subst ','\'',$(MAKEOVERRIDES))' > $@.tmp
	@$(if $(ZEDBSD_CONFIGMK),cat "$(ZEDBSD_CONFIGMK)" >> $@.tmp)
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

define PROGRAM_RULE
_CHILD_DIRS_$(1) := $$(filter $(1)%/,$$(filter-out $(1),$$(ALL_PROGRAM_DIRS)))
_CHILD_SRCS_$(1) := $$(foreach c,$$(_CHILD_DIRS_$(1)),$$(wildcard $$(c)* $$(c)*/* $$(c)*/*/*) $$(patsubst %/,%,$$(c)))
_ALL_SRCS_$(1) := $$(wildcard $(1)* $(1)*/* $(1)*/*/*)
_DIR_NAMES_$(1) := $$(patsubst %/.,%,$$(wildcard $$(addsuffix /.,$$(_ALL_SRCS_$(1)))))
_PKG_SRCS_$(1) := $$(filter-out $$(_CHILD_SRCS_$(1)) $$(_DIR_NAMES_$(1)),$$(_ALL_SRCS_$(1)))

# A package built from an upstream release is made again also when the
# rules all of them share change (userland/packages/external.mk).
$$(BUILD)/packages/$(notdir $(patsubst %/,%,$(1)))/.stamp: $$(_PKG_SRCS_$(1)) userland/base/package.mk $$(PACKAGE_CONFIG_STAMP) \
	$(if $(filter userland/packages/%,$(1)),userland/packages/external.mk)
	@case "$(1)" in \
		userland/packages/*) \
			$$(MAKE) -C $(1) $$(SUBMAKE_CONFIG) $$(EXTERNAL_SUBMAKE_CONFIG) ;; \
		*) \
			$$(MAKE) -C $(1) $$(SUBMAKE_CONFIG) CC="$$(CC)" CPPFLAGS="$$(USER_CPPFLAGS)" CFLAGS="$$(USER_CFLAGS)" AS="$$(AS)" LD="$$(LD)" RANLIB="$$(RANLIB)" NM="$$(NM)" OBJCOPY="$$(OBJCOPY)" OBJDUMP="$$(OBJDUMP)" READELF="$$(READELF)" STRIP="$$(STRIP)" ;; \
	esac
	@mkdir -p $$(dir $$@) && touch $$@

.PHONY: $(1) $(patsubst %/,%,$(1))
$(1) $(patsubst %/,%,$(1)):
	@case "$(1)" in \
		userland/packages/*) \
			$$(MAKE) -C $(1) $$(SUBMAKE_CONFIG) $$(EXTERNAL_SUBMAKE_CONFIG) ;; \
		*) \
			$$(MAKE) -C $(1) $$(SUBMAKE_CONFIG) CC="$$(CC)" CPPFLAGS="$$(USER_CPPFLAGS)" CFLAGS="$$(USER_CFLAGS)" AS="$$(AS)" LD="$$(LD)" RANLIB="$$(RANLIB)" NM="$$(NM)" OBJCOPY="$$(OBJCOPY)" OBJDUMP="$$(OBJDUMP)" READELF="$$(READELF)" STRIP="$$(STRIP)" ;; \
	esac
endef

$(foreach d,$(ALL_PROGRAM_DIRS),$(eval $(call PROGRAM_RULE,$(d))))

.PHONY: packages

programs: $(PROGRAM_STAMPS)


#
# Special Dependency Rules
#

# The single program is linked after every package that puts a program in it.
$(BUILD)/packages/crunch/.stamp: $(filter $(foreach p,$(CONFIG_USERLAND_SINGLEBINARY),$(BUILD)/packages/$(p)/.stamp),$(PROGRAM_STAMPS))

# Base dependencies
$(BUILD)/packages/sh/.stamp $(BUILD)/packages/net/.stamp: $(BUILD)/packages/libedit/.stamp
$(BUILD)/packages/curses/.stamp $(BUILD)/packages/vi/.stamp $(BUILD)/packages/terminfo-extra/.stamp: $(BUILD)/packages/terminfo/.stamp
$(BUILD)/packages/fidoctl/.stamp $(BUILD)/packages/ssh/.stamp $(BUILD)/packages/libssl-compat/.stamp: $(BUILD)/packages/libcrypto-compat/.stamp
$(BUILD)/packages/passkey-fido2/.stamp: $(BUILD)/packages/passkey/.stamp $(BUILD)/packages/libcrypto-compat/.stamp
$(BUILD)/packages/fetch/.stamp: $(BUILD)/packages/libssl-compat/.stamp $(BUILD)/packages/libcrypto-compat/.stamp
$(BUILD)/packages/noct/.stamp: $(BUILD)/packages/libegl/.stamp $(BUILD)/packages/libglesv2/.stamp

# Library dependencies
$(BUILD)/packages/libvulkan/.stamp: $(BUILD)/packages/libwayland/.stamp
$(BUILD)/packages/libpng-compat/.stamp: $(BUILD)/packages/libz-compat/.stamp
$(BUILD)/packages/libpdf/.stamp: $(BUILD)/packages/libz-compat/.stamp $(BUILD)/packages/libjpeg-compat/.stamp $(BUILD)/packages/libtruetype/.stamp
$(BUILD)/packages/libkeiland/.stamp: $(BUILD)/packages/libwayland/.stamp $(BUILD)/packages/libtruetype/.stamp $(BUILD)/packages/libvulkan/.stamp $(BUILD)/packages/libpng-compat/.stamp $(BUILD)/packages/libz-compat/.stamp
$(BUILD)/packages/libegl/.stamp: $(BUILD)/packages/libvulkan/.stamp $(BUILD)/packages/libwayland/.stamp $(BUILD)/packages/libwayland-egl/.stamp
$(BUILD)/packages/libglesv2/.stamp: $(BUILD)/packages/libegl/.stamp
$(BUILD)/packages/libGL/.stamp: $(BUILD)/packages/libegl/.stamp $(BUILD)/packages/libglesv2/.stamp
$(BUILD)/packages/libbrowser/.stamp: $(BUILD)/packages/libtruetype/.stamp $(BUILD)/packages/libvulkan/.stamp $(BUILD)/packages/libmedia/.stamp $(BUILD)/packages/libjpeg-compat/.stamp $(BUILD)/packages/libpng-compat/.stamp $(BUILD)/packages/libz-compat/.stamp $(BUILD)/packages/libgif-compat/.stamp $(BUILD)/packages/libwebp-compat/.stamp $(BUILD)/packages/libssl-compat/.stamp $(BUILD)/packages/libcrypto-compat/.stamp
$(BUILD)/packages/libGLX/.stamp: $(BUILD)/packages/libGL/.stamp $(BUILD)/packages/libegl/.stamp

# Desktop app dependencies
KEILAND_COMMON_DEPS := $(BUILD)/packages/libvulkan/.stamp $(BUILD)/packages/libwayland/.stamp $(BUILD)/packages/libkeiland/.stamp $(BUILD)/packages/libtruetype/.stamp
$(BUILD)/packages/settings/.stamp:    $(KEILAND_COMMON_DEPS) $(BUILD)/packages/preview/.stamp
$(BUILD)/packages/terminal/.stamp:    $(KEILAND_COMMON_DEPS)
$(BUILD)/packages/displayctl/.stamp:  $(BUILD)/packages/libkeiland/.stamp
$(BUILD)/packages/files/.stamp:       $(KEILAND_COMMON_DEPS) $(BUILD)/packages/preview/.stamp
$(BUILD)/packages/browser/.stamp:     $(KEILAND_COMMON_DEPS) $(BUILD)/packages/libbrowser/.stamp $(BUILD)/packages/libz-compat/.stamp
$(BUILD)/packages/imageview/.stamp:   $(KEILAND_COMMON_DEPS) $(BUILD)/packages/libz-compat/.stamp $(BUILD)/packages/libpng-compat/.stamp $(BUILD)/packages/libjpeg-compat/.stamp $(BUILD)/packages/libgif-compat/.stamp
$(BUILD)/packages/textedit/.stamp:    $(KEILAND_COMMON_DEPS) $(BUILD)/packages/libpng-compat/.stamp $(BUILD)/packages/libz-compat/.stamp
$(BUILD)/packages/pdfviewer/.stamp:   $(KEILAND_COMMON_DEPS) $(BUILD)/packages/libpdf/.stamp $(BUILD)/packages/libz-compat/.stamp
$(BUILD)/packages/notes/.stamp:       $(KEILAND_COMMON_DEPS) $(BUILD)/packages/libpdf/.stamp $(BUILD)/packages/libz-compat/.stamp $(BUILD)/packages/libpng-compat/.stamp $(BUILD)/packages/libjpeg-compat/.stamp $(BUILD)/packages/libgif-compat/.stamp
$(BUILD)/packages/photos/.stamp:      $(KEILAND_COMMON_DEPS) $(BUILD)/packages/mediastorage/.stamp $(BUILD)/packages/preview/.stamp $(BUILD)/packages/libz-compat/.stamp
$(BUILD)/packages/music/.stamp:       $(KEILAND_COMMON_DEPS) $(BUILD)/packages/libmedia/.stamp $(BUILD)/packages/libz-compat/.stamp $(BUILD)/packages/libpng-compat/.stamp $(BUILD)/packages/libjpeg-compat/.stamp $(BUILD)/packages/libgif-compat/.stamp
$(BUILD)/packages/videoplayer/.stamp: $(KEILAND_COMMON_DEPS) $(BUILD)/packages/libmedia/.stamp
$(BUILD)/packages/dispsink/.stamp:    $(KEILAND_COMMON_DEPS) $(BUILD)/packages/libmedia/.stamp $(BUILD)/packages/libssl-compat/.stamp $(BUILD)/packages/libcrypto-compat/.stamp
$(BUILD)/packages/calendar/.stamp:    $(KEILAND_COMMON_DEPS)
$(BUILD)/packages/mailer/.stamp:      $(KEILAND_COMMON_DEPS) $(BUILD)/packages/libssl-compat/.stamp $(BUILD)/packages/libcrypto-compat/.stamp
$(BUILD)/packages/monitor/.stamp:     $(KEILAND_COMMON_DEPS)
$(BUILD)/packages/phone/.stamp:       $(KEILAND_COMMON_DEPS) $(BUILD)/packages/preview/.stamp $(BUILD)/packages/libz-compat/.stamp
$(BUILD)/packages/mediastorage/.stamp: $(BUILD)/packages/libkeiland/.stamp $(BUILD)/packages/libwayland/.stamp $(BUILD)/packages/preview/.stamp
$(BUILD)/packages/ime/.stamp:         $(BUILD)/packages/libwayland/.stamp $(BUILD)/packages/libkeiland/.stamp
$(BUILD)/packages/sessiond/.stamp:    $(BUILD)/packages/passkey/.stamp
$(BUILD)/packages/xserver/.stamp:     $(KEILAND_COMMON_DEPS) $(BUILD)/packages/libGL/.stamp
$(BUILD)/packages/zgears/.stamp $(BUILD)/packages/glxtest/.stamp: $(BUILD)/packages/libGL/.stamp $(BUILD)/packages/libGLX/.stamp
$(BUILD)/packages/wayland/.stamp:     $(BUILD)/packages/libvulkan/.stamp $(BUILD)/packages/libtruetype/.stamp $(BUILD)/packages/libkeiland/.stamp $(BUILD)/packages/sessiond/.stamp $(BUILD)/packages/mediastorage/.stamp $(BUILD)/packages/libmedia/.stamp $(BUILD)/packages/libpng-compat/.stamp $(BUILD)/packages/libz-compat/.stamp $(BUILD)/packages/libjpeg-compat/.stamp
$(BUILD)/packages/rdpd/.stamp:        $(BUILD)/packages/libssl-compat/.stamp $(BUILD)/packages/libcrypto-compat/.stamp $(BUILD)/packages/passkey/.stamp $(BUILD)/packages/wayland/.stamp

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

#
# External Packages
#

# The packages built from upstream release tarballs (userland/packages/)
# share one set of cross entry points per build directory
# (userland/packages/external.mk).  They are made once, before any of those
# packages' own makes, so that two packages made at the same time do not
# write them at the same time.
EXTERNAL_PROGRAM_DIRS := $(filter userland/packages/%,$(ALL_PROGRAM_DIRS))
EXTERNAL_PROGRAM_STAMPS := $(foreach d,$(EXTERNAL_PROGRAM_DIRS),$(BUILD)/packages/$(notdir $(patsubst %/,%,$(d)))/.stamp)

.PHONY: packages-cross-toolchain
packages-cross-toolchain:
	@$(MAKE) -C userland/packages/libs/zlib $(SUBMAKE_CONFIG) packages-cross-toolchain

$(EXTERNAL_PROGRAM_STAMPS): | packages-cross-toolchain

# A package's own make builds an external package it builds against when
# that package's stage is missing (external.mk), so the other package's own
# make has to be finished by then; otherwise the two makes fetch, extract
# and build the same tree at the same time.  These follow the packages'
# EXT_<name>_DEPENDS and the stages their Makefiles name.
$(BUILD)/packages/openssh/.stamp:       $(BUILD)/packages/openssl/.stamp
$(BUILD)/packages/curl/.stamp:          $(BUILD)/packages/openssl/.stamp $(BUILD)/packages/zlib/.stamp
$(BUILD)/packages/python3/.stamp:       $(BUILD)/packages/openssl/.stamp $(BUILD)/packages/zlib/.stamp
$(BUILD)/packages/clang/.stamp:         $(BUILD)/packages/libcxx/.stamp
$(BUILD)/packages/libpng/.stamp:        $(BUILD)/packages/zlib/.stamp
$(BUILD)/packages/libtiff/.stamp:       $(BUILD)/packages/zlib/.stamp $(BUILD)/packages/libjpeg-turbo/.stamp
$(BUILD)/packages/freetype/.stamp:      $(BUILD)/packages/zlib/.stamp $(BUILD)/packages/libpng/.stamp
$(BUILD)/packages/fontconfig/.stamp:    $(BUILD)/packages/freetype/.stamp $(BUILD)/packages/expat/.stamp
$(BUILD)/packages/glib/.stamp:          $(BUILD)/packages/libffi/.stamp $(BUILD)/packages/pcre2/.stamp $(BUILD)/packages/zlib/.stamp
$(BUILD)/packages/graphene/.stamp:      $(BUILD)/packages/glib/.stamp
$(BUILD)/packages/gdk-pixbuf/.stamp:    $(BUILD)/packages/glib/.stamp $(BUILD)/packages/libpng/.stamp $(BUILD)/packages/libjpeg-turbo/.stamp $(BUILD)/packages/libtiff/.stamp
$(BUILD)/packages/harfbuzz/.stamp:      $(BUILD)/packages/glib/.stamp $(BUILD)/packages/freetype/.stamp $(BUILD)/packages/libcxx/.stamp
$(BUILD)/packages/cairo/.stamp:         $(BUILD)/packages/pixman/.stamp $(BUILD)/packages/freetype/.stamp $(BUILD)/packages/fontconfig/.stamp $(BUILD)/packages/libpng/.stamp $(BUILD)/packages/zlib/.stamp $(BUILD)/packages/glib/.stamp
$(BUILD)/packages/pango/.stamp:         $(BUILD)/packages/glib/.stamp $(BUILD)/packages/fribidi/.stamp $(BUILD)/packages/harfbuzz/.stamp $(BUILD)/packages/fontconfig/.stamp $(BUILD)/packages/freetype/.stamp $(BUILD)/packages/cairo/.stamp
$(BUILD)/packages/libxkbcommon/.stamp:  $(BUILD)/packages/xkeyboard-config/.stamp
$(BUILD)/packages/gtk4/.stamp:          $(BUILD)/packages/glib/.stamp $(BUILD)/packages/cairo/.stamp $(BUILD)/packages/pango/.stamp $(BUILD)/packages/harfbuzz/.stamp $(BUILD)/packages/fribidi/.stamp $(BUILD)/packages/fontconfig/.stamp $(BUILD)/packages/freetype/.stamp $(BUILD)/packages/gdk-pixbuf/.stamp $(BUILD)/packages/libpng/.stamp $(BUILD)/packages/libtiff/.stamp $(BUILD)/packages/libjpeg-turbo/.stamp $(BUILD)/packages/libepoxy/.stamp $(BUILD)/packages/libxkbcommon/.stamp $(BUILD)/packages/graphene/.stamp $(BUILD)/packages/libwayland/.stamp $(BUILD)/packages/wayland-protocols/.stamp
