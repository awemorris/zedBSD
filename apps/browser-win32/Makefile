# OmniGrammaton for Windows: the cross build of libbrowser, libmedia, libkeiland and the browser's user
# interface into Windows programs.  Run from the top of the tree:
#   make -f apps/browser-win32/Makefile WIN32_ARCH=x86_64    (or aarch64)
# or through the top Makefile: make browser-win64, make browser-winarm64.
# Output under $(BROWSER_WIN32_BUILD) (build/browser-win64 or build/browser-winarm64 by default).
#
# Windows gets no DLL of this tree: every library is a static library, and a program links them all
# into one .exe that imports only Windows' own DLLs, the C runtime's (UCRT) and, delayed, the Vulkan
# loader.  The threads of libbrowser and libmedia are winpthreads', linked statically; libkeiland's
# Windows files use the system's threads and locks directly.
#
# What it builds so far: the files of the operating-system layers (libbrowser's base/os-*, its storage,
# libmedia's media-os, libkeiland's Windows files), compiled with the Windows compiler and its warnings;
# the static libraries: libbrowser.a (all of libbrowser, with its Windows layer), libmedia.a (all of
# libmedia and its container reader, with media-os-win32.c), libkeiland.a (its Windows files so far), libtruetype and the png, jpeg, gif and z compat libraries
# (from the same source lists as the zedBSD build); the Vulkan headers and the import library of the
# Vulkan loader, and OpenSSL (libssl.a and libcrypto.a, cross-built by plan/ws334/tests/openssl-windows.sh
# from the release the zedBSD package uses; libbrowser's net/tls.c uses it through net/tls-static.c).
# A test's program links with them all (test-program); the
# browser's program and its resources join later, linked the same way (WIN32_PROGRAM).  Apart from those:
# libbrowser-script.a, the script core alone with its Windows layer, and script-core.exe over it.
#
# The toolchain, first found wins:
#   1. WIN32_CC=<compiler>
#   2. LLVM_MINGW=<root of llvm-mingw>: $(LLVM_MINGW)/bin/<triple>-clang
#   3. <triple>-clang on PATH
#   4. x86_64 only: x86_64-w64-mingw32ucrt-gcc on PATH (Debian's UCRT mingw-w64)
# where <triple> is x86_64-w64-mingw32 or aarch64-w64-mingw32.  The msvcrt mingw-w64 gcc
# (x86_64-w64-mingw32-gcc) is never used: the build targets the UCRT.
# Recipes run under a POSIX sh (also on a Windows host: Git for Windows' or MSYS2's).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

WIN32_ARCH ?= x86_64
ifeq ($(WIN32_ARCH),x86_64)
WIN32_NAME := win64
else ifeq ($(WIN32_ARCH),aarch64)
WIN32_NAME := winarm64
else
$(error WIN32_ARCH must be x86_64 or aarch64, not '$(WIN32_ARCH)')
endif
WIN32_TRIPLE := $(WIN32_ARCH)-w64-mingw32
BROWSER_WIN32_BUILD ?= build/browser-$(WIN32_NAME)

# The compiler and the binutils prefix that goes with it.
ifeq ($(strip $(WIN32_CC)),)
ifneq ($(strip $(LLVM_MINGW)),)
WIN32_CC := $(LLVM_MINGW)/bin/$(WIN32_TRIPLE)-clang
else ifneq ($(shell command -v $(WIN32_TRIPLE)-clang 2>/dev/null),)
WIN32_CC := $(WIN32_TRIPLE)-clang
else ifeq ($(WIN32_ARCH),x86_64)
ifneq ($(shell command -v x86_64-w64-mingw32ucrt-gcc 2>/dev/null),)
WIN32_CC := x86_64-w64-mingw32ucrt-gcc
endif
endif
endif
ifeq ($(strip $(WIN32_CC)),)
$(error no Windows compiler for $(WIN32_ARCH): install llvm-mingw and pass LLVM_MINGW=<its root> (for example LLVM_MINGW=$$HOME/opt/llvm-mingw), or WIN32_CC=<compiler>)
endif
WIN32_TOOL_PREFIX ?= $(patsubst %ucrt-gcc,%-,$(patsubst %-clang,%-,$(patsubst %-gcc,%-,$(WIN32_CC))))
WIN32_AR ?= $(WIN32_TOOL_PREFIX)ar
WIN32_DLLTOOL ?= $(WIN32_TOOL_PREFIX)dlltool
ifeq ($(WIN32_ARCH),x86_64)
WIN32_DLLTOOL_MACHINE := i386:x86-64
else
WIN32_DLLTOOL_MACHINE := arm64
endif

BROWSER_WIN32_OBJ := $(BROWSER_WIN32_BUILD)/obj
# The directory of the zip's contents (what a user runs).
BROWSER_WIN32_DIST := $(BROWSER_WIN32_BUILD)/OmniGrammaton
# Where the fetched tarballs are kept (fetch).
BROWSER_WIN32_SOURCES ?= $(BROWSER_WIN32_BUILD)/sources

# The flags of every file this build compiles.  The files written for this build are held to warning 0;
# libtruetype and the compat libraries are compiled as the zedBSD tree has them, without -Werror, and
# their warnings are kept beside each object (<object>.log) and counted in toolchain.txt.
# $(BROWSER_WIN32_OBJ)/include holds the compat headers and the Vulkan headers, copied (not -Iinclude/libc,
# which would hide the C runtime's headers).
WIN32_COMMON_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -MMD -MP \
	-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE \
	-I. -Iuserland/desktop/include -Iuserland/desktop/libbrowser -Iuserland/desktop/libmedia \
	-I$(BROWSER_WIN32_OBJ)/include
WIN32_CFLAGS := $(WIN32_COMMON_CFLAGS) -Werror
WIN32_COMPAT_CFLAGS := $(WIN32_COMMON_CFLAGS)

# What a program links with.  -static takes every library's static form: winpthreads (which llvm-mingw
# links only when named) goes into the .exe, and the C runtime and the system's libraries, which have
# no static form, stay imports of the UCRT's and Windows' own DLLs.  Vulkan goes through the loader's
# import library as a delayed import (lld's --delayload): a program starts on a machine without
# vulkan-1.dll and calls no vk* there.  The system's libraries are the ones the libraries call so far;
# each file that calls another adds its library here.
WIN32_THREAD_LDFLAGS := -lpthread
WIN32_VULKAN_LDFLAGS := -L$(BROWSER_WIN32_OBJ) -lvulkan-1 -Wl,--delayload=vulkan-1.dll
WIN32_SYSTEM_LDFLAGS := -lws2_32 -liphlpapi -lbcrypt -lcrypt32 -ldwmapi -lshcore -limm32 -lshell32 -lole32 -luuid \
	-lgdi32 -luser32 -ladvapi32
WIN32_PROGRAM_LDFLAGS := -static $(WIN32_VULKAN_LDFLAGS) $(WIN32_THREAD_LDFLAGS) $(WIN32_SYSTEM_LDFLAGS)

# The operating-system layers: the Windows files and the files every system shares.
BROWSER_WIN32_LAYER_SOURCES := \
	userland/desktop/libbrowser/base/os-win32.c \
	userland/desktop/libbrowser/base/os-system-win32.c \
	userland/desktop/libbrowser/base/os-file-win32.c \
	userland/desktop/libbrowser/base/os-net-win32.c \
	userland/desktop/libbrowser/base/os-time-win32.c \
	userland/desktop/libbrowser/base/os-zones.c \
	userland/desktop/libbrowser/storage/records.c \
	userland/desktop/libbrowser/storage/files.c \
	userland/desktop/libbrowser/storage/queue.c \
	userland/desktop/libmedia/media-os-win32.c \
	userland/desktop/libkeiland/platform/platform-win32.c \
	userland/desktop/libkeiland/platform/thread-win32.c \
	userland/desktop/libkeiland/platform/socket-win32.c \
	userland/desktop/libkeiland/win32/keys-table.c \
	userland/desktop/libkeiland/win32/frame.c \
	userland/desktop/libkeiland/win32/wide.c \
	userland/desktop/libkeiland/win32/input.c \
	userland/desktop/libkeiland/win32/cf-html.c

BROWSER_WIN32_LAYER_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(BROWSER_WIN32_LAYER_SOURCES))

# libbrowser: the zedBSD build's list less its POSIX operating-system layer, which the Windows layer
# above replaces.
include userland/desktop/libbrowser/sources.mk
LIBBROWSER_CORE_EXCLUDED := $(LIBBROWSER_OS_SOURCES) \
	$(LIBBROWSER_OS_NET_SOURCES)
LIBBROWSER_CORE_SOURCES := $(filter-out $(LIBBROWSER_CORE_EXCLUDED),$(LIBBROWSER_SOURCES))
# TLS: tls.c uses the OpenSSL linked into the program (WB_TLS_STATIC) through tls-static.c, which only
# this build compiles (it is not in sources.mk: the zedBSD build opens the package's shared libraries).
LIBBROWSER_CORE_SOURCES += $(LIBBROWSER_DIR)/net/tls-static.c
LIBBROWSER_CORE_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(LIBBROWSER_CORE_SOURCES))

# libbrowser's script core alone (the base library, the virtual machine and JavaScript), with the Windows
# files of the operating-system layer it uses (the system, the files and the clocks, not the network):
# libbrowser-script.a, the library a program that runs only JavaScript links.  Its objects are
# libbrowser.a's.  script-core.exe (a test's program over it and nothing else of libbrowser) links only
# when the core needs nothing of the engine.
LIBBROWSER_SCRIPT_WIN32_LAYER := $(LIBBROWSER_DIR)/base/os-win32.c \
	$(LIBBROWSER_DIR)/base/os-system-win32.c \
	$(LIBBROWSER_DIR)/base/os-file-win32.c \
	$(LIBBROWSER_DIR)/base/os-time-win32.c \
	$(LIBBROWSER_DIR)/base/os-zones.c
WIN32_SOURCES_browser-script := $(filter-out $(LIBBROWSER_OS_SOURCES),$(LIBBROWSER_SCRIPT_SOURCES)) \
	$(LIBBROWSER_SCRIPT_WIN32_LAYER)
LIBBROWSER_SCRIPT_ARCHIVE := $(BROWSER_WIN32_OBJ)/libbrowser-script.a
SCRIPT_CORE_SOURCES := plan/ws334/tests/script-core-main.c
SCRIPT_CORE_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(SCRIPT_CORE_SOURCES))
SCRIPT_CORE_PROGRAM := $(BROWSER_WIN32_BUILD)/tests/script-core.exe

# libmedia (with the container reader built into it): the zedBSD build's list less its POSIX
# operating-system layer, which media-os-win32.c of the layers above replaces.
include userland/desktop/libmedia/sources.mk
LIBMEDIA_CORE_SOURCES := $(filter-out $(LIBMEDIA_OS_SOURCES),$(LIBMEDIA_SOURCES))
LIBMEDIA_CORE_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(LIBMEDIA_CORE_SOURCES))

