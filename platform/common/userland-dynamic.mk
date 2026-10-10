# Shared portable userland link rules for the LP64 targets.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The platform provides DYNAMIC_SYSROOT, DYNAMIC_LINK_CFLAGS,
# DYNAMIC_LINK_LDFLAGS and DYNAMIC_ELF_MACHINE. Source and link dependencies
# stay identical on amd64 and arm64; libc, rtld and assembly stay platform-owned.
DYNAMIC_PORTABLE_PROGRAMS := vkdemo vkvideo-probe media-probe display-events wayland wltest acquire-fence-test gpu-forge-test menu-probe keiland-settings keiland-system printtest keiland-notify mediastorage fidoctl passkey-fido2 titlebar-probe popup-probe subsurface-probe seat-probe tablet-probe data-probe extras-probe wlshm keiland-ime ime-probe mview terminal files settings monitor notes pdfviewer imageview videoplayer music photos phone calendar mailer textedit kuidemo browser browser-probe glxtest zgears egltest glescompute xserver gpu-fence-test gpu-share-test

# The utility library.  It holds what is not part of the C library and not
# wanted by every program, and is built from the same tree so that the two
# cannot drift apart.
DYNAMIC_LIBUTIL_OBJS := $(DYNAMIC_DIR)/obj/src/libc/libutil.o

$(DYNAMIC_DIR)/libutil.so: $(DYNAMIC_LIBUTIL_OBJS) $(DYNAMIC_DIR)/libc.so \
	tools/build/check-dynamic-elf.py
	$(LD) $(DYNAMIC_LINK_LDFLAGS) -shared -soname libutil.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 $(DYNAMIC_LIBUTIL_OBJS) -L$(DYNAMIC_DIR) -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine $(DYNAMIC_ELF_MACHINE) \
 --role shared-library --needed libc.so --soname libutil.so $@

# Wayland client transport is a normal shared dependency of the Vulkan WSI.
DYNAMIC_WAYLAND_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libwayland-client)

$(DYNAMIC_DIR)/libwayland-client.so: $(DYNAMIC_WAYLAND_OBJS) $(DYNAMIC_DIR)/libc.so \
	userland/desktop/libwayland/exports.map tools/build/check-dynamic-elf.py
	$(LD) $(DYNAMIC_LINK_LDFLAGS) -shared -soname libwayland-client.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libwayland/exports.map \
 $(DYNAMIC_WAYLAND_OBJS) -L$(DYNAMIC_DIR) -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine $(DYNAMIC_ELF_MACHINE) --role shared-library \
 --needed libc.so --soname libwayland-client.so $@

# The TrueType reader draws glyphs for whoever puts text on a display; it
# needs nothing but the C library and the mathematics in it.
DYNAMIC_TRUETYPE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libtruetype)

$(DYNAMIC_DIR)/libtruetype.so: $(DYNAMIC_TRUETYPE_OBJS) $(DYNAMIC_DIR)/libc.so \
	userland/desktop/libtruetype/exports.map tools/build/check-dynamic-elf.py
	$(LD) $(DYNAMIC_LINK_LDFLAGS) -shared -soname libtruetype.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libtruetype/exports.map \
 $(DYNAMIC_TRUETYPE_OBJS) -L$(DYNAMIC_DIR) -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine $(DYNAMIC_ELF_MACHINE) --role shared-library \
 --needed libc.so --soname libtruetype.so $@

# libz-compat (ws071-p010): the zlib interface of the base programs; it needs nothing but the C library.
DYNAMIC_Z_COMPAT_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libz-compat)

$(DYNAMIC_DIR)/libz-compat.so: $(DYNAMIC_Z_COMPAT_OBJS) $(DYNAMIC_DIR)/libc.so \
	userland/base/libz-compat/exports.map tools/build/check-dynamic-elf.py
	$(LD) $(DYNAMIC_LINK_LDFLAGS) -shared -soname libz-compat.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/base/libz-compat/exports.map \
 $(DYNAMIC_Z_COMPAT_OBJS) -L$(DYNAMIC_DIR) -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine $(DYNAMIC_ELF_MACHINE) --role shared-library \
 --needed libc.so --soname libz-compat.so $@

# libpng-compat (ws071-p010): libpng's simplified API of the base programs, over libz-compat.
DYNAMIC_PNG_COMPAT_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libpng-compat)

$(DYNAMIC_DIR)/libpng-compat.so: $(DYNAMIC_PNG_COMPAT_OBJS) $(DYNAMIC_DIR)/libz-compat.so $(DYNAMIC_DIR)/libc.so \
	userland/base/libpng-compat/exports.map tools/build/check-dynamic-elf.py
	$(LD) $(DYNAMIC_LINK_LDFLAGS) -shared -soname libpng-compat.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/base/libpng-compat/exports.map \
 $(DYNAMIC_PNG_COMPAT_OBJS) -L$(DYNAMIC_DIR) -l:libz-compat.so -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine $(DYNAMIC_ELF_MACHINE) --role shared-library \
 --needed libz-compat.so --needed libc.so --soname libpng-compat.so $@

