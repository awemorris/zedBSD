# zedBSD userland package: fontconfig (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# fontconfig is under an MIT-style licence (HPND-sell-variant; the full text
# is its COPYING) and stays outside the source tree: only this Makefile and
# the recorded archive identity live here.  It finds and matches the fonts
# Pango draws with, for the GTK port (WS115) and later Qt (WS116).
# Provenance and the choice of 2.17.1 (2.18 needs a newer Meson than the
# build machine has) are in plan/ws115/port-contract.md.
FONTCONFIG_MAKEFILE := $(lastword $(MAKEFILE_LIST))
FONTCONFIG_DIR := $(patsubst %/,%,$(dir $(FONTCONFIG_MAKEFILE)))
include $(FONTCONFIG_DIR)/../../../base/package.mk
include $(FONTCONFIG_DIR)/../../external.mk

# Size and digest are the upstream bytes as measured; they match the digest
# the project publishes beside the archive.
EXT_fontconfig_VERSION := 2.17.1
EXT_fontconfig_ROOT := fontconfig-2.17.1
EXT_fontconfig_ARCHIVE := fontconfig-2.17.1.tar.xz
EXT_fontconfig_URL := https://gitlab.freedesktop.org/api/v4/projects/890/packages/generic/fontconfig/2.17.1/fontconfig-2.17.1.tar.xz
EXT_fontconfig_SIZE := 1326312
EXT_fontconfig_SHA256 := 9f5cae93f4fffc1fbc05ae99cdfc708cd60dfd6612ffc0512827025c026fa541
EXT_fontconfig_PATCH_LEVEL := zedbsd0
EXT_fontconfig_PATCHES :=

$(eval $(call EXTERNAL_SOURCE,fontconfig))

# ---------------------------------------------------------------- build

# Meson, through the common rule, with the build machine's gperf 3.3
# (devel/gperf, port-contract §4) for the generated lookup tables and expat
# for the configuration files.  What is set, and why:
#   default-fonts-dirs     /usr/share/fonts, where zedBSD installs its fonts
#   additional-fonts-dirs  no: the generic default probes the build machine's
#                          own X11 font directories, which says nothing about
#                          the target
#   cache-build            off: fc-cache cannot run on the build machine; the
#                          cache (/var/cache/fontconfig, or the user's own
#                          under ~/.cache) is made on the target
#   tools                  on: fc-cache, fc-list and fc-match are how the
#                          cache is made and the configuration is checked
#   iconv, nls, doc, tests off, as for GLib
EXT_fontconfig_MAKEFILE := $(FONTCONFIG_MAKEFILE)
EXT_fontconfig_DEPENDS := freetype expat
EXT_fontconfig_HOST_TOOLS := $(GPERF_HOST)
EXT_fontconfig_MESON_OPTIONS := \
	-Ddoc=disabled -Dnls=disabled -Dtests=disabled -Dtools=enabled \
	-Dcache-build=disabled -Diconv=disabled -Dxml-backend=expat \
	-Dfontations=disabled \
	-Ddefault-fonts-dirs=/usr/share/fonts -Dadditional-fonts-dirs=no

$(eval $(call EXTERNAL_MESON,fontconfig))

FONTCONFIG_STAGED := $(EXT_fontconfig_STAGEDIR)/.zedbsd-staged
FONTCONFIG_CHECKED := $(EXT_fontconfig_STAGEDIR)/.zedbsd-checked
FONTCONFIG_LICENSE := $(EXT_fontconfig_SRCDIR)/COPYING

# The library under its SONAME, with the libraries it needs; the file name
# carries the libtool version upstream computes (1.16.0 for 2.17.1).  The
# list is the dependency contract.
FONTCONFIG_SONAME := libfontconfig.so.1
FONTCONFIG_LIBRARY := $(EXT_fontconfig_STAGEDIR)/usr/lib/libfontconfig.so.1.16.0
FONTCONFIG_NEEDED := libfreetype.so.6 libexpat.so.1 libc.so

# The tools that make the cache and show what the configuration finds.
FONTCONFIG_TOOLS := fc-cache fc-list fc-match fc-query fc-cat