# libkeiland on Windows: the files of its source list that compile as they are (they include nothing of
# Wayland or POSIX), its Windows files, and the stubs of every public function neither has (generated:
# int ENOTSUP, a pointer NULL, nothing for void).  The rest of the list is the Wayland backend's.
LIBKEILAND_WIN32_COMMON_SOURCES := \
	userland/desktop/libkeiland/version.c \
	userland/desktop/libkeiland/motion.c \
	userland/desktop/libkeiland/scroll.c \
	userland/desktop/libkeiland/gesture.c \
	userland/desktop/libkeiland/settings-cache.c \
	userland/desktop/settings-keys/settings-keys.c \
	userland/desktop/libkeiland/translate-follow.c \
	userland/desktop/libkeiland/system/system-monitor-rate.c \
	userland/desktop/libkeiland/ui/canvas.c \
	userland/desktop/libkeiland/ui/drop-look.c \
	userland/desktop/libkeiland/ui/icons.c \
	userland/desktop/libkeiland/ui/icons-line.c \
	userland/desktop/libkeiland/ui/theme.c \
	userland/desktop/libkeiland/ui/input.c \
	userland/desktop/libkeiland/ui/scroll.c \
	userland/desktop/libkeiland/ui/scroll-bar.c \
	userland/desktop/libkeiland/ui/text-touch.c \
	userland/desktop/libkeiland/ui/text-bar.c \
	userland/desktop/libkeiland/ui/text-select.c \
	userland/desktop/libkeiland/ui/ui.c \
	userland/desktop/libkeiland/launch.c \
	userland/desktop/libkeiland/ui/declare.c \
	userland/desktop/libkeiland/ui/present-copy.c \
	userland/desktop/libkeiland/ui/present-damage.c \
	userland/desktop/libkeiland/ui/text-area.c \
	userland/desktop/libkeiland/ui/widgets.c \
	userland/desktop/libkeiland/ui/field.c \
	userland/desktop/libkeiland/ui/list.c \
	userland/desktop/libkeiland/ui/views.c \
	userland/desktop/libkeiland/ui/cards.c \
	userland/desktop/libkeiland/ui/chooser-view.c \
	userland/desktop/libkeiland/ui/picker-view.c \
	userland/desktop/picture/color-glyph.c
LIBKEILAND_WIN32_OWN_SOURCES := \
	userland/desktop/libkeiland/win32/app.c \
	userland/desktop/libkeiland/win32/window.c \
	userland/desktop/libkeiland/win32/present.c \
	userland/desktop/libkeiland/win32/places.c \
	userland/desktop/libkeiland/win32/theme.c \
	userland/desktop/libkeiland/win32/titlebar.c \
	userland/desktop/libkeiland/win32/ime.c \
	userland/desktop/libkeiland/win32/clipboard.c
# Files of the list that read files through the C runtime's POSIX calls only, which the Windows C runtime
# has too (the fonts, read as binary).
LIBKEILAND_WIN32_CRT_SOURCES := userland/desktop/libkeiland/ui/text.c
LIBKEILAND_WIN32_STUB_SOURCES := userland/desktop/libkeiland/win32/unsupported.c
LIBKEILAND_WIN32_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(LIBKEILAND_WIN32_COMMON_SOURCES) \
	$(LIBKEILAND_WIN32_CRT_SOURCES) $(LIBKEILAND_WIN32_OWN_SOURCES))

# The browser's user interface (the zedBSD build's list less its POSIX entry), a static library the
# program's entry (main.c) and a test's console program link: its files find their own headers under
# userland/desktop/browser, and libkeiland's application is called OmniGrammaton.
include userland/desktop/browser/sources.mk
BROWSER_UI_SOURCES := $(filter-out $(KEILAND_BROWSER_DIR)/main-posix.c,$(KEILAND_BROWSER_SOURCES))
BROWSER_UI_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(BROWSER_UI_SOURCES))
WIN32_SOURCES_browser-ui := $(BROWSER_UI_SOURCES)
$(BROWSER_UI_OBJECTS): WIN32_CFLAGS += -I$(KEILAND_BROWSER_DIR) -DSHELL_APPLICATION_ID=\"OmniGrammaton\"

# OmniGrammaton.exe: the entry, the single start and the resources (the manifest, the version and the
# icon) over every static library, a program of the Windows subsystem; and the directory a user runs it
# from, with the fonts, the browser's pages, the trusted roots and the licences under share, and its zip.
include apps/browser-win32/version.mk
OMNIGRAMMATON_SOURCES := apps/browser-win32/main.c apps/browser-win32/single.c
OMNIGRAMMATON_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(OMNIGRAMMATON_SOURCES)) \
	$(BROWSER_WIN32_OBJ)/apps/browser-win32/resources/OmniGrammaton.res.o
OMNIGRAMMATON_PROGRAM := $(BROWSER_WIN32_BUILD)/OmniGrammaton.exe
# The program is linked with its debug information (OMNIGRAMMATON_FULL) and split in two: the .exe that
# is shipped has none, and OmniGrammaton.dbg (the DWARF, which a .exe keeps in sections; lld's --pdb
# makes a PDB only from CodeView) stays in the build's output, named by the .exe's .gnu_debuglink.
OMNIGRAMMATON_FULL := $(BROWSER_WIN32_OBJ)/OmniGrammaton-full.exe
OMNIGRAMMATON_DEBUG := $(BROWSER_WIN32_BUILD)/OmniGrammaton.dbg
WIN32_OBJCOPY ?= $(WIN32_TOOL_PREFIX)objcopy
OMNIGRAMMATON_VERSION_WORDS := $(subst ., ,$(OMNIGRAMMATON_VERSION))
WIN32_WINDRES ?= $(WIN32_TOOL_PREFIX)windres
OMNIGRAMMATON_SHARE := \
	fonts/keiland.ttf=userland/desktop/fonts/Mahora-Regular.ttf \
	fonts/keiland-bold.ttf=userland/desktop/fonts/Mahora-Bold.ttf \
	fonts/keiland-mono.ttf=userland/desktop/fonts/Mahora-Mono.ttf \
	fonts/keiland-fallback-mono.ttf=userland/desktop/fonts/JetBrainsMono-Regular.ttf \
	fonts/keiland-fallback.ttf=userland/desktop/fonts/DroidSansFallbackFull.ttf \
	browser/start.html=$(KEILAND_BROWSER_DIR)/data/start.html \
	browser/about.html=$(KEILAND_BROWSER_DIR)/data/about.html \
	browser/text.html=$(KEILAND_BROWSER_DIR)/data/text.html
