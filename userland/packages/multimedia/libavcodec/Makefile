# zedBSD userland package: FFmpeg's libraries for playing audio and video (external release tarball, WS122).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# FFmpeg stays outside the source tree: only this Makefile and the recorded archive identity live here.  It is
# built as LGPL 2.1 or later: no --enable-gpl, --enable-version3 or --enable-nonfree, and --disable-autodetect so
# that nothing on the build machine is linked in.  The libraries are shared, so a user can replace them (the LGPL's
# relinking).  The license audit and the configuration are in plan/ws122/phase001/phase.md.
#
# The package is named after libavcodec, the decoders; it installs what a player needs with it: libavutil,
# libswresample (audio conversion), libavcodec, libavformat (containers) and libswscale (pixel conversion).
ZEDBSD_LIBAVCODEC_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_LIBAVCODEC_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_LIBAVCODEC_MAKEFILE)))
include $(ZEDBSD_LIBAVCODEC_DIR)/../../../base/package.mk
include $(ZEDBSD_LIBAVCODEC_DIR)/../../external.mk

# 9.0.2 was the latest release when recorded (2026-10-05).  Size and digest are the upstream bytes as measured; the
# archive's signature (ffmpeg-9.0.2.tar.xz.asc) was good with the FFmpeg release signing key,
# FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8.
ZEDBSD_EXT_libavcodec_VERSION := 9.0.2
ZEDBSD_EXT_libavcodec_ROOT := ffmpeg-9.0.2
ZEDBSD_EXT_libavcodec_ARCHIVE := ffmpeg-9.0.2.tar.xz
ZEDBSD_EXT_libavcodec_URL := https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz
ZEDBSD_EXT_libavcodec_SIZE := 12040788
ZEDBSD_EXT_libavcodec_SHA256 := 8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
ZEDBSD_EXT_libavcodec_PATCH_LEVEL := zedbsd0
ZEDBSD_EXT_libavcodec_PATCHES :=

$(eval $(call ZEDBSD_EXTERNAL_SOURCE,libavcodec))

# ---------------------------------------------------------------- build

# The libraries, by their sonames, in the order they depend on each other.
ZEDBSD_LIBAVCODEC_LIBRARIES := libavutil.so.61 libswresample.so.7 libavcodec.so.63 libavformat.so.63 libswscale.so.10
ZEDBSD_LIBAVCODEC_CONFIGURED := $(ZEDBSD_EXT_libavcodec_BUILDDIR)/.zedbsd-configured
ZEDBSD_LIBAVCODEC_STAGED := $(ZEDBSD_EXT_libavcodec_STAGEDIR)/.zedbsd-staged
ZEDBSD_LIBAVCODEC_STAGED_LIBRARIES := $(addprefix $(ZEDBSD_EXT_libavcodec_STAGEDIR)/usr/lib/,$(ZEDBSD_LIBAVCODEC_LIBRARIES))
ZEDBSD_LIBAVCODEC_LICENSES := $(ZEDBSD_EXT_libavcodec_SRCDIR)/COPYING.LGPLv2.1 $(ZEDBSD_EXT_libavcodec_SRCDIR)/LICENSE.md

# What is built, and why the rest is not:
#   the decoders, demuxers, parsers and bitstream filters (a player's), the file protocol, the pixel and sample
#   conversion;
#   no encoders or muxers (nothing here writes media), no filters (libavfilter) or devices (libavdevice), no network
#   protocols (a player of files), no hardware decoding (no VA-API here: WS083 brings the GPU's), no programs, no
#   documentation, no debug information;
#   target assembly stays for speed (x86 uses nasm; AArch64 uses the cross compiler).
# configure is told the cross tools by name; zedBSD is not one of its systems, so the target is "none" (ELF, POSIX
# threads are found by their probe).
ZEDBSD_LIBAVCODEC_ARCH := $(strip \
	$(if $(filter amd64,$(ZEDBSD_ARCHITECTURE)),x86_64,\
	$(if $(filter i386,$(ZEDBSD_ARCHITECTURE)),x86_32,\
	$(if $(filter arm64,$(ZEDBSD_ARCHITECTURE)),aarch64,\
	$(if $(filter sparcv9,$(ZEDBSD_ARCHITECTURE)),sparc64,$(ZEDBSD_ARCHITECTURE))))))
ZEDBSD_LIBAVCODEC_ELF_MACHINE := $(if $(filter arm64,$(ZEDBSD_ARCHITECTURE)),aarch64,$(ZEDBSD_ARCHITECTURE))
ZEDBSD_LIBAVCODEC_CONFIGURE_OPTIONS := \
	--prefix=/usr --libdir=/usr/lib --shlibdir=/usr/lib --incdir=/usr/include \
	--enable-cross-compile --target-os=none --arch=$(ZEDBSD_LIBAVCODEC_ARCH) \
	--pkg-config=false $(if $(filter x86_64 x86_32,$(ZEDBSD_LIBAVCODEC_ARCH)),--x86asmexe=nasm) \
	--disable-autodetect --disable-gpl --disable-nonfree \
	--disable-static --enable-shared --enable-pic --enable-pthreads \
	--disable-programs --disable-doc --disable-debug --disable-stripping \
	--disable-encoders --disable-muxers --disable-hwaccels --disable-devices \
	--disable-avdevice --disable-avfilter --disable-network \
	--disable-protocols --enable-protocol=file \
	--disable-symver

# The loader does not read symbol versions (as for libpng and pcre2): the version scripts keep their global and
# local lists (only the av*, swr* and sws* names are exported) but lose their version names, so the libraries define
# no versions; --disable-symver above leaves out the versioned compatibility aliases.
ZEDBSD_LIBAVCODEC_VERSION_SCRIPT := VERSION_SCRIPT_POSTPROCESS_CMD="sed -e 's/^LIB[A-Z]*_[0-9][0-9]* *{/{/'"

# The options are read when the tree is configured, so this file is an input.
$(ZEDBSD_LIBAVCODEC_CONFIGURED): $(ZEDBSD_EXT_libavcodec_SRCSTAMP) \
	$(ZEDBSD_EXTERNAL_CROSS_STAMP) $(ZEDBSD_LIBAVCODEC_MAKEFILE)
	@rm -rf '$(ZEDBSD_EXT_libavcodec_BUILDDIR)'
	@mkdir -p '$(ZEDBSD_EXT_libavcodec_BUILDDIR)'
	cd '$(ZEDBSD_EXT_libavcodec_BUILDDIR)' && \
		. '$(ZEDBSD_EXTERNAL_CROSS_ENV)' && \
		'$(ZEDBSD_EXT_libavcodec_SRCDIR)/configure' \
			$(ZEDBSD_LIBAVCODEC_CONFIGURE_OPTIONS) \
			--cc="$$CC" --cxx="$$CXX" --ar="$$AR" --ranlib="$$RANLIB" \
			--nm="$$NM" --strip="$$STRIP"
	@grep -q '^License: LGPL version 2.1 or later' '$(ZEDBSD_EXT_libavcodec_BUILDDIR)/ffbuild/config.log' || \
		grep -q 'FFMPEG_LICENSE "LGPL version 2.1 or later"' '$(ZEDBSD_EXT_libavcodec_BUILDDIR)/config.h' || \
		{ echo 'libavcodec: the configuration is not LGPL 2.1 or later' >&2; exit 1; }
	@touch '$@'

$(ZEDBSD_LIBAVCODEC_STAGED): $(ZEDBSD_LIBAVCODEC_CONFIGURED)
	@rm -rf '$(ZEDBSD_EXT_libavcodec_STAGEDIR)'
	$(MAKE) -C '$(ZEDBSD_EXT_libavcodec_BUILDDIR)' $(ZEDBSD_LIBAVCODEC_VERSION_SCRIPT)
	$(MAKE) -C '$(ZEDBSD_EXT_libavcodec_BUILDDIR)' $(ZEDBSD_LIBAVCODEC_VERSION_SCRIPT) \
		DESTDIR='$(ZEDBSD_EXT_libavcodec_STAGEDIR)' install
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_LIBAVCODEC_ELF_MACHINE) --role shared-library \
		--soname libavutil.so.61 --needed libc.so \
		'$(ZEDBSD_EXT_libavcodec_STAGEDIR)/usr/lib/libavutil.so.61'
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_LIBAVCODEC_ELF_MACHINE) --role shared-library \
		--soname libavcodec.so.63 --needed libswresample.so.7 --needed libavutil.so.61 --needed libc.so \
		'$(ZEDBSD_EXT_libavcodec_STAGEDIR)/usr/lib/libavcodec.so.63'
	@touch '$@'

$(ZEDBSD_LIBAVCODEC_STAGED_LIBRARIES): $(ZEDBSD_LIBAVCODEC_STAGED)
	@test -f '$@'

$(ZEDBSD_LIBAVCODEC_LICENSES): $(ZEDBSD_EXT_libavcodec_SRCSTAMP)
	@test -f '$@'

.PHONY: libavcodec
libavcodec: $(ZEDBSD_LIBAVCODEC_STAGED)
	@:

# ---------------------------------------------------------------- packaging

$(eval $(call ZEDBSD_USERLAND_PACKAGE,libavcodec,FFmpeg audio and video libraries (LGPL),*,n,package,,y,\
	packages/multimedia,multimedia/libavcodec,))

# The runtime: each library under its soname (the name the loader asks for) and the license texts.  The headers and
# the unversioned links belong to the link step and stay in the staging tree, which the player is built against.
ZEDBSD_PACKAGE_INPUTS += $(if $(filter libavcodec,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_LIBAVCODEC_STAGED_LIBRARIES) $(ZEDBSD_LIBAVCODEC_LICENSES))
ZEDBSD_PACKAGE_FILES += $(if $(filter libavcodec,$(ZEDBSD_USER_PROGRAMS)),\
	$(foreach library,$(ZEDBSD_LIBAVCODEC_LIBRARIES),--file /usr/lib/$(library)=$(ZEDBSD_EXT_libavcodec_STAGEDIR)/usr/lib/$(library)) \
	--file /usr/share/licenses/ffmpeg/COPYING.LGPLv2.1=$(ZEDBSD_EXT_libavcodec_SRCDIR)/COPYING.LGPLv2.1 \
	--file /usr/share/licenses/ffmpeg/LICENSE.md=$(ZEDBSD_EXT_libavcodec_SRCDIR)/LICENSE.md)