# The configuration files upstream turns on by default in conf.d, for the
# options above.  conf.d holds them as plain files rather than links into
# the templates: the image is built from a file list, and a changed list
# upstream shows up here.
FONTCONFIG_CONF := 10-hinting-slight.conf 10-scale-bitmap-fonts.conf \
	10-sub-pixel-none.conf 10-yes-antialias.conf 11-lcdfilter-default.conf \
	20-unhint-small-vera.conf 30-metric-aliases.conf 40-nonlatin.conf \
	45-generic.conf 45-latin.conf 48-spacing.conf 49-sansserif.conf \
	50-user.conf 51-local.conf 60-generic.conf 60-latin.conf \
	65-fonts-persian.conf 65-nonlatin.conf 69-unifont.conf \
	70-no-bitmaps-except-emoji.conf 80-delicious.conf 90-synthetic.conf
FONTCONFIG_CONF_AVAIL := $(EXT_fontconfig_STAGEDIR)/usr/share/fontconfig/conf.avail
FONTCONFIG_FONTS_CONF := $(EXT_fontconfig_STAGEDIR)/etc/fonts/fonts.conf

# zedBSD's own preference for the generic families, which names the fonts the
# desktop installs (files/55-zedbsd-families.conf says why).
FONTCONFIG_FAMILIES := $(FONTCONFIG_DIR)/files/55-zedbsd-families.conf

# Checks the library and the tools against their contract, and that the
# stage turned on exactly the configuration files listed above.
$(FONTCONFIG_CHECKED): $(FONTCONFIG_STAGED)
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(FONTCONFIG_SONAME) \
		$(foreach needed,$(FONTCONFIG_NEEDED),--needed $(needed)) \
		'$(FONTCONFIG_LIBRARY)'
	@set -e; $(foreach tool,$(FONTCONFIG_TOOLS),\
		$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role application \
		--needed $(FONTCONFIG_SONAME) --needed libc.so \
		'$(EXT_fontconfig_STAGEDIR)/usr/bin/$(tool)';)
	@enabled=$$(LC_ALL=C ls '$(EXT_fontconfig_STAGEDIR)/etc/fonts/conf.d' | grep '\.conf$$' | tr '\n' ' '); \
		if [ "$$enabled" != '$(strip $(FONTCONFIG_CONF)) ' ]; then \
			echo "fontconfig: conf.d differs from the declared list: $$enabled" >&2; \
			exit 1; \
		fi
	@touch '$@'

$(FONTCONFIG_LICENSE): $(EXT_fontconfig_SRCSTAMP)
	@test -f '$@'

.PHONY: fontconfig
fontconfig: $(FONTCONFIG_CHECKED)
	@:

# ---------------------------------------------------------------- packaging

# The library under its SONAME, the tools, fonts.conf, the enabled
# configuration files and zedBSD's family preference.  The headers and the .pc file go with the development
# packages of the GTK port (WS115 p002).  The cache directory is not shipped:
# fc-cache makes /var/cache/fontconfig when root runs it, and every other user
# gets a cache of their own.
$(eval $(call USERLAND_PACKAGE,fontconfig,Font configuration library,*,n,package,,y,\
	packages/libs,libs/fontconfig,libs/freetype libs/expat))

PACKAGE_INPUTS += $(if $(filter fontconfig,$(USER_PROGRAMS)),\
	$(FONTCONFIG_CHECKED) $(FONTCONFIG_LICENSE) $(FONTCONFIG_FAMILIES))
PACKAGE_FILES += $(if $(filter fontconfig,$(USER_PROGRAMS)),\
	--file /usr/lib/$(FONTCONFIG_SONAME)=$(FONTCONFIG_LIBRARY) \
	$(foreach tool,$(FONTCONFIG_TOOLS),\
		--file $(PACKAGE_BINDIR)/$(tool)=$(EXT_fontconfig_STAGEDIR)/usr/bin/$(tool) \
		--mode $(PACKAGE_BINDIR)/$(tool)=0755) \
	--file /etc/fonts/fonts.conf=$(FONTCONFIG_FONTS_CONF) \
	$(foreach conf,$(FONTCONFIG_CONF),\
		--file /etc/fonts/conf.d/$(conf)=$(FONTCONFIG_CONF_AVAIL)/$(conf)) \
	--file /etc/fonts/conf.d/55-zedbsd-families.conf=$(FONTCONFIG_FAMILIES) \
	--file /usr/share/licenses/fontconfig/COPYING=$(FONTCONFIG_LICENSE))

# The development files: the headers, the .pc files and the unversioned names
# of the libraries (external.mk).
$(eval $(call EXTERNAL_DEVELOPMENT,fontconfig,$(EXT_fontconfig_STAGEDIR),\
	lib/pkgconfig/fontconfig.pc,\
	include/fontconfig,\
	$(FONTCONFIG_SONAME)))