OMNIGRAMMATON_SHARE_COPIES := $(foreach pair,$(OMNIGRAMMATON_SHARE),$(BROWSER_WIN32_DIST)/share/$(word 1,$(subst =, ,$(pair))))

# The icon: a host tool (make-ico, built with the host's compiler) draws the browser's picture as the
# Keiland desktop does (its icons.c) and writes OmniGrammaton.ico, which the resources take.
HOST_CC ?= cc
OMNIGRAMMATON_ICON := $(BROWSER_WIN32_OBJ)/OmniGrammaton.ico
MAKE_ICO := $(BROWSER_WIN32_OBJ)/host/make-ico
MAKE_ICO_SOURCES = apps/browser-win32/tools/make-ico.c apps/browser-win32/tools/ico.c \
	userland/desktop/wayland/icons.c userland/desktop/picture/png-write.c $(LIBZ_COMPAT_SOURCES)

# The trusted roots: the ca-certificates package's file (its release, size and SHA-256 read from the
# package), from the shared downloads when they have it, else fetched; as share/certs/ca-certificates.pem.
CA_CERTIFICATES_PACKAGE := userland/packages/security/ca-certificates/Makefile
CA_CERTIFICATES_ARCHIVE := $(shell sed -n 's/^ZEDBSD_EXT_ca-certificates_ARCHIVE := //p' $(CA_CERTIFICATES_PACKAGE))
CA_CERTIFICATES_URL := $(shell sed -n 's/^ZEDBSD_EXT_ca-certificates_URL := //p' $(CA_CERTIFICATES_PACKAGE))
CA_CERTIFICATES_SHA256 := $(shell sed -n 's/^ZEDBSD_EXT_ca-certificates_SHA256 := //p' $(CA_CERTIFICATES_PACKAGE))
CA_CERTIFICATES_FILE := $(BROWSER_WIN32_SOURCES)/$(CA_CERTIFICATES_ARCHIVE)

# The licences of what the program is made of (share/licenses, with a README.txt that lists them): the
# tree's, the fonts', the engine's tables', OpenSSL's (from its release, which the OpenSSL build
# extracted), the toolchain's parts linked in (llvm-mingw's MinGW-w64 runtime and winpthreads, and LLVM's
# compiler runtime) and the trusted roots'.
OPENSSL_RELEASE := $(shell sed -n 's/^ZEDBSD_EXT_openssl_VERSION := //p' userland/packages/security/openssl/Makefile)
WIN32_TOOLCHAIN_ROOT := $(abspath $(dir $(shell command -v $(WIN32_CC) 2>/dev/null || echo $(WIN32_CC)))..)
WIN32_MINGW_LICENSES := $(WIN32_TOOLCHAIN_ROOT)/$(WIN32_TRIPLE)/share/mingw32
OMNIGRAMMATON_LICENSES = \
	README.txt=apps/browser-win32/resources/licenses-README.txt \
	zedBSD-LICENSE.txt=LICENSE \
	Mahora-LICENSE.txt=userland/desktop/fonts/Mahora-LICENSE.txt \
	JetBrainsMono-OFL.txt=userland/desktop/fonts/JetBrainsMono-OFL.txt \
	DroidSansFallback-LICENSE.txt=userland/desktop/fonts/DroidSansFallback-LICENSE.txt \
	Apache-2.0.txt=userland/desktop/fonts/Apache-2.0.txt \
	WHATWG-entities.txt=userland/base/licenses/browser/WHATWG-entities \
	Unicode-License-V3.txt=userland/base/licenses/browser/Unicode-License-V3 \
	OpenSSL-LICENSE.txt=$(BROWSER_WIN32_OPENSSL)/work/src/openssl-$(OPENSSL_RELEASE)/LICENSE.txt \
	MinGW-w64-runtime.txt=$(WIN32_MINGW_LICENSES)/COPYING.MinGW-w64-runtime.txt \
	winpthreads.txt=$(WIN32_MINGW_LICENSES)/COPYING.winpthreads.txt \
	LLVM-LICENSE.txt=$(WIN32_TOOLCHAIN_ROOT)/LICENSE.TXT \
	ca-certificates-MPL-2.0.txt=userland/packages/security/ca-certificates/MPL-2.0.txt
OMNIGRAMMATON_LICENSE_COPIES := $(foreach pair,$(OMNIGRAMMATON_LICENSES),$(BROWSER_WIN32_DIST)/share/licenses/$(word 1,$(subst =, ,$(pair))))

# The zip: OmniGrammaton-<version>-<win64|winarm64>.zip of the directory, the same bytes from the same files.
OMNIGRAMMATON_ZIP := $(BROWSER_WIN32_BUILD)/OmniGrammaton-$(OMNIGRAMMATON_VERSION)-$(WIN32_NAME).zip

# The static libraries of the tree's libraries, each from its files of the Windows build so far:
# libbrowser (its storage modules, which the layers above list too, once) with its Windows layer,
# libmedia with media-os-win32.c, and libkeiland (its common files, its Windows files and the stubs).
WIN32_SOURCES_browser := $(LIBBROWSER_CORE_SOURCES) \
	$(filter-out $(LIBBROWSER_CORE_SOURCES),$(filter userland/desktop/libbrowser/%,$(BROWSER_WIN32_LAYER_SOURCES)))
WIN32_SOURCES_media := $(LIBMEDIA_CORE_SOURCES) $(filter userland/desktop/libmedia/%,$(BROWSER_WIN32_LAYER_SOURCES))
WIN32_SOURCES_keiland := $(LIBKEILAND_WIN32_COMMON_SOURCES) $(filter userland/desktop/libkeiland/%,$(BROWSER_WIN32_LAYER_SOURCES)) \
	$(LIBKEILAND_WIN32_CRT_SOURCES) $(LIBKEILAND_WIN32_OWN_SOURCES) $(LIBKEILAND_WIN32_STUB_SOURCES)
WIN32_TREE_LIBRARIES := browser-ui browser media keiland

