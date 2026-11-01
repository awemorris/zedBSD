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
# the static libraries: libbrowser.a (libbrowser without its network and storage, with its Windows
# layer), libmedia.a and libkeiland.a (their Windows files so far), libtruetype and the png, jpeg, gif
# and z compat libraries (from the same source lists as the zedBSD build); the Vulkan headers and the
# import library of the Vulkan loader.  A test's program links with them all (test-program); the
# browser's program and its resources join later, linked the same way (WIN32_PROGRAM).
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
WIN32_SYSTEM_LDFLAGS := -lws2_32 -lbcrypt -lcrypt32 -lshell32 -lole32
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

# libbrowser without the cookies and the storage (they come with the instance later): the zedBSD
# build's list less its POSIX operating-system layer, which the Windows layer above replaces.
include userland/desktop/libbrowser/sources.mk
LIBBROWSER_CORE_EXCLUDED := $(LIBBROWSER_OS_SOURCES) \
	$(LIBBROWSER_OS_NET_SOURCES) \
	$(LIBBROWSER_DIR)/net/cookie.c \
	$(LIBBROWSER_DIR)/page/storage.c
LIBBROWSER_CORE_SOURCES := $(filter-out $(LIBBROWSER_CORE_EXCLUDED),$(LIBBROWSER_SOURCES))
LIBBROWSER_CORE_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(LIBBROWSER_CORE_SOURCES))

# The static libraries of the tree's libraries, each from its files of the Windows build so far:
# libbrowser's core with its Windows layer and its storage modules, libmedia's and libkeiland's Windows
# files (their shared files join with their Windows ports).
WIN32_SOURCES_browser := $(LIBBROWSER_CORE_SOURCES) $(filter userland/desktop/libbrowser/%,$(BROWSER_WIN32_LAYER_SOURCES))
WIN32_SOURCES_media := $(filter userland/desktop/libmedia/%,$(BROWSER_WIN32_LAYER_SOURCES))
WIN32_SOURCES_keiland := $(filter userland/desktop/libkeiland/%,$(BROWSER_WIN32_LAYER_SOURCES))
WIN32_TREE_LIBRARIES := browser media keiland

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

# Every static library a program links, a user's before what it uses (lld also finds a name in an
# archive given earlier, but the order states the layers).
WIN32_PROGRAM_ARCHIVES := $(WIN32_TREE_ARCHIVES) $(WIN32_COMPAT_ARCHIVES)

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

.PHONY: all layers libbrowser-core libraries vulkan fetch flags clean test-program
all: layers libbrowser-core libraries vulkan $(BROWSER_WIN32_OBJ)/toolchain.txt

layers: $(BROWSER_WIN32_LAYER_OBJECTS)
libbrowser-core: $(LIBBROWSER_CORE_OBJECTS)
libraries: $(WIN32_PROGRAM_ARCHIVES)
vulkan: $(WIN32_VULKAN_HEADERS) $(WIN32_VULKAN_LIBRARY)
fetch: $(VULKAN_HEADERS_TARBALL) $(VULKAN_LOADER_TARBALL)

test-program:
	@if [ -z '$(TEST_PROGRAM)' ] || [ -z '$(TEST_PROGRAM_SOURCES)' ]; then echo 'test-program needs TEST_PROGRAM=<name> TEST_PROGRAM_SOURCES="<file>.c ..."'; exit 1; fi
	@$(MAKE) --no-print-directory -f $(firstword $(MAKEFILE_LIST)) $(BROWSER_WIN32_TESTS)/$(TEST_PROGRAM).exe

$(BROWSER_WIN32_OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(WIN32_CC) $(WIN32_CFLAGS) -c $< -o $@

# libbrowser includes the compat headers (the images) and the Vulkan headers (the GPU renderer).
$(LIBBROWSER_CORE_OBJECTS): | $(WIN32_COMPAT_HEADER_COPIES) $(WIN32_VULKAN_HEADERS)

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
$(foreach library,$(WIN32_TREE_LIBRARIES) $(WIN32_COMPAT_LIBRARIES),$(eval $(call WIN32_STATIC_LIBRARY,$(library))))

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
$(eval $(call WIN32_PROGRAM,$(BROWSER_WIN32_TESTS)/$(TEST_PROGRAM).exe,$(TEST_PROGRAM_OBJECTS),-mconsole))
$(TEST_PROGRAM_OBJECTS): | $(WIN32_COMPAT_HEADER_COPIES) $(WIN32_VULKAN_HEADERS)
endif

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

# Removes this build's output only; refuses an output directory that is empty, the root or build itself.
clean:
	@case "$(BROWSER_WIN32_BUILD)" in ""|/|.|build|build/|./build) echo "refusing to clean '$(BROWSER_WIN32_BUILD)'"; exit 1;; esac
	rm -rf -- "$(BROWSER_WIN32_BUILD)"

.PHONY: FORCE
FORCE:

-include $(BROWSER_WIN32_LAYER_OBJECTS:.o=.d) $(LIBBROWSER_CORE_OBJECTS:.o=.d) $(WIN32_COMPAT_OBJECTS:.o=.d) \
	$(TEST_PROGRAM_OBJECTS:.o=.d)