# libjpeg-compat (ws074-p019): the libjpeg decompression interface of the base programs; it needs nothing
# but the C library.
DYNAMIC_JPEG_COMPAT_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libjpeg-compat)

$(DYNAMIC_DIR)/libjpeg-compat.so: $(DYNAMIC_JPEG_COMPAT_OBJS) $(DYNAMIC_DIR)/libc.so \
	userland/base/libjpeg-compat/exports.map tools/build/check-dynamic-elf.py
	$(LD) $(DYNAMIC_LINK_LDFLAGS) -shared -soname libjpeg-compat.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/base/libjpeg-compat/exports.map \
 $(DYNAMIC_JPEG_COMPAT_OBJS) -L$(DYNAMIC_DIR) -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine $(DYNAMIC_ELF_MACHINE) --role shared-library \
 --needed libc.so --soname libjpeg-compat.so $@

# libpdf (ws079-p004): the PDF library of the base programs; its reader decodes Flate streams through
# libz-compat and JPEG images through libjpeg-compat (ws079-p006), and draws the glyphs of text through
# libtruetype (ws079-p007).
DYNAMIC_PDF_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libpdf)

$(DYNAMIC_DIR)/libpdf.so: $(DYNAMIC_PDF_OBJS) $(DYNAMIC_DIR)/libz-compat.so $(DYNAMIC_DIR)/libjpeg-compat.so \
	$(DYNAMIC_DIR)/libtruetype.so $(DYNAMIC_DIR)/libc.so \
	userland/base/libpdf/exports.map tools/build/check-dynamic-elf.py
	$(LD) $(DYNAMIC_LINK_LDFLAGS) -shared -soname libpdf.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/base/libpdf/exports.map \
 $(DYNAMIC_PDF_OBJS) -L$(DYNAMIC_DIR) -l:libz-compat.so -l:libjpeg-compat.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine $(DYNAMIC_ELF_MACHINE) --role shared-library \
 --needed libz-compat.so --needed libjpeg-compat.so --needed libtruetype.so --needed libc.so --soname libpdf.so $@

# libgif-compat (ws074-p051): giflib's decoding interface of the base programs; it needs nothing but the
# C library.
DYNAMIC_GIF_COMPAT_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libgif-compat)

$(DYNAMIC_DIR)/libgif-compat.so: $(DYNAMIC_GIF_COMPAT_OBJS) $(DYNAMIC_DIR)/libc.so \
	userland/base/libgif-compat/exports.map tools/build/check-dynamic-elf.py
	$(LD) $(DYNAMIC_LINK_LDFLAGS) -shared -soname libgif-compat.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/base/libgif-compat/exports.map \
 $(DYNAMIC_GIF_COMPAT_OBJS) -L$(DYNAMIC_DIR) -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine $(DYNAMIC_ELF_MACHINE) --role shared-library \
 --needed libc.so --soname libgif-compat.so $@

# The Wayland EGL window (WS068 p002); it needs nothing but the C library.
DYNAMIC_WAYLAND_EGL_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libwayland-egl)

$(DYNAMIC_DIR)/libwayland-egl.so: $(DYNAMIC_WAYLAND_EGL_OBJS) $(DYNAMIC_DIR)/libc.so \
	userland/desktop/libwayland-egl/exports.map tools/build/check-dynamic-elf.py
	$(LD) $(DYNAMIC_LINK_LDFLAGS) -shared -soname libwayland-egl.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libwayland-egl/exports.map \
 $(DYNAMIC_WAYLAND_EGL_OBJS) -L$(DYNAMIC_DIR) -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine $(DYNAMIC_ELF_MACHINE) --role shared-library \
 --needed libc.so --soname libwayland-egl.so $@

# EGL over Vulkan (WS068 p002): Wayland and display-direct windows.
DYNAMIC_EGL_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libegl)

$(DYNAMIC_DIR)/libEGL.so: $(DYNAMIC_EGL_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so userland/desktop/libegl/exports.map tools/build/check-dynamic-elf.py
	$(LD) $(DYNAMIC_LINK_LDFLAGS) -shared -soname libEGL.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libegl/exports.map \
 $(DYNAMIC_EGL_OBJS) -L$(DYNAMIC_DIR) -l:libvulkan.so -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine $(DYNAMIC_ELF_MACHINE) --role shared-library \
 --needed libvulkan.so --needed libwayland-client.so --needed libc.so --soname libEGL.so $@

# OpenGL ES over EGL and Vulkan (WS068 p002, p008); it finds its context and frame through libEGL.
DYNAMIC_GLESV2_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libglesv2)

$(DYNAMIC_DIR)/libGLESv2.so: $(DYNAMIC_GLESV2_OBJS) $(DYNAMIC_DIR)/libEGL.so $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libc.so \
	userland/desktop/libglesv2/exports.map tools/build/check-dynamic-elf.py
	$(LD) $(DYNAMIC_LINK_LDFLAGS) -shared -soname libGLESv2.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libglesv2/exports.map \
 $(DYNAMIC_GLESV2_OBJS) -L$(DYNAMIC_DIR) -l:libEGL.so -l:libvulkan.so -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine $(DYNAMIC_ELF_MACHINE) --role shared-library \
 --needed libEGL.so --needed libvulkan.so --needed libc.so --soname libGLESv2.so $@

# The desktop's way into the system and the compositor's Wayland extensions (the
# System Menu, WS070): the C library and the Wayland client; the file
# chooser (ws092-p003) draws its text with libtruetype.
DYNAMIC_LIBKEILAND_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libkeiland)

$(DYNAMIC_DIR)/libkeiland.so: $(DYNAMIC_LIBKEILAND_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libpng-compat.so $(DYNAMIC_DIR)/libz-compat.so \
	$(DYNAMIC_DIR)/libc.so userland/desktop/libkeiland/exports.map tools/build/check-dynamic-elf.py
	$(PYTHON) userland/desktop/libkeiland/exports.py --check
	$(LD) $(DYNAMIC_LINK_LDFLAGS) -shared -soname libkeiland.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libkeiland/exports.map \
 $(DYNAMIC_LIBKEILAND_OBJS) -L$(DYNAMIC_DIR) -l:libwayland-client.so -l:libtruetype.so -l:libvulkan.so \
 -l:libpng-compat.so -l:libz-compat.so -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine $(DYNAMIC_ELF_MACHINE) --role shared-library \
 --needed libwayland-client.so --needed libtruetype.so --needed libvulkan.so --needed libpng-compat.so \
 --needed libz-compat.so --needed libc.so --soname libkeiland.so $@

# Vulkan is an ordinary shared dependency of the portable base application.
DYNAMIC_VULKAN_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libvulkan)
DYNAMIC_VKDEMO_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,vkdemo)
DYNAMIC_VULKAN_CHECK := tools/build/check-dynamic-elf.py

$(DYNAMIC_DIR)/libvulkan.so: $(DYNAMIC_VULKAN_OBJS) $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/libwayland-client.so \
	userland/desktop/libvulkan/exports.map userland/desktop/libvulkan/api-commands.tsv \
	$(DYNAMIC_VULKAN_CHECK)
	$(LD) $(DYNAMIC_LINK_LDFLAGS) -shared -soname libvulkan.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libvulkan/exports.map \
 $(DYNAMIC_VULKAN_OBJS) -L$(DYNAMIC_DIR) -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role shared-library \
 --needed libwayland-client.so --needed libc.so --soname libvulkan.so \
 --exports-tsv userland/desktop/libvulkan/api-commands.tsv $@

$(BUILD)/bin/vkdemo: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_VKDEMO_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_VKDEMO_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libc.so $@

# The Vulkan Video probe (ws083) is a plain Vulkan client like vkdemo.
DYNAMIC_VKVIDEO_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,vkvideo-probe)

$(BUILD)/bin/vkvideo-probe: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_VKVIDEO_PROBE_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_VKVIDEO_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libc.so $@

# Native media diagnostics link libmedia; optional application codecs are deliberately absent.
DYNAMIC_MEDIA_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,media-probe)

$(BUILD)/bin/media-probe: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_MEDIA_PROBE_OBJS) $(DYNAMIC_DIR)/libmedia.so $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_MEDIA_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libmedia.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libmedia.so --needed libc.so $@

# The Vulkan display events probe (ws113-p003) is a plain Vulkan client like vkdemo.
DYNAMIC_DISPLAY_EVENTS_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,display-events)

$(BUILD)/bin/display-events: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_DISPLAY_EVENTS_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_DISPLAY_EVENTS_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libc.so $@

# The compositor draws window mode with standard Vulkan (WS035 p052), and
# reaches the system (the network, ws035-p013) through libkeiland.
DYNAMIC_WAYLAND_PROGRAM_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,wayland)

$(BUILD)/bin/wayland: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_WAYLAND_PROGRAM_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libpng-compat.so $(DYNAMIC_DIR)/libjpeg-compat.so $(DYNAMIC_DIR)/libz-compat.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_WAYLAND_PROGRAM_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libtruetype.so -l:libkeiland.so -l:libpng-compat.so -l:libjpeg-compat.so -l:libz-compat.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libtruetype.so --needed libkeiland.so --needed libpng-compat.so --needed libjpeg-compat.so \
 --needed libz-compat.so --needed libc.so $@

# The test application imports only standard Wayland and Vulkan entry points.
DYNAMIC_WLTEST_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,wltest)

$(BUILD)/bin/wltest: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_WLTEST_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_WLTEST_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libc.so $@

# The acquire-fence test is wltest's window and renderer with a fence of its own.
DYNAMIC_ACQUIRE_FENCE_TEST_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,acquire-fence-test)

$(BUILD)/bin/acquire-fence-test: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_ACQUIRE_FENCE_TEST_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_ACQUIRE_FENCE_TEST_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libc.so $@

# The GPU buffer forgery test (ws103-p004): Vulkan, Wayland and the C library.
DYNAMIC_GPU_FORGE_TEST_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,gpu-forge-test)

$(BUILD)/bin/gpu-forge-test: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_GPU_FORGE_TEST_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_GPU_FORGE_TEST_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libc.so $@

# The System Menu probe (WS070 p002): Wayland, libkeiland and the C library.
DYNAMIC_MENU_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,menu-probe)

$(BUILD)/bin/menu-probe: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_MENU_PROBE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_MENU_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libkeiland.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libwayland-client.so --needed libkeiland.so --needed libc.so $@

# The settings probe (ws135-p003): Wayland, libkeiland and the C library.
DYNAMIC_KEILAND_SETTINGS_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,keiland-settings)

$(BUILD)/bin/keiland-settings: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_KEILAND_SETTINGS_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_KEILAND_SETTINGS_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libkeiland.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libwayland-client.so --needed libkeiland.so --needed libc.so $@

# The system probe (ws131-p010): Wayland, libkeiland and the C library.
DYNAMIC_KEILAND_SYSTEM_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,keiland-system)

$(BUILD)/bin/keiland-system: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_KEILAND_SYSTEM_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_KEILAND_SYSTEM_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libkeiland.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libwayland-client.so --needed libkeiland.so --needed libc.so $@

# The print test client (ws145-p003): Wayland, libkeiland and the C library.
DYNAMIC_PRINTTEST_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,printtest)

$(BUILD)/bin/printtest: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_PRINTTEST_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_PRINTTEST_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libkeiland.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libwayland-client.so --needed libkeiland.so --needed libc.so $@

# The notification client (ws156-p002): Wayland, libkeiland and the C library.
DYNAMIC_KEILAND_NOTIFY_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,keiland-notify)

$(BUILD)/bin/keiland-notify: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_KEILAND_NOTIFY_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_KEILAND_NOTIFY_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libkeiland.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libwayland-client.so --needed libkeiland.so --needed libc.so $@

# The media database helper uses libkeiland for commit notifications.
DYNAMIC_MEDIASTORAGE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,mediastorage)

$(BUILD)/bin/mediastorage: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_MEDIASTORAGE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_MEDIASTORAGE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libkeiland.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libwayland-client.so --needed libkeiland.so --needed libc.so $@

# The security key tool (ws161-p004): libpasskey's sources, OpenSSL's libcrypto (the package's staged headers and
# library; the Guardrail's exception until the release, WS172 p005) and the C library.
DYNAMIC_FIDOCTL_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,fidoctl)
DYNAMIC_FIDOCTL_SSL := $(ZEDBSD_EXT_openssl_STAGEDIR)/usr

$(DYNAMIC_FIDOCTL_OBJS): DYNAMIC_CPPFLAGS += -I$(DYNAMIC_FIDOCTL_SSL)/include
$(DYNAMIC_FIDOCTL_OBJS): $(ZEDBSD_OPENSSL_STAGED)

$(BUILD)/bin/fidoctl: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_FIDOCTL_OBJS) $(ZEDBSD_OPENSSL_LIBCRYPTO) \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_FIDOCTL_OBJS) \
 -L$(DYNAMIC_DIR) -L$(DYNAMIC_FIDOCTL_SSL)/lib -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libcrypto.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libcrypto.so --needed libc.so $@

# passkey's security key style (ws172-p003): libpasskey's sources, OpenSSL's libcrypto (as fidoctl's) and the C
# library (crypt() for the password of a registration).
DYNAMIC_PASSKEY_FIDO2_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,passkey-fido2)

$(DYNAMIC_PASSKEY_FIDO2_OBJS): DYNAMIC_CPPFLAGS += -I$(DYNAMIC_FIDOCTL_SSL)/include
$(DYNAMIC_PASSKEY_FIDO2_OBJS): $(ZEDBSD_OPENSSL_STAGED)

$(BUILD)/bin/passkey-fido2: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_PASSKEY_FIDO2_OBJS) $(ZEDBSD_OPENSSL_LIBCRYPTO) \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_PASSKEY_FIDO2_OBJS) \
 -L$(DYNAMIC_DIR) -L$(DYNAMIC_FIDOCTL_SSL)/lib -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libcrypto.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libcrypto.so --needed libc.so $@

# The Titlebar Presentation probe (WS070 p008): Wayland, libkeiland and the C library.
DYNAMIC_TITLEBAR_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,titlebar-probe)

$(BUILD)/bin/titlebar-probe: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_TITLEBAR_PROBE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_TITLEBAR_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libkeiland.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libwayland-client.so --needed libkeiland.so --needed libc.so $@

# The popup probe (WS035 p076): standard Wayland and the C library.
DYNAMIC_POPUP_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,popup-probe)

$(BUILD)/bin/popup-probe: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_POPUP_PROBE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_POPUP_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libkeiland.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libwayland-client.so --needed libkeiland.so --needed libc.so $@

# The sub-surface probe (WS035 p077): standard Wayland and the C library.
DYNAMIC_SUBSURFACE_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,subsurface-probe)

$(BUILD)/bin/subsurface-probe: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_SUBSURFACE_PROBE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_SUBSURFACE_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libwayland-client.so --needed libc.so $@

# The keyboard and output probe (WS035 p078): standard Wayland and the C library.
DYNAMIC_SEAT_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,seat-probe)

$(BUILD)/bin/seat-probe: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_SEAT_PROBE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_SEAT_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libwayland-client.so --needed libc.so $@

# The pen probe (WS079 p003): standard Wayland (the tablet protocol) and the C library.
DYNAMIC_TABLET_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,tablet-probe)

$(BUILD)/bin/tablet-probe: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_TABLET_PROBE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_TABLET_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libwayland-client.so --needed libc.so $@

# The clipboard probe (WS035 p079): standard Wayland and the C library.
DYNAMIC_DATA_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,data-probe)

$(BUILD)/bin/data-probe: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_DATA_PROBE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_DATA_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libwayland-client.so --needed libc.so $@

# The decoration, cursor-shape and viewporter probe (WS035 p080): standard Wayland and the C library.
DYNAMIC_EXTRAS_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,extras-probe)

$(BUILD)/bin/extras-probe: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_EXTRAS_PROBE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_EXTRAS_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libwayland-client.so --needed libc.so $@

# The wl_shm test client imports only standard Wayland and C library entry points.
DYNAMIC_WLSHM_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,wlshm)

$(BUILD)/bin/wlshm: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_WLSHM_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_WLSHM_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libkeiland.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libwayland-client.so --needed libkeiland.so --needed libc.so $@

# The input method (ws095-p004) imports standard Wayland and C library entry points; its candidate window
# (ws095-p005) draws with libkeiland's canvas and text (libkeiland/ui, and so with what libkeiland links).  Its test client imports
# only standard Wayland and C library entry points.
DYNAMIC_KEILAND_IME_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,keiland-ime)

$(BUILD)/bin/keiland-ime: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_KEILAND_IME_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libpng-compat.so $(DYNAMIC_DIR)/libz-compat.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_KEILAND_IME_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libpng-compat.so -l:libz-compat.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libpng-compat.so --needed libz-compat.so --needed libc.so $@

DYNAMIC_IME_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,ime-probe)

$(BUILD)/bin/ime-probe: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_IME_PROBE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_IME_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libwayland-client.so --needed libc.so $@

# The model viewer imports only standard Wayland, Vulkan and C library entry points.
DYNAMIC_MVIEW_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,mview)

$(BUILD)/bin/mview: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_MVIEW_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_MVIEW_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libc.so $@

# The terminal imports standard Wayland, Vulkan, TrueType and C library entry
# points (WS035 p068), and the compositor's System Menu through libkeiland (WS070).
DYNAMIC_TERMINAL_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,terminal)

$(BUILD)/bin/terminal: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_TERMINAL_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_TERMINAL_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so --needed libc.so $@

# The file manager (WS071) imports standard Wayland, Vulkan, TrueType and C
# library entry points, and the compositor's own extensions through libkeiland.
# ws168-p004: no decoder (keiland-preview makes its pictures in a sandbox).
DYNAMIC_FILES_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,files)

$(BUILD)/bin/files: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_FILES_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_FILES_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so --needed libc.so $@

# Settings (WS089) imports standard Wayland, Vulkan, TrueType and C library entry points, and
# the compositor's own extensions through libkeiland.  It compiles the file manager's canvas, text and
# icons (the same objects as files: the pattern rule builds them once).
DYNAMIC_SETTINGS_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,settings)

$(BUILD)/bin/settings: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_SETTINGS_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_SETTINGS_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so --needed libc.so $@

# The System Monitor (ws134-p002) imports standard Wayland, Vulkan, TrueType and C library entry points,
# and the titlebar, and the window and text (libkeiland/ui), through libkeiland.
DYNAMIC_MONITOR_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,monitor)

$(BUILD)/bin/monitor: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_MONITOR_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_MONITOR_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libc.so $@

# Notes (ws079-p005) imports standard Wayland, Vulkan, TrueType and C library entry points,
# the compositor's System Menu and the recent files through libkeiland, and libpdf for its PDF; ws175-p007: libz-compat
# for the images it keeps compressed; ws175-p008: libpng-compat, libjpeg-compat and libgif-compat for the image files
# it puts on a page (picture.c).
DYNAMIC_NOTES_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,notes)

$(BUILD)/bin/notes: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_NOTES_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so $(DYNAMIC_DIR)/libpng-compat.so $(DYNAMIC_DIR)/libjpeg-compat.so \
	$(DYNAMIC_DIR)/libgif-compat.so $(DYNAMIC_DIR)/libz-compat.so $(DYNAMIC_DIR)/libpdf.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_NOTES_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libpng-compat.so -l:libjpeg-compat.so \
 -l:libgif-compat.so -l:libz-compat.so -l:libpdf.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libpng-compat.so --needed libjpeg-compat.so --needed libgif-compat.so --needed libz-compat.so --needed libpdf.so \
 --needed libc.so $@

# PDF Viewer (ws079-p006) imports standard Wayland, Vulkan, TrueType and C library entry points,
# the compositor's menus and titlebar through libkeiland, and libpdf (which brings libz-compat and
# libjpeg-compat); ws189-p003: libz-compat itself, for the PNG of an image dragged out.
DYNAMIC_PDFVIEWER_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,pdfviewer)

