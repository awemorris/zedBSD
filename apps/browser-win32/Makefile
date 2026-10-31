# OmniGrammaton for Windows: the cross build of libbrowser, libmedia, libkeiland and the browser's user
# interface into Windows programs.  Run from the top of the tree:
#   make -f apps/browser-win32/Makefile WIN32_ARCH=x86_64    (or aarch64)
# or through the top Makefile: make browser-win64, make browser-winarm64.
# Output under $(BROWSER_WIN32_BUILD) (build/browser-win64 or build/browser-winarm64 by default).
#
# This is the build's skeleton: it finds the toolchain, writes what it found, and compiles the files
# of the operating-system layers (libbrowser's base/os-*, its storage, libmedia's media-os), so their
# skeletons and implementations are built with the Windows compiler and its warnings.  The libraries,
# DLLs, programs and resources join it later.
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

# The flags of every file this build compiles.  WS334's own files are held to warning 0.
WIN32_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Werror \
	-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE \
	-I. -Iuserland/desktop/include -Iuserland/desktop/libbrowser -Iuserland/desktop/libmedia

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
	userland/desktop/libmedia/media-os-win32.c

BROWSER_WIN32_OBJ := $(BROWSER_WIN32_BUILD)/obj
BROWSER_WIN32_LAYER_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(BROWSER_WIN32_LAYER_SOURCES))

.PHONY: all layers clean
all: layers $(BROWSER_WIN32_OBJ)/toolchain.txt

layers: $(BROWSER_WIN32_LAYER_OBJECTS)

$(BROWSER_WIN32_OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(WIN32_CC) $(WIN32_CFLAGS) -c $< -o $@

# What compiled this build, for the record of a test.
$(BROWSER_WIN32_OBJ)/toolchain.txt: FORCE
	@mkdir -p $(dir $@)
	@{ echo "arch: $(WIN32_ARCH)"; echo "compiler: $(WIN32_CC)"; $(WIN32_CC) --version | head -1; } > $@.new
	@if cmp -s $@.new $@; then rm -f $@.new; else mv $@.new $@; fi

# Removes this build's output only; refuses an output directory that is empty, the root or build itself.
clean:
	@case "$(BROWSER_WIN32_BUILD)" in ""|/|.|build|build/|./build) echo "refusing to clean '$(BROWSER_WIN32_BUILD)'"; exit 1;; esac
	rm -rf -- "$(BROWSER_WIN32_BUILD)"

.PHONY: FORCE
FORCE:
