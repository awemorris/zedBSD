# The Linux build of the Keiland desktop (WS105, plan/ws105/design.md §3).  It shares no rule with the zedBSD build:
# the host's compiler, headers and C library build every package listed in KEILAND_LINUX_PACKAGES, each with its
# own Makefile.linux, into $(KEILAND_LINUX_BUILD), and install copies the result under $(DESTDIR)$(KEILAND_PREFIX).
#
#   make keiland-linux                       (from the top-level Makefile: make -f userland/desktop/keiland-linux.mk all)
#   make keiland-linux-install DESTDIR=...   (install)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

KEILAND_LINUX_BUILD ?= build/keiland-linux
KEILAND_PREFIX ?= /opt/keiland
DESTDIR ?=
KEILAND_LINUX_OPT ?= -O2 -g
# The Debian multiarch name of the host (clang has no -print-multiarch on Debian 19).
KEILAND_LINUX_MULTIARCH ?= $(shell gcc -print-multiarch 2>/dev/null || dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null)
KEILAND_LINUX_EXTRA_CPPFLAGS ?=

KEILAND_LINUX_CPPFLAGS := -D_GNU_SOURCE \
	-DKEILAND_BINDIR='"$(KEILAND_PREFIX)/bin"' -DKEILAND_LIBEXECDIR='"$(KEILAND_PREFIX)/libexec"' \
	-DKEILAND_DATADIR='"$(KEILAND_PREFIX)/share"' -DKEILAND_SYSCONFDIR='"$(KEILAND_PREFIX)/etc"' \
	$(KEILAND_LINUX_EXTRA_CPPFLAGS) -I. -Iuserland/desktop/include -I$(KEILAND_LINUX_BUILD)/include
# -Wno-format-truncation: gcc's guess that a display string may be cut short (the strings are cut on purpose; D24).
KEILAND_LINUX_CFLAGS := $(KEILAND_LINUX_OPT) -std=gnu17 -Wall -Wextra -Werror -Wno-format-truncation -fPIC
KEILAND_LINUX_LDFLAGS := -Wl,-rpath,$(KEILAND_PREFIX)/lib -Wl,--enable-new-dtags \
	-Wl,-rpath-link,$(KEILAND_LINUX_BUILD)/lib -L$(KEILAND_LINUX_BUILD)/lib

# The headers the Linux build takes from zedBSD's C library side: copies, never -Iinclude/libc (it would hide glibc's).
KEILAND_LINUX_HEADERS := $(shell find include/libc/compat -type f -name '*.h') \
	include/libc/pdf.h include/libc/sha2.h include/libc/md5.h include/libc/sha1.h
KEILAND_LINUX_HEADER_COPIES := $(patsubst include/libc/%,$(KEILAND_LINUX_BUILD)/include/%,$(KEILAND_LINUX_HEADERS))

$(KEILAND_LINUX_BUILD)/include/%: include/libc/%
	@mkdir -p $(dir $@)
	cp $< $@

$(KEILAND_LINUX_BUILD)/obj/%.o: %.c | $(KEILAND_LINUX_HEADER_COPIES)
	@mkdir -p $(dir $@)
	$(CC) $(KEILAND_LINUX_CFLAGS) $(KEILAND_LINUX_CPPFLAGS) $(KEILAND_LINUX_CPPFLAGS_$(subst /,_,$(dir $<))) -MMD -MP -c $< -o $@

KEILAND_LINUX_SOURCES :=
KEILAND_LINUX_ALL :=
KEILAND_LINUX_INSTALL :=