$(BUILD)/bin/pdfviewer: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_PDFVIEWER_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so $(DYNAMIC_DIR)/libpdf.so \
	$(DYNAMIC_DIR)/libz-compat.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_PDFVIEWER_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libpdf.so -l:libz-compat.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libpdf.so --needed libz-compat.so --needed libc.so $@

# Image Viewer (ws091) imports standard Wayland, Vulkan, TrueType and C library entry points,
# the compositor's menus, titlebar and glass through libkeiland, and libpng-compat (with libz-compat),
# libjpeg-compat and libgif-compat for its images.
DYNAMIC_IMAGEVIEW_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,imageview)

$(BUILD)/bin/imageview: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_IMAGEVIEW_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so $(DYNAMIC_DIR)/libpng-compat.so \
	$(DYNAMIC_DIR)/libz-compat.so $(DYNAMIC_DIR)/libjpeg-compat.so $(DYNAMIC_DIR)/libgif-compat.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_IMAGEVIEW_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libpng-compat.so \
 -l:libz-compat.so -l:libjpeg-compat.so -l:libgif-compat.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libpng-compat.so --needed libz-compat.so --needed libjpeg-compat.so --needed libgif-compat.so \
 --needed libc.so $@

# Video Player (WS122) imports standard Wayland, Vulkan, TrueType and C library entry points, the window and the
# widgets through libkeiland, and the container reader and the decoders through libmedia (ws177-p031).  It is not
# linked to FFmpeg: its application adapter opens optional libavcodec, libavutil and libswscale with dlopen
# when the system has them (the libavcodec package), without FFmpeg's headers.
DYNAMIC_VIDEOPLAYER_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,videoplayer)

$(BUILD)/bin/videoplayer: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_VIDEOPLAYER_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so $(DYNAMIC_DIR)/libmedia.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_VIDEOPLAYER_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libmedia.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libmedia.so --needed libc.so $@

# Music (WS120) imports standard Wayland, Vulkan, TrueType and C library entry points, the window and the widgets
# through libkeiland, and the covers' decoding through libjpeg-compat and libpng-compat (picture.c's GIF part needs
# libgif-compat), and the container reader and the decoders through libmedia (ws177-p031).  Like Video Player it is
# not linked to FFmpeg: its application adapter opens optional libavcodec with dlopen.
DYNAMIC_MUSIC_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,music)

$(BUILD)/bin/music: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_MUSIC_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so $(DYNAMIC_DIR)/libpng-compat.so $(DYNAMIC_DIR)/libz-compat.so \
	$(DYNAMIC_DIR)/libjpeg-compat.so $(DYNAMIC_DIR)/libgif-compat.so $(DYNAMIC_DIR)/libmedia.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_MUSIC_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libpng-compat.so -l:libz-compat.so \
 -l:libjpeg-compat.so -l:libgif-compat.so -l:libmedia.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libpng-compat.so --needed libz-compat.so --needed libjpeg-compat.so --needed libgif-compat.so --needed libmedia.so \
 --needed libc.so $@

# Photos (WS157) imports standard Wayland, Vulkan, TrueType and C library entry points, the window and the widgets
# through libkeiland, and the pictures' decoding through libjpeg-compat, libpng-compat and libgif-compat.
DYNAMIC_PHOTOS_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,photos)

$(BUILD)/bin/photos: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_PHOTOS_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so $(DYNAMIC_DIR)/libpng-compat.so $(DYNAMIC_DIR)/libz-compat.so \
	$(DYNAMIC_DIR)/libjpeg-compat.so $(DYNAMIC_DIR)/libgif-compat.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_PHOTOS_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libpng-compat.so -l:libz-compat.so \
 -l:libjpeg-compat.so -l:libgif-compat.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libpng-compat.so --needed libz-compat.so --needed libjpeg-compat.so --needed libgif-compat.so --needed libc.so $@

# Phone (WS170 p000, the mock of the messages and calls application) imports standard Wayland, Vulkan,
# TrueType and C library entry points, and the window, the widgets and the drawing through libkeiland.
DYNAMIC_PHONE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,phone)

$(BUILD)/bin/phone: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_PHONE_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libpng-compat.so $(DYNAMIC_DIR)/libz-compat.so $(DYNAMIC_DIR)/libjpeg-compat.so $(DYNAMIC_DIR)/libgif-compat.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_PHONE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libpng-compat.so -l:libz-compat.so -l:libjpeg-compat.so -l:libgif-compat.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libpng-compat.so --needed libz-compat.so --needed libjpeg-compat.so --needed libgif-compat.so --needed libc.so $@

# Calendar (WS155 p000, the mock of the calendar) imports standard Wayland, Vulkan, TrueType, the C library's and
# its mathematics' entry points, and the window, the widgets and the drawing through libkeiland.
DYNAMIC_CALENDAR_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,calendar)

$(BUILD)/bin/calendar: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_CALENDAR_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_CALENDAR_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libc.so $@

# Mail (WS169 p000, the mock of the mail application) imports standard Wayland, Vulkan, TrueType and C library entry
# points, and the window, the widgets and the drawing through libkeiland.
DYNAMIC_MAILER_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,mailer)

$(BUILD)/bin/mailer: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_MAILER_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_MAILER_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libc.so $@

