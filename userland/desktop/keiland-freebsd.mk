# Native FreeBSD Keiland build, independent of the Linux and zedBSD rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Builds real native libraries and the compositor with FreeBSD seat authority.
.DEFAULT_GOAL := all

KEILAND_FREEBSD_BUILD ?= build/keiland-freebsd
KEILAND_PREFIX ?= /opt/keiland
DESTDIR ?=
KEILAND_FREEBSD_OPT ?= -O2 -g
KEILAND_FREEBSD_LOCALBASE ?= /usr/local
KEILAND_FREEBSD_PYTHON ?= python3
KEILAND_FREEBSD_EXTRA_CPPFLAGS ?=
KEILAND_FREEBSD_CPPFLAGS := \
	-DKEILAND_BINDIR='"$(KEILAND_PREFIX)/bin"' -DKEILAND_LIBEXECDIR='"$(KEILAND_PREFIX)/libexec"' \
	-DKEILAND_DATADIR='"$(KEILAND_PREFIX)/share"' -DKEILAND_SYSCONFDIR='"$(KEILAND_PREFIX)/etc"' \
	$(KEILAND_FREEBSD_EXTRA_CPPFLAGS) -I. -Iuserland/desktop/include \
	-I$(KEILAND_FREEBSD_BUILD)/include -I$(KEILAND_FREEBSD_LOCALBASE)/include
KEILAND_FREEBSD_CFLAGS := $(KEILAND_FREEBSD_OPT) -std=gnu17 -Wall -Wextra -Werror -fPIC
KEILAND_FREEBSD_LDFLAGS := -Wl,-rpath,$(KEILAND_PREFIX)/lib -Wl,--enable-new-dtags \
	-Wl,-rpath-link,$(KEILAND_FREEBSD_BUILD)/lib -L$(KEILAND_FREEBSD_BUILD)/lib

# Copy only the selected compatibility headers; native libc headers keep their standard names.
KEILAND_FREEBSD_HEADERS := $(shell find include/libc/compat -type f -name '*.h') \
	include/libc/pdf.h include/libc/sha2.h include/libc/md5.h include/libc/sha1.h
KEILAND_FREEBSD_HEADER_COPIES := $(patsubst include/libc/%,$(KEILAND_FREEBSD_BUILD)/include/%,$(KEILAND_FREEBSD_HEADERS))
KEILAND_FREEBSD_NATIVE_HEADERS := $(KEILAND_FREEBSD_BUILD)/include/pty.h $(KEILAND_FREEBSD_BUILD)/include/sys/xattr.h
$(KEILAND_FREEBSD_BUILD)/include/%: include/libc/%
	@mkdir -p $(dir $@)
	cp $< $@

# Use the installed native DRM UAPI under the common source's include spelling.
$(KEILAND_FREEBSD_BUILD)/include/drm:
	@mkdir -p $(dir $@)
	test -f $(KEILAND_FREEBSD_LOCALBASE)/include/libdrm/drm.h
	ln -s $(KEILAND_FREEBSD_LOCALBASE)/include/libdrm $@

$(KEILAND_FREEBSD_BUILD)/obj/%.o: %.c | $(KEILAND_FREEBSD_HEADER_COPIES) $(KEILAND_FREEBSD_NATIVE_HEADERS) $(KEILAND_FREEBSD_BUILD)/include/drm
	@mkdir -p $(dir $@)
	$(CC) $(KEILAND_FREEBSD_CFLAGS) $(KEILAND_FREEBSD_CPPFLAGS) $(KEILAND_FREEBSD_CPPFLAGS_$(subst /,_,$(dir $<))) -MMD -MP -c $< -o $@

KEILAND_FREEBSD_SOURCES :=
KEILAND_FREEBSD_ALL :=
KEILAND_FREEBSD_INSTALL :=