# $(1) name, $(2) SONAME, $(3) sources, $(4) our libraries it links (SONAMEs or a .a), $(5) system libraries,
# $(6) the version script or empty, $(7) more link flags
define KEILAND_LINUX_LIBRARY
KEILAND_LINUX_SOURCES += $(3)
KEILAND_LINUX_LIBRARY_OBJS_$(1) := $$(patsubst %.c,$(KEILAND_LINUX_BUILD)/obj/%.o,$(3))
-include $$(KEILAND_LINUX_LIBRARY_OBJS_$(1):.o=.d)
$(KEILAND_LINUX_BUILD)/lib/$(2): $$(KEILAND_LINUX_LIBRARY_OBJS_$(1)) $$(addprefix $(KEILAND_LINUX_BUILD)/lib/,$(4)) $(6)
	@mkdir -p $$(dir $$@)
	$$(CC) -shared -Wl,-soname,$(2) -Wl,-z,defs $$(if $(6),-Wl$$(comma)--version-script=$(6)) $(7) $$(KEILAND_LINUX_LDFLAGS) \
		$$(KEILAND_LINUX_LIBRARY_OBJS_$(1)) $$(addprefix -l:,$(4)) $(5) -o $$@
KEILAND_LINUX_ALL += $(KEILAND_LINUX_BUILD)/lib/$(2)
KEILAND_LINUX_INSTALL += lib/$(2)
endef

# $(1) name, $(2) bin or libexec, $(3) sources, $(4) our libraries it links, $(5) system libraries
define KEILAND_LINUX_PROGRAM
KEILAND_LINUX_SOURCES += $(3)
KEILAND_LINUX_PROGRAM_OBJS_$(1) := $$(patsubst %.c,$(KEILAND_LINUX_BUILD)/obj/%.o,$(3))
-include $$(KEILAND_LINUX_PROGRAM_OBJS_$(1):.o=.d)
$(KEILAND_LINUX_BUILD)/$(2)/$(1): $$(KEILAND_LINUX_PROGRAM_OBJS_$(1)) $$(addprefix $(KEILAND_LINUX_BUILD)/lib/,$(4))
	@mkdir -p $$(dir $$@)
	$$(CC) -pie $$(KEILAND_LINUX_LDFLAGS) $$(KEILAND_LINUX_PROGRAM_OBJS_$(1)) $$(addprefix -l:,$(4)) $(5) -o $$@
KEILAND_LINUX_ALL += $(KEILAND_LINUX_BUILD)/$(2)/$(1)
KEILAND_LINUX_INSTALL += $(2)/$(1)
endef

# $(1) the path under the prefix, $(2) the source file
define KEILAND_LINUX_DATA
$(KEILAND_LINUX_BUILD)/$(1): $(2)
	@mkdir -p $$(dir $$@)
	cp $$< $$@
KEILAND_LINUX_ALL += $(KEILAND_LINUX_BUILD)/$(1)
KEILAND_LINUX_INSTALL += $(1)
endef

# $(1) name, $(2) the archive's file name, $(3) sources
define KEILAND_LINUX_STATIC
KEILAND_LINUX_SOURCES += $(3)
KEILAND_LINUX_STATIC_OBJS_$(1) := $$(patsubst %.c,$(KEILAND_LINUX_BUILD)/obj/%.o,$(3))
-include $$(KEILAND_LINUX_STATIC_OBJS_$(1):.o=.d)
$(KEILAND_LINUX_BUILD)/lib/$(2): $$(KEILAND_LINUX_STATIC_OBJS_$(1))
	@mkdir -p $$(dir $$@)
	rm -f $$@
	$$(AR) rcs $$@ $$(KEILAND_LINUX_STATIC_OBJS_$(1))
KEILAND_LINUX_ALL += $(KEILAND_LINUX_BUILD)/lib/$(2)
endef

comma := ,

KEILAND_LINUX_PACKAGES ?= userland/base/libz-compat/Makefile.linux \
	userland/base/libpng-compat/Makefile.linux \
	userland/base/libjpeg-compat/Makefile.linux \
	userland/base/libgif-compat/Makefile.linux \
	userland/desktop/linux-compat/Makefile.linux \
	userland/desktop/libwayland/Makefile.linux \
	userland/desktop/libtruetype/Makefile.linux \
	userland/desktop/libvulkan-compat/Makefile.linux \
	userland/desktop/libkeiland-backend-linux/Makefile.linux \
	userland/desktop/libkeiland/Makefile.linux \
	userland/desktop/wayland/Makefile.linux \
	userland/tests/wlshm/Makefile.linux
