#
# Browser
#

WIN32_ARCH ?= x86_64
ifeq ($(WIN32_ARCH),x86_64)
WIN32_NAME := win64
else ifeq ($(WIN32_ARCH),aarch64)
WIN32_NAME := winarm64
else ifeq ($(WIN32_ARCH),i386)
WIN32_NAME := win32
else
$(error WIN32_ARCH must be x86_64, aarch64, or i386, not '$(WIN32_ARCH)')
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
ifneq ($(shell command -v x86_64-w64-mingw32-gcc 2>/dev/null),)
WIN32_CC := x86_64-w64-mingw32-gcc
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

WIN32_COMMON_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -MMD -MP \
	-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE \
	-I. -Iuserland/desktop/include -Iuserland/desktop/libbrowser -Iuserland/desktop/libmedia \
	-I$(BROWSER_WIN32_OBJ)/include
WIN32_CFLAGS := $(WIN32_COMMON_CFLAGS) -Wno-unknown-warning-option -Wno-maybe-uninitialized -Werror
WIN32_COMPAT_CFLAGS := $(WIN32_COMMON_CFLAGS)

WIN32_VULKAN_LDFLAGS := -L$(BROWSER_WIN32_OBJ) -lvulkan-1
WIN32_SYSTEM_LDFLAGS := -lws2_32 -liphlpapi -lbcrypt -lcrypt32 -ldwmapi -lshcore -limm32 -lshell32 -lole32 -luuid \
	-lgdi32 -luser32 -ladvapi32
WIN32_PROGRAM_LDFLAGS := -static $(WIN32_VULKAN_LDFLAGS) $(WIN32_SYSTEM_LDFLAGS)

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

include userland/desktop/libbrowser/sources.mk
LIBBROWSER_CORE_EXCLUDED := $(LIBBROWSER_OS_SOURCES) \
	$(LIBBROWSER_OS_NET_SOURCES)
LIBBROWSER_CORE_SOURCES := $(filter-out $(LIBBROWSER_CORE_EXCLUDED),$(LIBBROWSER_SOURCES))
LIBBROWSER_CORE_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(LIBBROWSER_CORE_SOURCES))

LIBBROWSER_SCRIPT_WIN32_LAYER := $(LIBBROWSER_DIR)/base/os-win32.c \
	$(LIBBROWSER_DIR)/base/os-system-win32.c \
	$(LIBBROWSER_DIR)/base/os-file-win32.c \
	$(LIBBROWSER_DIR)/base/os-time-win32.c \
	$(LIBBROWSER_DIR)/base/os-zones.c
WIN32_SOURCES_browser-script := $(filter-out $(LIBBROWSER_OS_SOURCES),$(LIBBROWSER_SCRIPT_SOURCES)) \
	$(LIBBROWSER_SCRIPT_WIN32_LAYER)
LIBBROWSER_SCRIPT_ARCHIVE := $(BROWSER_WIN32_OBJ)/libbrowser-script.a
SCRIPT_CORE_SOURCES := apps/browser-win32/tools/script-core-main.c
SCRIPT_CORE_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(SCRIPT_CORE_SOURCES))
SCRIPT_CORE_PROGRAM := $(BROWSER_WIN32_BUILD)/tests/script-core.exe

include userland/desktop/libmedia/sources.mk
LIBMEDIA_CORE_SOURCES := $(filter-out $(LIBMEDIA_OS_SOURCES),$(LIBMEDIA_SOURCES))
LIBMEDIA_CORE_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(LIBMEDIA_CORE_SOURCES))

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
	userland/desktop/libkeiland/win32/clipboard.c \
	userland/desktop/libkeiland/win32/pointer.c \
	userland/desktop/libkeiland/win32/drag.c

LIBKEILAND_WIN32_CRT_SOURCES := userland/desktop/libkeiland/ui/text.c
LIBKEILAND_WIN32_STUB_SOURCES := userland/desktop/libkeiland/win32/unsupported.c
LIBKEILAND_WIN32_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(LIBKEILAND_WIN32_COMMON_SOURCES) \
	$(LIBKEILAND_WIN32_CRT_SOURCES) $(LIBKEILAND_WIN32_OWN_SOURCES))

include userland/desktop/browser/sources.mk

BROWSER_UI_SOURCES := $(filter-out $(KEILAND_BROWSER_DIR)/main-posix.c,$(KEILAND_BROWSER_SOURCES)) \
	apps/browser-win32/protect.c
BROWSER_UI_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(BROWSER_UI_SOURCES))
WIN32_SOURCES_browser-ui := $(BROWSER_UI_SOURCES)
$(BROWSER_UI_OBJECTS): WIN32_CFLAGS += -I$(KEILAND_BROWSER_DIR) -DSHELL_APPLICATION_ID=\"OmniGrammaton\"

include apps/browser-win32/version.mk
OMNIGRAMMATON_SOURCES := apps/browser-win32/main.c apps/browser-win32/single.c
OMNIGRAMMATON_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(OMNIGRAMMATON_SOURCES)) \
	$(BROWSER_WIN32_OBJ)/apps/browser-win32/resources/OmniGrammaton.res.o
OMNIGRAMMATON_PROGRAM := $(BROWSER_WIN32_BUILD)/OmniGrammaton.exe

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

HOST_CC ?= cc
OMNIGRAMMATON_ICON := $(BROWSER_WIN32_OBJ)/OmniGrammaton.ico
MAKE_ICO := $(BROWSER_WIN32_OBJ)/host/make-ico
MAKE_ICO_SOURCES = apps/browser-win32/tools/make-ico.c apps/browser-win32/tools/ico.c \
	userland/desktop/wayland/icons.c userland/desktop/picture/png-write.c $(LIBZ_COMPAT_SOURCES)

CA_CERTIFICATES_PACKAGE := userland/packages/security/ca-certificates/Makefile
CA_CERTIFICATES_ARCHIVE := $(shell sed -n 's/^EXT_ca-certificates_ARCHIVE := //p' $(CA_CERTIFICATES_PACKAGE))
CA_CERTIFICATES_URL := $(shell sed -n 's/^EXT_ca-certificates_URL := //p' $(CA_CERTIFICATES_PACKAGE))
CA_CERTIFICATES_SHA256 := $(shell sed -n 's/^EXT_ca-certificates_SHA256 := //p' $(CA_CERTIFICATES_PACKAGE))
CA_CERTIFICATES_FILE := $(BROWSER_WIN32_SOURCES)/$(CA_CERTIFICATES_ARCHIVE)

WIN32_TOOLCHAIN_ROOT := $(abspath $(dir $(shell command -v $(WIN32_CC) 2>/dev/null || echo $(WIN32_CC)))..)
WIN32_MINGW_LICENSES := $(WIN32_TOOLCHAIN_ROOT)/$(WIN32_TRIPLE)/share/mingw32
OMNIGRAMMATON_LICENSES = \
	README.txt=apps/browser-win32/resources/licenses-README.txt \
	zedBSD-LICENSE.txt=LICENSE \
	Mahora-LICENSE.txt=userland/desktop/fonts/Mahora-LICENSE.txt \
	JetBrainsMono-OFL.txt=userland/desktop/fonts/JetBrainsMono-OFL.txt \
	DroidSansFallback-LICENSE.txt=userland/desktop/fonts/DroidSansFallback-LICENSE.txt \
	WHATWG-entities.txt=userland/base/licenses/browser/WHATWG-entities \
	Unicode-License-V3.txt=userland/base/licenses/browser/Unicode-License-V3 \
	ca-certificates-MPL-2.0.txt=userland/packages/security/ca-certificates/MPL-2.0.txt
OMNIGRAMMATON_LICENSE_COPIES := $(foreach pair,$(OMNIGRAMMATON_LICENSES),$(BROWSER_WIN32_DIST)/share/licenses/$(word 1,$(subst =, ,$(pair))))

OMNIGRAMMATON_ZIP := $(BROWSER_WIN32_BUILD)/OmniGrammaton-$(OMNIGRAMMATON_VERSION)-$(WIN32_NAME).zip

WIN32_SOURCES_browser := $(LIBBROWSER_CORE_SOURCES) \
	$(filter-out $(LIBBROWSER_CORE_SOURCES),$(filter userland/desktop/libbrowser/%,$(BROWSER_WIN32_LAYER_SOURCES)))
WIN32_SOURCES_media := $(LIBMEDIA_CORE_SOURCES) $(filter userland/desktop/libmedia/%,$(BROWSER_WIN32_LAYER_SOURCES))
WIN32_SOURCES_keiland := $(LIBKEILAND_WIN32_COMMON_SOURCES) $(filter userland/desktop/libkeiland/%,$(BROWSER_WIN32_LAYER_SOURCES)) \
	$(LIBKEILAND_WIN32_CRT_SOURCES) $(LIBKEILAND_WIN32_OWN_SOURCES) $(LIBKEILAND_WIN32_STUB_SOURCES)
WIN32_TREE_LIBRARIES := browser-ui browser media keiland

include userland/desktop/libtruetype/sources.mk
include userland/desktop/libpng-compat/sources.mk
include userland/desktop/libjpeg-compat/sources.mk
include userland/desktop/libgif-compat/sources.mk
include userland/desktop/libwebp-compat/sources.mk
include userland/base/libz-compat/sources.mk
WIN32_COMPAT_LIBRARIES := truetype png-compat jpeg-compat gif-compat webp-compat z-compat
WIN32_SOURCES_truetype := $(LIBTRUETYPE_SOURCES)
WIN32_SOURCES_png-compat := $(LIBPNG_COMPAT_SOURCES)
WIN32_SOURCES_jpeg-compat := $(LIBJPEG_COMPAT_SOURCES)
WIN32_SOURCES_gif-compat := $(LIBGIF_COMPAT_SOURCES)
WIN32_SOURCES_webp-compat := $(LIBWEBP_COMPAT_SOURCES)
WIN32_SOURCES_z-compat := $(LIBZ_COMPAT_SOURCES)
WIN32_COMPAT_SOURCES := $(foreach library,$(WIN32_COMPAT_LIBRARIES),$(WIN32_SOURCES_$(library)))
WIN32_COMPAT_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(WIN32_COMPAT_SOURCES))
WIN32_COMPAT_ARCHIVES := $(foreach library,$(WIN32_COMPAT_LIBRARIES),$(BROWSER_WIN32_OBJ)/lib$(library).a)
WIN32_TREE_ARCHIVES := $(foreach library,$(WIN32_TREE_LIBRARIES),$(BROWSER_WIN32_OBJ)/lib$(library).a)

include userland/base/libcrypto-compat/sources.mk
include userland/base/libssl-compat/sources.mk
WIN32_TLS_LIBRARIES := ssl-compat crypto-compat
WIN32_SOURCES_ssl-compat := $(LIBSSL_COMPAT_SOURCES)
WIN32_SOURCES_crypto-compat := $(LIBCRYPTO_COMPAT_SOURCES) $(LIBCRYPTO_COMPAT_WIN32_SOURCES)
WIN32_TLS_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(WIN32_SOURCES_ssl-compat) $(WIN32_SOURCES_crypto-compat))
WIN32_TLS_ARCHIVES := $(foreach library,$(WIN32_TLS_LIBRARIES),$(BROWSER_WIN32_OBJ)/lib$(library).a)

WIN32_PROGRAM_ARCHIVES := $(WIN32_TREE_ARCHIVES) $(WIN32_COMPAT_ARCHIVES) $(WIN32_TLS_ARCHIVES)

BROWSER_WIN32_TESTS := $(BROWSER_WIN32_BUILD)/tests
TEST_PROGRAM_OBJECTS := $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(TEST_PROGRAM_SOURCES))