# $(1) name, $(2) SONAME, $(3) sources, $(4) our libraries it links (SONAMEs or a .a), $(5) system libraries,
# $(6) the version script or empty, $(7) more link flags
define KEILAND_FREEBSD_LIBRARY
KEILAND_FREEBSD_SOURCES += $(3)
KEILAND_FREEBSD_LIBRARY_OBJS_$(1) := $$(patsubst %.c,$(KEILAND_FREEBSD_BUILD)/obj/%.o,$(3))
-include $$(KEILAND_FREEBSD_LIBRARY_OBJS_$(1):.o=.d)
$(KEILAND_FREEBSD_BUILD)/lib/$(2): $$(KEILAND_FREEBSD_LIBRARY_OBJS_$(1)) $$(addprefix $(KEILAND_FREEBSD_BUILD)/lib/,$(4)) $(6)
	@mkdir -p $$(dir $$@)
	$$(CC) -shared -Wl,-soname,$(2) -Wl,-z,defs $$(if $(6),-Wl$$(comma)--version-script=$(6)) $(7) $$(KEILAND_FREEBSD_LDFLAGS) \
		$$(KEILAND_FREEBSD_LIBRARY_OBJS_$(1)) $$(addprefix -l:,$(4)) $(5) -o $$@
KEILAND_FREEBSD_ALL += $(KEILAND_FREEBSD_BUILD)/lib/$(2)
KEILAND_FREEBSD_INSTALL += lib/$(2)
endef

# $(1) name, $(2) bin or libexec, $(3) sources, $(4) own libraries, $(5) system libraries
define KEILAND_FREEBSD_PROGRAM
KEILAND_FREEBSD_SOURCES += $(3)
KEILAND_FREEBSD_PROGRAM_OBJS_$(1) := $$(patsubst %.c,$(KEILAND_FREEBSD_BUILD)/obj/%.o,$(3))
-include $$(KEILAND_FREEBSD_PROGRAM_OBJS_$(1):.o=.d)
$(KEILAND_FREEBSD_BUILD)/$(2)/$(1): $$(KEILAND_FREEBSD_PROGRAM_OBJS_$(1)) $$(addprefix $(KEILAND_FREEBSD_BUILD)/lib/,$(4))
	@mkdir -p $$(dir $$@)
	$$(CC) -pie $$(KEILAND_FREEBSD_LDFLAGS) $$(KEILAND_FREEBSD_PROGRAM_OBJS_$(1)) $$(addprefix -l:,$(4)) $(5) -o $$@
KEILAND_FREEBSD_ALL += $(KEILAND_FREEBSD_BUILD)/$(2)/$(1)
KEILAND_FREEBSD_INSTALL += $(2)/$(1)
endef

# $(1) path under the prefix, $(2) source file
define KEILAND_FREEBSD_DATA
$(KEILAND_FREEBSD_BUILD)/$(1): $(2)
	@mkdir -p $$(dir $$@)
	cp $$< $$@
KEILAND_FREEBSD_ALL += $(KEILAND_FREEBSD_BUILD)/$(1)
KEILAND_FREEBSD_INSTALL += $(1)
endef

# $(1) name, $(2) the archive's file name, $(3) sources
define KEILAND_FREEBSD_STATIC
KEILAND_FREEBSD_SOURCES += $(3)
KEILAND_FREEBSD_STATIC_OBJS_$(1) := $$(patsubst %.c,$(KEILAND_FREEBSD_BUILD)/obj/%.o,$(3))
-include $$(KEILAND_FREEBSD_STATIC_OBJS_$(1):.o=.d)
$(KEILAND_FREEBSD_BUILD)/lib/$(2): $$(KEILAND_FREEBSD_STATIC_OBJS_$(1))
	@mkdir -p $$(dir $$@)
	rm -f $$@
	ar rcs $$@ $$(KEILAND_FREEBSD_STATIC_OBJS_$(1))
KEILAND_FREEBSD_ALL += $(KEILAND_FREEBSD_BUILD)/lib/$(2)
endef

comma := ,

KEILAND_FREEBSD_PACKAGES ?= userland/base/libz-compat/Makefile.freebsd \
	userland/base/libpng-compat/Makefile.freebsd \
	userland/base/libjpeg-compat/Makefile.freebsd \
	userland/base/libgif-compat/Makefile.freebsd \
	userland/desktop/freebsd-compat/Makefile.freebsd \
	userland/desktop/libwayland/Makefile.freebsd \
	userland/desktop/libtruetype/Makefile.freebsd \
	userland/desktop/libvulkan-compat/Makefile.freebsd \
	userland/desktop/libkeiland-backend-freebsd/Makefile.freebsd \
	userland/desktop/libkeiland/Makefile.freebsd \
	userland/base/libpdf/Makefile.freebsd \
	userland/packages/libseat/Makefile.freebsd \
	userland/desktop/wayland/Makefile.freebsd \
	userland/desktop/terminal/Makefile.freebsd \
	userland/desktop/files/Makefile.freebsd \
	userland/desktop/settings/Makefile.freebsd \
	userland/desktop/notes/Makefile.freebsd \
	userland/desktop/monitor/Makefile.freebsd \
	userland/desktop/textedit/Makefile.freebsd \
	userland/desktop/imageview/Makefile.freebsd \
	userland/desktop/pdfviewer/Makefile.freebsd \
	userland/desktop/ime/Makefile.freebsd \
	userland/tests/wlshm/Makefile.freebsd \
	userland/tests/wltest/Makefile.freebsd \
	userland/tests/vkdemo/Makefile.freebsd \
	userland/tests/mview/Makefile.freebsd \
	userland/tests/kuidemo/Makefile.freebsd