include $(KEILAND_LINUX_PACKAGES)

include userland/tests/vkdemo/Makefile.linux
include userland/desktop/mediastorage/Makefile.linux
include userland/tests/wltest/Makefile.linux
include userland/tests/mview/Makefile.linux

include userland/base/libpdf/Makefile.linux
include userland/desktop/preview/Makefile.linux
include userland/desktop/terminal/Makefile.linux
include userland/desktop/files/Makefile.linux
include userland/desktop/settings/Makefile.linux
include userland/desktop/notes/Makefile.linux
include userland/desktop/monitor/Makefile.linux
include userland/desktop/textedit/Makefile.linux
include userland/desktop/imageview/Makefile.linux
include userland/desktop/pdfviewer/Makefile.linux
include userland/tests/kuidemo/Makefile.linux
include userland/desktop/ime/Makefile.linux

# App Home uses the compositor's existing config parser; no common built-in list changes.
$(KEILAND_LINUX_BUILD)/etc/keiland/apps.conf: userland/desktop/wayland/data/apps-linux.conf.in
	@mkdir -p $(dir $@)
	sed 's|@PREFIX@|$(KEILAND_PREFIX)|g' $< > $@.tmp
	mv $@.tmp $@
KEILAND_LINUX_ALL += $(KEILAND_LINUX_BUILD)/etc/keiland/apps.conf
KEILAND_LINUX_INSTALL += etc/keiland/apps.conf

# The console launcher shares its shell implementation with the FreeBSD build.
$(KEILAND_LINUX_BUILD)/bin/keiland-desktop: userland/desktop/wayland/keiland-desktop.in
	@mkdir -p $(dir $@)
	sed 's|@PREFIX@|$(KEILAND_PREFIX)|g' $< > $@.tmp
	chmod 0755 $@.tmp
	mv $@.tmp $@
KEILAND_LINUX_ALL += $(KEILAND_LINUX_BUILD)/bin/keiland-desktop
KEILAND_LINUX_INSTALL += bin/keiland-desktop

# Bundled gradients are generated outside git (PNG, ws138-p002); the default picture is the generated Dawn (2026-10-08 user), the tree's Birch-Lake.png (U3's default) is in the catalogue.
KEILAND_LINUX_WALLPAPER_NAMES := Aurora Dawn Lagoon Meadow Twilight
KEILAND_LINUX_WALLPAPERS := $(addprefix $(KEILAND_LINUX_BUILD)/share/keiland/wallpapers/,$(addsuffix .png,$(KEILAND_LINUX_WALLPAPER_NAMES)))
$(KEILAND_LINUX_WALLPAPERS) &: userland/desktop/wallpapers/generate.py
	python3 $< $(KEILAND_LINUX_BUILD)/share/keiland/wallpapers
KEILAND_LINUX_WALLPAPER ?= $(KEILAND_LINUX_BUILD)/share/keiland/wallpapers/Dawn.png
$(eval $(call KEILAND_LINUX_DATA,share/keiland/wallpaper.png,$(KEILAND_LINUX_WALLPAPER)))
$(eval $(call KEILAND_LINUX_DATA,share/keiland/wallpapers/Birch-Lake.png,userland/desktop/wallpapers/Birch-Lake.png))
# The compositor's own landscape is offered in the catalogue too (ws099-p019, 2026-10-05 user).
$(eval $(call KEILAND_LINUX_DATA,share/keiland/wallpapers/Lakeside.png,userland/desktop/wallpapers/Lakeside.png))
KEILAND_LINUX_ALL += $(KEILAND_LINUX_WALLPAPERS)
KEILAND_LINUX_INSTALL += $(addprefix share/keiland/wallpapers/,$(addsuffix .png,$(filter-out Dawn,$(KEILAND_LINUX_WALLPAPER_NAMES))))