# The compat headers (<compat/zlib/zlib.h> and the others), copied from zedBSD's C library side.
WIN32_COMPAT_HEADERS := $(wildcard include/libc/compat/*.h include/libc/compat/*/*.h)
WIN32_COMPAT_HEADER_COPIES := $(patsubst include/libc/%,$(BROWSER_WIN32_OBJ)/include/%,$(WIN32_COMPAT_HEADERS))

VULKAN_SDK_TAG := vulkan-sdk-1.4.309.0
VULKAN_HEADERS_TARBALL := $(BROWSER_WIN32_SOURCES)/Vulkan-Headers-$(VULKAN_SDK_TAG).tar.gz
VULKAN_HEADERS_URL := https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/$(VULKAN_SDK_TAG).tar.gz
VULKAN_HEADERS_SHA256 := 2bc1b4127950badc80212abf1edfa5c3b5032f3425edf37255863ba7592c1969
VULKAN_LOADER_TARBALL := $(BROWSER_WIN32_SOURCES)/Vulkan-Loader-$(VULKAN_SDK_TAG).tar.gz
VULKAN_LOADER_URL := https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/$(VULKAN_SDK_TAG).tar.gz
VULKAN_LOADER_SHA256 := ddfeca84a868899fbb2c7f28b7c8fd1006e34b2b13ce653a63bddfb65cbc8d13
WIN32_VULKAN_HEADERS := $(BROWSER_WIN32_OBJ)/include/vulkan/vulkan.h
WIN32_VULKAN_LIBRARY := $(BROWSER_WIN32_OBJ)/libvulkan-1.a

.PHONY: zip icon program dist keiland-objects all layers libbrowser-core libbrowser-script script-core libraries vulkan headers tls fetch flags clean test-program
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
tls: $(WIN32_TLS_ARCHIVES)
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

$(LIBBROWSER_CORE_OBJECTS) $(BROWSER_UI_OBJECTS) $(OMNIGRAMMATON_OBJECTS): | $(WIN32_COMPAT_HEADER_COPIES) $(WIN32_VULKAN_HEADERS)
$(LIBMEDIA_CORE_OBJECTS): | $(WIN32_VULKAN_HEADERS)

$(WIN32_TLS_OBJECTS): | $(WIN32_COMPAT_HEADER_COPIES)

$(LIBKEILAND_WIN32_OBJECTS) $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(LIBKEILAND_WIN32_STUB_SOURCES)): | \
	$(WIN32_COMPAT_HEADER_COPIES) $(WIN32_VULKAN_HEADERS)
$(SCRIPT_CORE_OBJECTS): | $(WIN32_VULKAN_HEADERS)

$(WIN32_COMPAT_OBJECTS): $(BROWSER_WIN32_OBJ)/%.o: %.c | $(WIN32_COMPAT_HEADER_COPIES)
	@mkdir -p $(dir $@)
	$(WIN32_CC) $(WIN32_COMPAT_CFLAGS) -c $< -o $@ 2> $@.log || { cat $@.log; exit 1; }

$(BROWSER_WIN32_OBJ)/include/compat/%.h: include/libc/compat/%.h
	@mkdir -p $(dir $@)
	cp $< $@

define WIN32_STATIC_LIBRARY
$(BROWSER_WIN32_OBJ)/lib$(1).a: $(patsubst %.c,$(BROWSER_WIN32_OBJ)/%.o,$(WIN32_SOURCES_$(1)))
	@mkdir -p $$(dir $$@)
	rm -f $$@.new
	$(WIN32_AR) qcs $$@.new $$^
	mv -f $$@.new $$@
endef
$(foreach library,$(WIN32_TREE_LIBRARIES) $(WIN32_COMPAT_LIBRARIES) $(WIN32_TLS_LIBRARIES) browser-script,$(eval $(call WIN32_STATIC_LIBRARY,$(library))))

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
	$(WIN32_COMPAT_OBJECTS:.o=.d) $(WIN32_TLS_OBJECTS:.o=.d) \
	$(TEST_PROGRAM_OBJECTS:.o=.d) $(SCRIPT_CORE_OBJECTS:.o=.d)