include $(KEILAND_FREEBSD_PACKAGES)

# App Home uses the compositor's existing config parser; no common built-in list changes.
$(KEILAND_FREEBSD_BUILD)/etc/keiland/apps.conf: userland/desktop/wayland/data/apps-freebsd.conf.in
	@mkdir -p $(dir $@)
	sed 's|@PREFIX@|$(KEILAND_PREFIX)|g' $< > $@.tmp
	mv $@.tmp $@
KEILAND_FREEBSD_ALL += $(KEILAND_FREEBSD_BUILD)/etc/keiland/apps.conf
KEILAND_FREEBSD_INSTALL += etc/keiland/apps.conf

# The console launcher shares its shell implementation with the Linux build.
$(KEILAND_FREEBSD_BUILD)/bin/keiland-desktop: userland/desktop/wayland/keiland-desktop.in
	@mkdir -p $(dir $@)
	sed 's|@PREFIX@|$(KEILAND_PREFIX)|g' $< > $@.tmp
	chmod 0755 $@.tmp
	mv $@.tmp $@
KEILAND_FREEBSD_ALL += $(KEILAND_FREEBSD_BUILD)/bin/keiland-desktop
KEILAND_FREEBSD_INSTALL += bin/keiland-desktop

# Bundled gradients are generated outside git (PNG, ws138-p002); the default picture is the generated Dawn (2026-10-08 user), the tree's Birch-Lake.png (U3's default) is in the catalogue.
KEILAND_FREEBSD_WALLPAPER_NAMES := Aurora Dawn Lagoon Meadow Twilight
KEILAND_FREEBSD_WALLPAPERS := $(addprefix $(KEILAND_FREEBSD_BUILD)/share/keiland/wallpapers/,$(addsuffix .png,$(KEILAND_FREEBSD_WALLPAPER_NAMES)))
$(KEILAND_FREEBSD_WALLPAPERS) &: userland/desktop/wallpapers/generate.py
	$(KEILAND_FREEBSD_PYTHON) $< $(KEILAND_FREEBSD_BUILD)/share/keiland/wallpapers
KEILAND_FREEBSD_WALLPAPER ?= $(KEILAND_FREEBSD_BUILD)/share/keiland/wallpapers/Dawn.png
$(eval $(call KEILAND_FREEBSD_DATA,share/keiland/wallpaper.png,$(KEILAND_FREEBSD_WALLPAPER)))
$(eval $(call KEILAND_FREEBSD_DATA,share/keiland/wallpapers/Birch-Lake.png,userland/desktop/wallpapers/Birch-Lake.png))
# The compositor's own landscape is offered in the catalogue too (ws099-p019, 2026-10-05 user).
$(eval $(call KEILAND_FREEBSD_DATA,share/keiland/wallpapers/Lakeside.png,userland/desktop/wallpapers/Lakeside.png))
KEILAND_FREEBSD_ALL += $(KEILAND_FREEBSD_WALLPAPERS)
KEILAND_FREEBSD_INSTALL += $(addprefix share/keiland/wallpapers/,$(addsuffix .png,$(filter-out Dawn,$(KEILAND_FREEBSD_WALLPAPER_NAMES))))