# Text Editor (WS092) imports standard Wayland, Vulkan, TrueType and C library entry points, and
# the compositor's menus, titlebar, glass and recent files through libkeiland.
DYNAMIC_TEXTEDIT_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,textedit)

$(BUILD)/bin/textedit: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_TEXTEDIT_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libpng-compat.so $(DYNAMIC_DIR)/libz-compat.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_TEXTEDIT_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libpng-compat.so -l:libz-compat.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libpng-compat.so --needed libz-compat.so --needed libc.so $@

# The widgets' sampler (WS090 ws090-p005, the test image only) imports standard Wayland, Vulkan, TrueType
# and C library entry points, and the widgets, the window and the compositor's glass through libkeiland.
DYNAMIC_KUIDEMO_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,kuidemo)

$(BUILD)/bin/kuidemo: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_KUIDEMO_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_KUIDEMO_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libc.so $@

# libmedia (ws121-p002, WS202): media containers, original AAC-LC and standard Vulkan Video H.264
# (optional Vulkan loader through dlopen, the audiod client and the engine); it links only the C library.
DYNAMIC_MEDIA_LIBRARY_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libmedia)

$(DYNAMIC_DIR)/libmedia.so: $(DYNAMIC_MEDIA_LIBRARY_OBJS) $(DYNAMIC_DIR)/libc.so \
	userland/desktop/libmedia/exports.map tools/build/check-dynamic-elf.py
	$(LD) $(DYNAMIC_LINK_LDFLAGS) -shared -soname libmedia.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libmedia/exports.map \
 $(DYNAMIC_MEDIA_LIBRARY_OBJS) -L$(DYNAMIC_DIR) -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine $(DYNAMIC_ELF_MACHINE) --role shared-library \
 --needed libc.so --soname libmedia.so $@

# The Web browser engine (WS074, libbrowser since ws074-p057) keeps its modules in subdirectories of
# userland/desktop/libbrowser and includes their private headers from that root; it imports standard Vulkan for its
# GPU renderer (ws074-p014), libtruetype for its text, libjpeg-compat, libpng-compat (with
# libz-compat) and libgif-compat for its images (ws074-p021), and libmedia for <video> and <audio> (ws121-p004).  Only the calls of <browser.h> leave it.
DYNAMIC_BROWSER_LIBRARY_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libbrowser)
$(DYNAMIC_BROWSER_LIBRARY_OBJS): DYNAMIC_CPPFLAGS += -Iuserland/desktop/libbrowser

$(DYNAMIC_DIR)/libbrowser.so: $(DYNAMIC_BROWSER_LIBRARY_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libmedia.so \
	$(DYNAMIC_DIR)/libjpeg-compat.so $(DYNAMIC_DIR)/libpng-compat.so $(DYNAMIC_DIR)/libz-compat.so \
	$(DYNAMIC_DIR)/libgif-compat.so $(DYNAMIC_DIR)/libc.so \
	userland/desktop/libbrowser/exports.map tools/build/check-dynamic-elf.py
	$(LD) $(DYNAMIC_LINK_LDFLAGS) -shared -soname libbrowser.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libbrowser/exports.map \
 $(DYNAMIC_BROWSER_LIBRARY_OBJS) -L$(DYNAMIC_DIR) -l:libvulkan.so -l:libtruetype.so -l:libmedia.so \
 -l:libjpeg-compat.so -l:libpng-compat.so -l:libz-compat.so -l:libgif-compat.so -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine $(DYNAMIC_ELF_MACHINE) --role shared-library \
 --needed libvulkan.so --needed libtruetype.so --needed libmedia.so --needed libjpeg-compat.so --needed libpng-compat.so \
 --needed libz-compat.so --needed libgif-compat.so --needed libc.so --soname libbrowser.so $@

# /bin/browser is the shell over libbrowser (ws074-p057): its command line and headless modes (main.c)
# and its window (shell/), which import the engine through <browser.h>, standard Wayland and Vulkan
# for the window and its swapchain (ws074-p014), and the compositor's titlebar through libkeiland (ws074-p045).
DYNAMIC_BROWSER_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,browser)
$(DYNAMIC_BROWSER_OBJS): DYNAMIC_CPPFLAGS += -Iuserland/desktop/browser

$(BUILD)/bin/browser: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_BROWSER_OBJS) $(DYNAMIC_DIR)/libbrowser.so $(DYNAMIC_DIR)/libvulkan.so \
	$(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libz-compat.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so \
	$(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_BROWSER_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libbrowser.so -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libz-compat.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libbrowser.so --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so \
 --needed libz-compat.so --needed libc.so $@

# The second program over libbrowser (ws074-p057): <browser.h> and the C library, no window.
DYNAMIC_BROWSER_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,browser-probe)

$(BUILD)/bin/browser-probe: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_BROWSER_PROBE_OBJS) $(DYNAMIC_DIR)/libbrowser.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so \
	$(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_BROWSER_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libbrowser.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libbrowser.so --needed libc.so $@

# OpenGL (WS069 p004; WS178): libGLESv2's translation with the fixed function and OpenGL 3.x, GL only.
DYNAMIC_GL_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libgl)