# libtruetype and the compat libraries: the zedBSD build's source lists, each a static library a
# program links with libbrowser.a.
include userland/desktop/libtruetype/sources.mk
include userland/desktop/libpng-compat/sources.mk
include userland/desktop/libjpeg-compat/sources.mk
include userland/desktop/libgif-compat/sources.mk
include userland/base/libz-compat/sources.mk
WIN32_COMPAT_LIBRARIES := truetype png-compat jpeg-compat gif-compat z-compat
WIN32_SOURCES_truetype := $(LIBTRUETYPE_SOURCES)
WIN32_SOURCES_png-compat := $(LIBPNG_COMPAT_SOURCES)
WIN32_SOURCES_jpeg-compat := $(LIBJPEG_COMPAT_SOURCES)
WIN32_SOURCES_gif-compat := $(LIBGIF_COMPAT_SOURCES)
WIN32_SOURCES_z-compat := $(LIBZ_COMPAT_SOURCES)
WIN32_COMPAT_SOURCES := $(foreach library,$(WIN32_COMPAT_LIBRARIES),$(WIN32_SOURCES_$(library)))
WIN32_COMPAT_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(WIN32_COMPAT_SOURCES))
WIN32_COMPAT_ARCHIVES := $(foreach library,$(WIN32_COMPAT_LIBRARIES),$(BROWSER_WIN32_OBJ)/lib$(library).a)
WIN32_TREE_ARCHIVES := $(foreach library,$(WIN32_TREE_LIBRARIES),$(BROWSER_WIN32_OBJ)/lib$(library).a)

# OpenSSL for Windows (static, cross-built by plan/ws334/tests/openssl-windows.sh): the libraries and
# the headers tls-static.c includes.  The script runs once per output directory; its build tree is kept
# beside the output, so a second run only copies.
BROWSER_WIN32_OPENSSL := $(BROWSER_WIN32_BUILD)/openssl
WIN32_OPENSSL_LIBSSL := $(BROWSER_WIN32_OPENSSL)/lib/libssl.a
WIN32_OPENSSL_LIBCRYPTO := $(BROWSER_WIN32_OPENSSL)/lib/libcrypto.a
WIN32_OPENSSL_ARCHIVES := $(WIN32_OPENSSL_LIBSSL) $(WIN32_OPENSSL_LIBCRYPTO)

# Every static library a program links, a user's before what it uses (lld also finds a name in an
# archive given earlier, but the order states the layers).  libssl uses libcrypto.
WIN32_PROGRAM_ARCHIVES := $(WIN32_TREE_ARCHIVES) $(WIN32_COMPAT_ARCHIVES) $(WIN32_OPENSSL_ARCHIVES)

# A test's program (test-program): TEST_PROGRAM=<name> TEST_PROGRAM_SOURCES="<file>.c ...", linked
# with every static library into $(BROWSER_WIN32_BUILD)/tests/<name>.exe.
BROWSER_WIN32_TESTS := $(BROWSER_WIN32_BUILD)/tests
TEST_PROGRAM_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(TEST_PROGRAM_SOURCES))

# The compat headers (<compat/zlib/zlib.h> and the others), copied from zedBSD's C library side.
WIN32_COMPAT_HEADERS := $(wildcard include/libc/compat/*.h include/libc/compat/*/*.h)
WIN32_COMPAT_HEADER_COPIES := $(patsubst include/libc/%,$(BROWSER_WIN32_OBJ)/include/%,$(WIN32_COMPAT_HEADERS))

# The Vulkan headers and the loader's export list, from Khronos' tarballs of the SDK's tag (the version
# of the host's libvulkan-dev).  The SHA-256 values are of GitHub's archives of the tags.
VULKAN_SDK_TAG := vulkan-sdk-1.4.309.0
VULKAN_HEADERS_TARBALL := $(BROWSER_WIN32_SOURCES)/Vulkan-Headers-$(VULKAN_SDK_TAG).tar.gz
VULKAN_HEADERS_URL := https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/$(VULKAN_SDK_TAG).tar.gz
VULKAN_HEADERS_SHA256 := 2bc1b4127950badc80212abf1edfa5c3b5032f3425edf37255863ba7592c1969
VULKAN_LOADER_TARBALL := $(BROWSER_WIN32_SOURCES)/Vulkan-Loader-$(VULKAN_SDK_TAG).tar.gz
VULKAN_LOADER_URL := https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/$(VULKAN_SDK_TAG).tar.gz
VULKAN_LOADER_SHA256 := ddfeca84a868899fbb2c7f28b7c8fd1006e34b2b13ce653a63bddfb65cbc8d13
WIN32_VULKAN_HEADERS := $(BROWSER_WIN32_OBJ)/include/vulkan/vulkan.h
WIN32_VULKAN_LIBRARY := $(BROWSER_WIN32_OBJ)/libvulkan-1.a

.PHONY: zip icon program dist keiland-objects all layers libbrowser-core libbrowser-script script-core libraries vulkan headers openssl fetch flags clean test-program
all: layers libbrowser-core libraries vulkan $(BROWSER_WIN32_OBJ)/toolchain.txt program dist
program: $(OMNIGRAMMATON_PROGRAM) $(OMNIGRAMMATON_DEBUG)
dist: $(BROWSER_WIN32_DIST)/OmniGrammaton.exe $(OMNIGRAMMATON_SHARE_COPIES) $(OMNIGRAMMATON_LICENSE_COPIES) \
	$(BROWSER_WIN32_DIST)/share/certs/ca-certificates.pem
zip: $(OMNIGRAMMATON_ZIP)
icon: $(OMNIGRAMMATON_ICON)