# The Japanese dictionary is in the tree (userland/desktop/ime/dict/SKK-JISYO.ja, one file of the supplement and
# REmacs's dictionary since ws095-p017); the copyright holder's WS095 D1 relicensing places it under the project license.
$(eval $(call KEILAND_LINUX_DATA,share/keiland/ime/ja/SKK-JISYO.ja,userland/desktop/ime/dict/SKK-JISYO.ja))
# The catalogs of the desktop's translations (WS158): share/keiland/locale/LANGUAGE/DOMAIN.tr.
$(foreach catalog,$(sort $(wildcard userland/desktop/locale/*/*.tr)),$(eval $(call KEILAND_LINUX_DATA,share/keiland/locale/$(patsubst userland/desktop/locale/%,%,$(catalog)),$(catalog))))
# The SKK engine's dictionaries (WS154): REmacs's two, copied apart into userland/desktop/ime/skk-dict, under zlib.
$(eval $(call KEILAND_LINUX_DATA,share/keiland/ime/skk/SKK-JISYO.X,userland/desktop/ime/skk-dict/SKK-JISYO.X))
$(eval $(call KEILAND_LINUX_DATA,share/keiland/ime/skk/SKK-JISYO.remacs,userland/desktop/ime/skk-dict/SKK-JISYO.remacs))

.PHONY: all install clean
all: $(KEILAND_LINUX_ALL)

# Libraries an earlier Keiland installed that are part of another now: the widgets joined libkeiland (WS131 p012).
# An install over an earlier one removes them, so that nothing loads a stale copy.
KEILAND_LINUX_RETIRED := lib/libkeiui.so

install: all
	@set -e; for f in $(KEILAND_LINUX_INSTALL); do \
		mode=0644; case $$f in lib/*|bin/*|libexec/*) mode=0755 ;; esac; \
		install -D -m $$mode $(KEILAND_LINUX_BUILD)/$$f $(DESTDIR)$(KEILAND_PREFIX)/$$f; \
	done
	@set -e; for f in $(KEILAND_LINUX_RETIRED); do rm -f $(DESTDIR)$(KEILAND_PREFIX)/$$f; done

clean:
	rm -rf $(KEILAND_LINUX_BUILD)/obj $(KEILAND_LINUX_BUILD)/lib $(KEILAND_LINUX_BUILD)/bin $(KEILAND_LINUX_BUILD)/libexec \
		$(KEILAND_LINUX_BUILD)/include $(KEILAND_LINUX_BUILD)/share $(KEILAND_LINUX_BUILD)/etc $(KEILAND_LINUX_BUILD)/gen \
		$(KEILAND_LINUX_BUILD)/stage

.PHONY: install-session print-sources header-dependencies
$(KEILAND_LINUX_BUILD)/share/wayland-sessions/keiland.desktop: userland/desktop/wayland/data/keiland.desktop
	@mkdir -p $(dir $@)
	sed 's|/opt/keiland|$(KEILAND_PREFIX)|g' $< > $@.tmp
	mv $@.tmp $@
install-session: $(KEILAND_LINUX_BUILD)/share/wayland-sessions/keiland.desktop
	install -D -m 0644 $< $(DESTDIR)/usr/share/wayland-sessions/keiland.desktop
print-sources:
	@printf '%s\n' $(KEILAND_LINUX_SOURCES)

# Include system headers in the audit; -MMD deliberately omits them from ordinary build dependencies.
KEILAND_LINUX_HEADER_DEPS := $(patsubst %.c,$(KEILAND_LINUX_BUILD)/header-check/%.d,$(KEILAND_LINUX_SOURCES))
header-dependencies: $(KEILAND_LINUX_HEADER_DEPS)
$(KEILAND_LINUX_BUILD)/header-check/%.d: %.c | $(KEILAND_LINUX_HEADER_COPIES)
	@mkdir -p $(dir $@)
	$(CC) $(KEILAND_LINUX_CFLAGS) $(KEILAND_LINUX_CPPFLAGS) $(KEILAND_LINUX_CPPFLAGS_$(subst /,_,$(dir $<))) -M $< -o $@