$(DYNAMIC_DIR)/libGL.so: $(DYNAMIC_GL_OBJS) $(DYNAMIC_DIR)/libEGL.so $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libc.so \
	userland/desktop/libGL/exports.map tools/build/check-dynamic-elf.py
	$(LD) $(DYNAMIC_LINK_LDFLAGS) -shared -soname libGL.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libGL/exports.map \
 $(DYNAMIC_GL_OBJS) -L$(DYNAMIC_DIR) -l:libEGL.so -l:libvulkan.so -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine $(DYNAMIC_ELF_MACHINE) --role shared-library \
 --needed libEGL.so --needed libvulkan.so --needed libc.so --soname libGL.so $@

# GLX (WS069 p004; WS178): xserver's library, contexts of libGL's GL for X windows, a private libX11 inside.
DYNAMIC_GLX_OBJS := $(patsubst %.c,$(DYNAMIC_DIR)/obj/%.o,$(KEILAND_LIBGLX_SOURCES))

$(DYNAMIC_DIR)/libGLX.so: $(DYNAMIC_GLX_OBJS) $(DYNAMIC_DIR)/libGL.so $(DYNAMIC_DIR)/libEGL.so $(DYNAMIC_DIR)/libc.so \
	userland/desktop/xserver/libGLX/exports.map tools/build/check-dynamic-elf.py
	$(LD) $(DYNAMIC_LINK_LDFLAGS) -shared -soname libGLX.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/xserver/libGLX/exports.map \
 $(DYNAMIC_GLX_OBJS) -L$(DYNAMIC_DIR) -l:libGL.so -l:libEGL.so -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine $(DYNAMIC_ELF_MACHINE) --role shared-library \
 --needed libGL.so --needed libEGL.so --needed libc.so --soname libGLX.so $@

# The GLX test application (WS069 p004): Xlib built in, GL from libGL and GLX from libGLX.
DYNAMIC_GLXTEST_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,glxtest)

$(BUILD)/bin/glxtest: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_GLXTEST_OBJS) $(DYNAMIC_DIR)/libGL.so $(DYNAMIC_DIR)/libGLX.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_GLXTEST_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libGL.so -l:libGLX.so -l:libc.so -o $@

# Gears (WS069 p005): OpenGL 1.x's fixed function through GLX, Xlib built in; GL from libGL, GLX from libGLX.
DYNAMIC_ZGEARS_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,zgears)

$(BUILD)/bin/zgears: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_ZGEARS_OBJS) $(DYNAMIC_DIR)/libGL.so $(DYNAMIC_DIR)/libGLX.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_ZGEARS_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libGL.so -l:libGLX.so -l:libc.so -o $@

# The EGL test application (WS068 p002) imports standard Wayland, EGL and GLES entry points.
DYNAMIC_EGLTEST_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,egltest)

$(BUILD)/bin/egltest: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_EGLTEST_OBJS) $(DYNAMIC_DIR)/libEGL.so $(DYNAMIC_DIR)/libGLESv2.so $(DYNAMIC_DIR)/libwayland-egl.so \
	$(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_EGLTEST_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libEGL.so -l:libGLESv2.so -l:libwayland-egl.so -l:libwayland-client.so -l:libc.so -o $@

# The OpenGL ES 3.1 compute test (ws101-p009) imports standard EGL and GLES entry points (a pbuffer context, no window).
DYNAMIC_GLESCOMPUTE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,glescompute)

$(BUILD)/bin/glescompute: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_GLESCOMPUTE_OBJS) $(DYNAMIC_DIR)/libEGL.so $(DYNAMIC_DIR)/libGLESv2.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_GLESCOMPUTE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libEGL.so -l:libGLESv2.so -l:libc.so -o $@

# The compositor's X11 server imports standard Wayland, Vulkan, TrueType and C library entry points (WS069 p008, p011).
DYNAMIC_X11SERVER_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,xserver)

$(BUILD)/bin/xserver: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_X11SERVER_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o $(DYNAMIC_X11SERVER_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so --needed libc.so $@

# The external-fence test uses only the installed standard Vulkan shared library.
$(BUILD)/bin/gpu-fence-test: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_DIR)/obj/userland/tests/gpu-fence/main.o \
	$(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
 $(DYNAMIC_DIR)/obj/userland/tests/gpu-fence/main.o \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libvulkan.so --needed libc.so $@

# This finite fixture links private Vulkan helpers without exporting them from the public DSO.
$(DYNAMIC_DIR)/obj/userland/tests/gpu-share/main.o: DYNAMIC_CPPFLAGS += -Iuserland/desktop/libvulkan

$(BUILD)/bin/gpu-share-test: $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
	$(DYNAMIC_DIR)/obj/userland/tests/gpu-share/main.o $(DYNAMIC_VULKAN_OBJS) \
	$(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_LINK_CFLAGS) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(DYNAMIC_SYSROOT)/usr/lib/crt1.o \
 $(DYNAMIC_DIR)/obj/userland/tests/gpu-share/main.o $(DYNAMIC_VULKAN_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine $(DYNAMIC_ELF_MACHINE) --role application \
 --needed libwayland-client.so --needed libc.so $@