layers: $(BROWSER_WIN32_LAYER_OBJECTS)
libbrowser-core: $(LIBBROWSER_CORE_OBJECTS)
libraries: $(WIN32_PROGRAM_ARCHIVES)
vulkan: $(WIN32_VULKAN_HEADERS) $(WIN32_VULKAN_LIBRARY)
openssl: $(WIN32_OPENSSL_ARCHIVES)
headers: $(WIN32_VULKAN_HEADERS) $(WIN32_COMPAT_HEADER_COPIES)
# libkeiland's objects but the stubs (what the stubs' generator reads).
keiland-objects: $(LIBKEILAND_WIN32_OBJECTS) $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(filter userland/desktop/libkeiland/%,$(BROWSER_WIN32_LAYER_SOURCES)))
libbrowser-script: $(LIBBROWSER_SCRIPT_ARCHIVE)
script-core: $(SCRIPT_CORE_PROGRAM)
fetch: $(VULKAN_HEADERS_TARBALL) $(VULKAN_LOADER_TARBALL)

test-program:
	@if [ -z '$(TEST_PROGRAM)' ] || [ -z '$(TEST_PROGRAM_SOURCES)' ]; then echo 'test-program needs TEST_PROGRAM=<name> TEST_PROGRAM_SOURCES="<file>.c ..."'; exit 1; fi
	@$(MAKE) --no-print-directory -f $(firstword $(MAKEFILE_LIST)) $(BROWSER_WIN32_TESTS)/$(TEST_PROGRAM).exe

$(BROWSER_WIN32_OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(WIN32_CC) $(WIN32_CFLAGS) -c $< -o $@

# libbrowser includes the compat headers (the images) and the Vulkan headers (the GPU renderer, and
# <browser/browser.h>, which names Vulkan's types).  Of the script core only js/tool.c includes
# <browser/browser.h>, and so does script-core.exe's main.
$(LIBBROWSER_CORE_OBJECTS) $(BROWSER_UI_OBJECTS) $(OMNIGRAMMATON_OBJECTS): | $(WIN32_COMPAT_HEADER_COPIES) $(WIN32_VULKAN_HEADERS)
# libmedia's Vulkan Video files include the Vulkan headers (and the others include nothing copied).
$(LIBMEDIA_CORE_OBJECTS): | $(WIN32_VULKAN_HEADERS)

# tls.c calls the OpenSSL linked into the program, which tls-static.c includes the headers of.
$(BROWSER_WIN32_OBJ)/$(LIBBROWSER_DIR)/net/tls.o: WIN32_CFLAGS += -DWB_TLS_STATIC
$(BROWSER_WIN32_OBJ)/$(LIBBROWSER_DIR)/net/tls-static.o: WIN32_CFLAGS += -I$(BROWSER_WIN32_OPENSSL)/include
$(BROWSER_WIN32_OBJ)/$(LIBBROWSER_DIR)/net/tls-static.o: | $(WIN32_OPENSSL_LIBSSL)

# OpenSSL, one run of the script for both libraries and the headers.  The toolchain is llvm-mingw
# (LLVM_MINGW=, else the script's default).
$(WIN32_OPENSSL_LIBSSL): plan/ws334/tests/openssl-windows.sh plan/ws334/tests/openssl-conf/10-mingw-arm64.conf
	@mkdir -p $(BROWSER_WIN32_OPENSSL)
	$(if $(strip $(LLVM_MINGW)),LLVM_MINGW='$(LLVM_MINGW)') sh plan/ws334/tests/openssl-windows.sh $(WIN32_ARCH) $(BROWSER_WIN32_OPENSSL)
$(WIN32_OPENSSL_LIBCRYPTO): $(WIN32_OPENSSL_LIBSSL)
	@test -f $@
# libkeiland's public headers name Vulkan's types, and its colour glyphs read PNG (the compat headers).
$(LIBKEILAND_WIN32_OBJECTS) $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(LIBKEILAND_WIN32_STUB_SOURCES)): | \
	$(WIN32_COMPAT_HEADER_COPIES) $(WIN32_VULKAN_HEADERS)
$(SCRIPT_CORE_OBJECTS): | $(WIN32_VULKAN_HEADERS)

# A compat object keeps what the compiler said in <object>.log, and shows it only when the compile fails.
$(WIN32_COMPAT_OBJECTS): $(BROWSER_WIN32_OBJ)/%.o: %.c | $(WIN32_COMPAT_HEADER_COPIES)
	@mkdir -p $(dir $@)
	$(WIN32_CC) $(WIN32_COMPAT_CFLAGS) -c $< -o $@ 2> $@.log || { cat $@.log; exit 1; }

# Only the compat headers: zedBSD's C library side also has Vulkan headers (include/libc/vulkan), which
# must not replace the Khronos headers this build copies under the same directory.
$(BROWSER_WIN32_OBJ)/include/compat/%.h: include/libc/compat/%.h
	@mkdir -p $(dir $@)
	cp $< $@

# lib<name>.a from its list, made afresh each time.  The members are appended (q), not replaced (r):
# libbrowser has files of the same name in different directories (page/input.c and html/input.c), and
# an archive keeps members by their file's name, so replacing would keep only the last of them.
define WIN32_STATIC_LIBRARY
$(BROWSER_WIN32_OBJ)/lib$(1).a: $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(WIN32_SOURCES_$(1)))
	@mkdir -p $$(dir $$@)
	rm -f $$@.new
	$(WIN32_AR) qcs $$@.new $$^
	mv -f $$@.new $$@
endef
$(foreach library,$(WIN32_TREE_LIBRARIES) $(WIN32_COMPAT_LIBRARIES) browser-script,$(eval $(call WIN32_STATIC_LIBRARY,$(library))))

# A program: <output>.exe from its objects and every static library, with the flags the subsystem
# needs (-mwindows for the browser, -mconsole for a test), so that another program is one line.
#   $(call WIN32_PROGRAM,<output>,<objects>,<subsystem flag>)
define WIN32_PROGRAM
$(1): $(2) $(WIN32_PROGRAM_ARCHIVES) $(WIN32_VULKAN_LIBRARY)
	@mkdir -p $$(dir $$@)
	$(WIN32_CC) $(3) -o $$@.new $(2) $(WIN32_PROGRAM_ARCHIVES) $(WIN32_PROGRAM_LDFLAGS)
	mv -f $$@.new $$@