# The Japanese dictionary is in the tree (userland/desktop/ime/dict/SKK-JISYO.ja, one file of the supplement and
# REmacs's dictionary since ws095-p017); the copyright holder's WS095 D1 relicensing places it under the project license.
$(eval $(call KEILAND_FREEBSD_DATA,share/keiland/ime/ja/SKK-JISYO.ja,userland/desktop/ime/dict/SKK-JISYO.ja))
# The catalogs of the desktop's translations (WS158): share/keiland/locale/LANGUAGE/DOMAIN.tr.
$(foreach catalog,$(sort $(wildcard userland/desktop/locale/*/*.tr)),$(eval $(call KEILAND_FREEBSD_DATA,share/keiland/locale/$(patsubst userland/desktop/locale/%,%,$(catalog)),$(catalog))))
# The SKK engine's dictionaries (WS154): REmacs's two, copied apart into userland/desktop/ime/skk-dict, under zlib.
$(eval $(call KEILAND_FREEBSD_DATA,share/keiland/ime/skk/SKK-JISYO.X,userland/desktop/ime/skk-dict/SKK-JISYO.X))
$(eval $(call KEILAND_FREEBSD_DATA,share/keiland/ime/skk/SKK-JISYO.remacs,userland/desktop/ime/skk-dict/SKK-JISYO.remacs))

.PHONY: all libraries install install-headers print-sources header-dependencies
all: $(KEILAND_FREEBSD_ALL)
libraries: $(filter %.a %.so %.so.1,$(KEILAND_FREEBSD_ALL))

# FreeBSD install has no GNU -D; create each destination directory explicitly.
# Libraries and headers an earlier Keiland installed that are part of another now: the widgets joined libkeiland
# (WS131 p012) and their headers <keiland.h> (WS131 p023), and the headers that moved to their own directories,
# <keiland/keiland.h> and <truetype/truetype.h> (WS131 p026).  An install over an earlier one removes them, so that
# nothing loads or includes a stale copy.
KEILAND_FREEBSD_RETIRED := lib/libkeiui.so include/keiland-ui.h include/keiui.h include/keiland.h include/truetype.h

install: all install-headers
	@set -e; for f in $(KEILAND_FREEBSD_INSTALL); do \
		mode=0644; case $$f in lib/*|bin/*|libexec/*) mode=0755 ;; esac; \
		mkdir -p "$(DESTDIR)$(KEILAND_PREFIX)/$$(dirname "$$f")"; \
		install -m $$mode "$(KEILAND_FREEBSD_BUILD)/$$f" "$(DESTDIR)$(KEILAND_PREFIX)/$$f"; \
	done
	@set -e; for f in $(KEILAND_FREEBSD_RETIRED); do rm -f "$(DESTDIR)$(KEILAND_PREFIX)/$$f"; done

KEILAND_FREEBSD_PUBLIC_HEADERS := $(shell find userland/desktop/include/wayland -type f -name '*.h') \
	$(addprefix userland/desktop/include/,wayland-client.h wayland-client-core.h wayland-client-protocol.h \
	wayland-util.h xdg-shell-client-protocol.h primary-selection-unstable-v1-client-protocol.h \
	tablet-unstable-v2-client-protocol.h truetype/truetype.h keiland/keiland.h)
install-headers:
	@mkdir -p "$(DESTDIR)$(KEILAND_PREFIX)/include"
	install -m 0644 include/libc/pdf.h "$(DESTDIR)$(KEILAND_PREFIX)/include/pdf.h"
	@set -e; for f in $(KEILAND_FREEBSD_PUBLIC_HEADERS); do \
		rel=$${f#userland/desktop/include/}; \
		mkdir -p "$(DESTDIR)$(KEILAND_PREFIX)/include/$$(dirname "$$rel")"; \
		install -m 0644 "$$f" "$(DESTDIR)$(KEILAND_PREFIX)/include/$$rel"; \
	done

print-sources:
	@printf '%s\n' $(KEILAND_FREEBSD_SOURCES)

# Include native system headers in audit output, unlike ordinary -MMD dependency files.
KEILAND_FREEBSD_HEADER_DEPS := $(patsubst %.c,$(KEILAND_FREEBSD_BUILD)/header-check/%.d,$(KEILAND_FREEBSD_SOURCES))
header-dependencies: $(KEILAND_FREEBSD_HEADER_DEPS)
$(KEILAND_FREEBSD_BUILD)/header-check/%.d: %.c | $(KEILAND_FREEBSD_HEADER_COPIES) $(KEILAND_FREEBSD_NATIVE_HEADERS) $(KEILAND_FREEBSD_BUILD)/include/drm
	@mkdir -p $(dir $@)
	$(CC) $(KEILAND_FREEBSD_CFLAGS) $(KEILAND_FREEBSD_CPPFLAGS) $(KEILAND_FREEBSD_CPPFLAGS_$(subst /,_,$(dir $<))) -M $< -o $@