endef
ifneq ($(strip $(TEST_PROGRAM)),)
$(eval $(call WIN32_PROGRAM,$(BROWSER_WIN32_TESTS)/$(TEST_PROGRAM).exe,$(TEST_PROGRAM_OBJECTS),-mconsole $(TEST_PROGRAM_LDFLAGS)))
$(TEST_PROGRAM_OBJECTS): | $(WIN32_COMPAT_HEADER_COPIES) $(WIN32_VULKAN_HEADERS)
endif

# OmniGrammaton.exe (wWinMain: -municode).
$(eval $(call WIN32_PROGRAM,$(OMNIGRAMMATON_FULL),$(OMNIGRAMMATON_OBJECTS),-mwindows -municode))

# The split: the debug information to its own file, the .exe without it and with the link to that file
# (the symbol table stays, so a stack still has names without the .dbg).
$(OMNIGRAMMATON_PROGRAM): $(OMNIGRAMMATON_FULL)
	@mkdir -p $(dir $@)
	$(WIN32_OBJCOPY) --only-keep-debug $< $(OMNIGRAMMATON_DEBUG)
	$(WIN32_OBJCOPY) --strip-debug --add-gnu-debuglink=$(OMNIGRAMMATON_DEBUG) $< $@.new
	mv -f $@.new $@
$(OMNIGRAMMATON_DEBUG): $(OMNIGRAMMATON_PROGRAM) ;

# The resources, compiled with the version's numbers and the icon.
$(BROWSER_WIN32_OBJ)/apps/browser-win32/resources/OmniGrammaton.res.o: apps/browser-win32/resources/OmniGrammaton.rc \
	apps/browser-win32/resources/OmniGrammaton.manifest apps/browser-win32/version.mk $(OMNIGRAMMATON_ICON)
	@mkdir -p $(dir $@)
	$(WIN32_WINDRES) -I apps/browser-win32/resources -I $(BROWSER_WIN32_OBJ) \
		-DOMNIGRAMMATON_VERSION_MAJOR=$(word 1,$(OMNIGRAMMATON_VERSION_WORDS)) \
		-DOMNIGRAMMATON_VERSION_MINOR=$(word 2,$(OMNIGRAMMATON_VERSION_WORDS)) \
		-DOMNIGRAMMATON_VERSION_PATCH=$(word 3,$(OMNIGRAMMATON_VERSION_WORDS)) \
		'-DOMNIGRAMMATON_VERSION_TEXT=\"$(OMNIGRAMMATON_VERSION)\"' -i $< -o $@

# The directory a user runs the program from.
$(BROWSER_WIN32_DIST)/OmniGrammaton.exe: $(OMNIGRAMMATON_PROGRAM)
	@mkdir -p $(dir $@)
	cp $< $@
define WIN32_SHARE_COPY
$(BROWSER_WIN32_DIST)/share/$(word 1,$(subst =, ,$(1))): $(word 2,$(subst =, ,$(1)))
	@mkdir -p $$(dir $$@)
	cp $$< $$@
endef
$(foreach pair,$(OMNIGRAMMATON_SHARE),$(eval $(call WIN32_SHARE_COPY,$(pair))))
define WIN32_LICENSE_COPY
$(BROWSER_WIN32_DIST)/share/licenses/$(word 1,$(subst =, ,$(1))): $(word 2,$(subst =, ,$(1)))
	@mkdir -p $$(dir $$@)
	cp $$< $$@
endef
$(foreach pair,$(OMNIGRAMMATON_LICENSES),$(eval $(call WIN32_LICENSE_COPY,$(pair))))
# OpenSSL's licence comes with its release, which its build extracts.
$(BROWSER_WIN32_OPENSSL)/work/src/openssl-$(OPENSSL_RELEASE)/LICENSE.txt: $(WIN32_OPENSSL_LIBSSL)
	@test -f $@ || { echo '$@: not in the OpenSSL release'; exit 1; }
$(BROWSER_WIN32_DIST)/share/certs/ca-certificates.pem: $(CA_CERTIFICATES_FILE)
	@mkdir -p $(dir $@)
	cp $< $@

# The roots' file: the shared downloads' copy when it is there (read only), else fetched; checked either way.
$(CA_CERTIFICATES_FILE):
	@mkdir -p $(dir $@)
	@if [ -f build/distfiles/$(CA_CERTIFICATES_ARCHIVE) ]; then cp build/distfiles/$(CA_CERTIFICATES_ARCHIVE) $@.part; \
	else curl -fsSL --retry 3 -o $@.part '$(CA_CERTIFICATES_URL)'; fi
	@echo '$(CA_CERTIFICATES_SHA256)  $@.part' | sha256sum -c --quiet - || { echo '$@: the SHA-256 is not $(CA_CERTIFICATES_SHA256)'; exit 1; }
	mv -f $@.part $@

# The icon's host tool and the icon.  The host tool includes the compat zlib header (the Windows build's copy).
$(MAKE_ICO): $(MAKE_ICO_SOURCES) | $(WIN32_COMPAT_HEADER_COPIES)
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -I. -I$(BROWSER_WIN32_OBJ)/include -Iuserland/base/libz-compat \
		-o $@ $(MAKE_ICO_SOURCES) -lm
$(OMNIGRAMMATON_ICON): $(MAKE_ICO)
	@mkdir -p $(dir $@)
	$(MAKE_ICO) $@.new
	mv -f $@.new $@

# The zip of the directory (made afresh whenever a file of it is newer).
$(OMNIGRAMMATON_ZIP): dist apps/browser-win32/tools/make-zip.py
	python3 -I apps/browser-win32/tools/make-zip.py $(BROWSER_WIN32_DIST) $@

# script-core.exe: the script core's library alone, with the system's libraries, no thread library (the
# core's Windows layer starts no thread) and no Vulkan.
$(SCRIPT_CORE_PROGRAM): $(SCRIPT_CORE_OBJECTS) $(LIBBROWSER_SCRIPT_ARCHIVE)
	@mkdir -p $(dir $@)
	$(WIN32_CC) -mconsole -o $@.new $(SCRIPT_CORE_OBJECTS) $(LIBBROWSER_SCRIPT_ARCHIVE) -static $(WIN32_SYSTEM_LDFLAGS)
	mv -f $@.new $@

# A tarball, fetched once and checked against its SHA-256.
define WIN32_FETCH
$(1):
	@mkdir -p $$(dir $$@)
	curl -fsSL --retry 3 -o $$@.part '$(2)'
	@echo '$(3)  $$@.part' | sha256sum -c --quiet - || { echo '$$@: the SHA-256 is not $(3)'; exit 1; }
	mv -f $$@.part $$@
endef
$(eval $(call WIN32_FETCH,$(VULKAN_HEADERS_TARBALL),$(VULKAN_HEADERS_URL),$(VULKAN_HEADERS_SHA256)))
$(eval $(call WIN32_FETCH,$(VULKAN_LOADER_TARBALL),$(VULKAN_LOADER_URL),$(VULKAN_LOADER_SHA256)))

# include/vulkan and include/vk_video of the headers' tarball.
$(WIN32_VULKAN_HEADERS): $(VULKAN_HEADERS_TARBALL)
	@mkdir -p $(BROWSER_WIN32_OBJ)/vulkan-headers $(BROWSER_WIN32_OBJ)/include
	tar -xzmf $< -C $(BROWSER_WIN32_OBJ)/vulkan-headers --strip-components=1 Vulkan-Headers-$(VULKAN_SDK_TAG)/include
	cp -R $(BROWSER_WIN32_OBJ)/vulkan-headers/include/vulkan $(BROWSER_WIN32_OBJ)/vulkan-headers/include/vk_video \
		$(BROWSER_WIN32_OBJ)/include/
	touch $@

# The loader's import library, from the export list in its tarball (loader/vulkan-1.def).
$(BROWSER_WIN32_OBJ)/vulkan-1.def: $(VULKAN_LOADER_TARBALL)
	@mkdir -p $(BROWSER_WIN32_OBJ)/vulkan-loader
	tar -xzmf $< -C $(BROWSER_WIN32_OBJ)/vulkan-loader --strip-components=2 Vulkan-Loader-$(VULKAN_SDK_TAG)/loader/vulkan-1.def
	cp $(BROWSER_WIN32_OBJ)/vulkan-loader/vulkan-1.def $@

$(WIN32_VULKAN_LIBRARY): $(BROWSER_WIN32_OBJ)/vulkan-1.def
	$(WIN32_DLLTOOL) -m $(WIN32_DLLTOOL_MACHINE) -d $< -D vulkan-1.dll -l $@

# What compiled this build, for the record of a test, and how many warnings libtruetype and the compat
# libraries have with it.
$(BROWSER_WIN32_OBJ)/toolchain.txt: $(WIN32_COMPAT_OBJECTS) FORCE
	@mkdir -p $(dir $@)
	@{ echo "arch: $(WIN32_ARCH)"; echo "compiler: $(WIN32_CC)"; $(WIN32_CC) --version | head -1; \
		echo "compat and libtruetype warnings: $$(cat $(addsuffix .log,$(WIN32_COMPAT_OBJECTS)) | grep -c ': warning: ')"; } > $@.new
	@if cmp -s $@.new $@; then rm -f $@.new; else mv $@.new $@; fi

# The flags a program or a test outside this file builds with, one NAME=value a line.
flags:
	@echo 'WIN32_CC=$(WIN32_CC)'
	@echo 'WIN32_TOOL_PREFIX=$(WIN32_TOOL_PREFIX)'
	@echo 'WIN32_CFLAGS=$(WIN32_CFLAGS)'
	@echo 'WIN32_THREAD_LDFLAGS=$(WIN32_THREAD_LDFLAGS)'
	@echo 'WIN32_VULKAN_LDFLAGS=$(WIN32_VULKAN_LDFLAGS)'
	@echo 'WIN32_PROGRAM_LDFLAGS=$(WIN32_PROGRAM_LDFLAGS)'
	@echo 'WIN32_PROGRAM_ARCHIVES=$(WIN32_PROGRAM_ARCHIVES)'
	@echo 'LIBBROWSER_CORE_OBJECTS=$(LIBBROWSER_CORE_OBJECTS)'
	@echo 'LIBKEILAND_WIN32_COMMON_SOURCES=$(LIBKEILAND_WIN32_COMMON_SOURCES)'
	@echo 'LIBKEILAND_WIN32_OBJECTS=$(LIBKEILAND_WIN32_OBJECTS) $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(filter userland/desktop/libkeiland/%,$(BROWSER_WIN32_LAYER_SOURCES)))'

# Removes this build's output only; refuses an output directory that is empty, the root or build itself.
clean:
	@case "$(BROWSER_WIN32_BUILD)" in ""|/|.|build|build/|./build) echo "refusing to clean '$(BROWSER_WIN32_BUILD)'"; exit 1;; esac
	rm -rf -- "$(BROWSER_WIN32_BUILD)"

.PHONY: FORCE
FORCE:

-include $(BROWSER_WIN32_LAYER_OBJECTS:.o=.d) $(LIBBROWSER_CORE_OBJECTS:.o=.d) $(LIBMEDIA_CORE_OBJECTS:.o=.d) \
	$(LIBKEILAND_WIN32_OBJECTS:.o=.d) $(BROWSER_UI_OBJECTS:.o=.d) $(OMNIGRAMMATON_OBJECTS:.o=.d) \
	$(WIN32_COMPAT_OBJECTS:.o=.d) \
	$(TEST_PROGRAM_OBJECTS:.o=.d) $(SCRIPT_CORE_OBJECTS:.o=.d)
