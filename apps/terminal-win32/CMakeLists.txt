# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# mikuterm (Micro Unicode Terminal): zedBSD's terminal (userland/desktop/terminal) on
# libkeiland's Windows backend, with zedBSD's own SSH client engine (userland/base/ssh) for
# each tab, as one Windows program that runs from the folder its zip unpacks to.
#
# The build is its own: it reads no Makefile of the tree but the source lists the libraries
# keep (sources.mk), and names the rest here.  Two ways to build it:
#
#   llvm-mingw (cross, x64 or arm64):
#     cmake -S apps/terminal-win32 -B <build> -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=$PWD/apps/terminal-win32/cmake/llvm-mingw-x86_64.cmake -DLLVM_MINGW=<root>
#     cmake --build <build> --target zip
#   Visual Studio (MSVC or clang-cl, x64 or ARM64):
#     cmake -S apps/terminal-win32 -B <build> -A x64
#     cmake --build <build> --config Release --target zip
#
# The frames are painted on the processor and shown by libkeiland (no Vulkan).  MIKUTERM_VULKAN
# keeps libkeiland's Vulkan surface in the build; off, libkeiland reads no Vulkan header.  The X
# server's drawing and showing (present-vulkan.c, render-vulkan.c, win32/vulkan.c) always read
# Vulkan's headers (MIKUTERM_VULKAN_INCLUDE, or the release's tarball: a copy in
# MIKUTERM_DISTFILES first, else fetched, checked against its SHA-256) and open vulkan-1.dll at
# run time; the program imports no Vulkan library.  The zip's fonts are the tree's, and Noto Color Emoji, fetched and checked against
# the SHA-256 its package records (a copy in MIKUTERM_DISTFILES is used first); nothing fetched
# goes into the tree.
#

cmake_minimum_required(VERSION 3.20)
project(mikuterm VERSION 0.1.0 LANGUAGES C RC)

get_filename_component(MIKUTERM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(MIKUTERM_DIR ${CMAKE_CURRENT_SOURCE_DIR})

option(MIKUTERM_VULKAN "Keep libkeiland's Vulkan surface (needs Vulkan's headers)" OFF)
set(MIKUTERM_VULKAN_INCLUDE "" CACHE PATH "A directory holding vulkan/vulkan.h (empty: fetch the headers)")
set(MIKUTERM_DISTFILES "${MIKUTERM_ROOT}/build/distfiles" CACHE PATH "Where downloaded files may already be")

# The warnings: every one is an error with a GNU-style compiler (clang, llvm-mingw's); MSVC's
# warnings stay warnings unless MIKUTERM_WERROR is set.
if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
	set(MIKUTERM_MSVC_STYLE ON)
	set(MIKUTERM_WERROR_DEFAULT OFF)
else()
	set(MIKUTERM_MSVC_STYLE OFF)
	set(MIKUTERM_WERROR_DEFAULT ON)
endif()
option(MIKUTERM_WERROR "Make every warning an error" ${MIKUTERM_WERROR_DEFAULT})

# The architecture's name in the zip's name.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|ARM64|arm64)$" OR CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
	set(MIKUTERM_ZIP_ARCH arm64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86|i686|i586|i486|i386)$")
	set(MIKUTERM_ZIP_ARCH x86)
else()
	set(MIKUTERM_ZIP_ARCH x64)
endif()

# The lists mikuterm's builds share, and the reader of the tree's sources.mk lists.
include(${MIKUTERM_ROOT}/apps/mikuterm-common/cmake/mikuterm-sources.cmake)

mikuterm_read_sources(MIKUTERM_TRUETYPE_SOURCES userland/desktop/libtruetype/sources.mk LIBTRUETYPE_SOURCES)
mikuterm_read_sources(MIKUTERM_PNG_SOURCES userland/desktop/libpng-compat/sources.mk LIBPNG_COMPAT_SOURCES)
mikuterm_read_sources(MIKUTERM_Z_SOURCES userland/base/libz-compat/sources.mk LIBZ_COMPAT_SOURCES)
mikuterm_read_sources(MIKUTERM_CRYPTO_SOURCES userland/base/libcrypto-compat/sources.mk LIBCRYPTO_COMPAT_SOURCES)
mikuterm_read_sources(MIKUTERM_CRYPTO_OS_SOURCES userland/base/libcrypto-compat/sources.mk LIBCRYPTO_COMPAT_WIN32_SOURCES)
mikuterm_read_sources(MIKUTERM_SSL_SOURCES userland/base/libssl-compat/sources.mk LIBSSL_COMPAT_SOURCES)
mikuterm_read_sources(MIKUTERM_RDP_SOURCES userland/desktop/librdp/sources.mk LIBRDP_SOURCES)

# libkeiland on Windows: the files every system shares, the Windows ones, and the stubs of what Windows has not
# (the lists are in apps/libkeiland/cmake/keiland-sources.cmake, shared by the programs that use libkeiland).
include(${MIKUTERM_ROOT}/apps/libkeiland/cmake/keiland-sources.cmake)
set(MIKUTERM_KEILAND_SOURCES ${KEILAND_COMMON_SOURCES} ${KEILAND_WIN32_SOURCES})
list(TRANSFORM MIKUTERM_KEILAND_SOURCES PREPEND ${MIKUTERM_ROOT}/)

# mikuterm's own files: the Windows ones here, and the ones every build shares (apps/mikuterm-common).
set(MIKUTERM_SOURCES
	${MIKUTERM_COMMON_SOURCES}
	${MIKUTERM_COMMON_SFTP_SOURCES}
	${MIKUTERM_COMMON_RDP_SOURCES}
	${MIKUTERM_COMMON_MEDIA_SOURCES}
	${MIKUTERM_COMMON_X11_SOURCES}
	${MIKUTERM_COMMON_PROGRAM_SOURCES}
	${MIKUTERM_DIR}/main.c
	${MIKUTERM_DIR}/folder.c
	${MIKUTERM_DIR}/wsl.c
	${MIKUTERM_DIR}/program.c
	${MIKUTERM_DIR}/proxy.c
	${MIKUTERM_DIR}/credential.c
	${MIKUTERM_DIR}/extdisp.c
	${MIKUTERM_DIR}/extdisp-gpu.c
	${MIKUTERM_DIR}/extdisp-d3d12.c
	${MIKUTERM_DIR}/sftp-local-win32.c
	${MIKUTERM_DIR}/sftp-drag-win32.c
	${MIKUTERM_DIR}/vi-connection-win32.c
	${MIKUTERM_DIR}/x11-spawn-win32.c
	${MIKUTERM_DIR}/x11-child.c
	${MIKUTERM_DIR}/x11-tab-child.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/client.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/glyphs.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/keymap.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/protocol.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/rootless.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/selection.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/window.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/resource.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/event.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/property.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/region.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/render-cpu.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/render-vulkan.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/present-vulkan.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/gc.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/draw.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/shape.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/colormap.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/colornames.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/input.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/wm.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/font.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/cursor.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/xkb.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/xinput.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/rooted.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/extension.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/shape-ext.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/xfixes.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/sync.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/render-ext.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/render-composite.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/coverage.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/randr.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/xinerama.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/win32/backend.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/win32/window.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/win32/input.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/win32/ime.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/win32/layout.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/win32/clipboard.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/win32/scancode.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/win32/keysyms.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/win32/scale.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/win32/text.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/win32/vulkan.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/win32/cursor.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/win32/decor.c
	${MIKUTERM_DIR}/resources/mikuterm.rc)

# The compat headers (<compat/zlib/zlib.h> and the others), copied from zedBSD's C library side so
# that the rest of that side is not on the include path.
set(MIKUTERM_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
file(GLOB_RECURSE MIKUTERM_COMPAT_HEADERS CONFIGURE_DEPENDS RELATIVE ${MIKUTERM_ROOT}/include/libc
	${MIKUTERM_ROOT}/include/libc/compat/*.h)
foreach(header IN LISTS MIKUTERM_COMPAT_HEADERS)
	configure_file(${MIKUTERM_ROOT}/include/libc/${header} ${MIKUTERM_INCLUDE}/${header} COPYONLY)
endforeach()

# The headers vi needs from zedBSD's C library side (<regex.h>, <terminfo.h>), copied apart so
# that the rest of include/libc is not on the include path.
set(MIKUTERM_VI_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/vi-include)
configure_file(${MIKUTERM_ROOT}/include/libc/regex.h ${MIKUTERM_VI_INCLUDE}/regex.h COPYONLY)
configure_file(${MIKUTERM_ROOT}/include/libc/terminfo.h ${MIKUTERM_VI_INCLUDE}/terminfo.h COPYONLY)

# The version, for the resource and Help > About.
configure_file(${MIKUTERM_VERSION_IN} ${MIKUTERM_INCLUDE}/mikuterm-version.h @ONLY)

# Vulkan's headers: the given directory, or the release's tarball (MIKUTERM_DISTFILES' copy first).
set(MIKUTERM_VULKAN_SDK_TAG vulkan-sdk-1.4.309.0)
set(MIKUTERM_VULKAN_SHA256 2bc1b4127950badc80212abf1edfa5c3b5032f3425edf37255863ba7592c1969)
if(NOT MIKUTERM_VULKAN_INCLUDE)
	set(tarball ${CMAKE_CURRENT_BINARY_DIR}/sources/Vulkan-Headers-${MIKUTERM_VULKAN_SDK_TAG}.tar.gz)
	set(local ${MIKUTERM_DISTFILES}/Vulkan-Headers-${MIKUTERM_VULKAN_SDK_TAG}.tar.gz)
	if(NOT EXISTS ${tarball} AND EXISTS ${local})
		file(SHA256 ${local} local_sha256)
		if(local_sha256 STREQUAL MIKUTERM_VULKAN_SHA256)
			configure_file(${local} ${tarball} COPYONLY)
		endif()
	endif()
	if(NOT EXISTS ${tarball})
		file(DOWNLOAD
			https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/${MIKUTERM_VULKAN_SDK_TAG}.tar.gz
			${tarball}
			EXPECTED_HASH SHA256=${MIKUTERM_VULKAN_SHA256}
			TLS_VERIFY ON)
	endif()
	file(ARCHIVE_EXTRACT INPUT ${tarball} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/vulkan-headers
		PATTERNS Vulkan-Headers-${MIKUTERM_VULKAN_SDK_TAG}/include)
	set(MIKUTERM_VULKAN_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/vulkan-headers/Vulkan-Headers-${MIKUTERM_VULKAN_SDK_TAG}/include)
endif()

#
# The flags every C file of the build is compiled with: Windows 10's API in UTF-16, the tree's
# paths from its top, the copied compat headers; C11 with GNU's extensions where the compiler
# has them.
#
add_library(mikuterm_flags INTERFACE)
target_include_directories(mikuterm_flags INTERFACE
	${MIKUTERM_ROOT}
	${MIKUTERM_ROOT}/userland/desktop/include
	${MIKUTERM_INCLUDE})
target_compile_definitions(mikuterm_flags INTERFACE
	_WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE)
if(MIKUTERM_VULKAN)
	target_include_directories(mikuterm_flags INTERFACE ${MIKUTERM_VULKAN_INCLUDE})
else()
	target_compile_definitions(mikuterm_flags INTERFACE KEILAND_WIN32_NO_VULKAN)
endif()
# The X server's Vulkan files read Vulkan's headers whether libkeiland's surface is kept or not.
set_source_files_properties(
	${MIKUTERM_DIR}/extdisp-gpu.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/render-vulkan.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/present-vulkan.c
	${MIKUTERM_ROOT}/userland/desktop/xserver/win32/vulkan.c
	PROPERTIES INCLUDE_DIRECTORIES ${MIKUTERM_VULKAN_INCLUDE})
if(MIKUTERM_MSVC_STYLE)
	target_compile_options(mikuterm_flags INTERFACE $<$<COMPILE_LANGUAGE:C>:/std:c11 /W3 /utf-8>)
	target_compile_definitions(mikuterm_flags INTERFACE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
	if(MIKUTERM_WERROR)
		target_compile_options(mikuterm_flags INTERFACE $<$<COMPILE_LANGUAGE:C>:/WX>)
	endif()
else()
	target_compile_options(mikuterm_flags INTERFACE
		$<$<COMPILE_LANGUAGE:C>:-std=gnu11 -Wall -Wextra -Wno-unknown-warning-option -Wno-maybe-uninitialized>)
	if(MIKUTERM_WERROR)
		target_compile_options(mikuterm_flags INTERFACE $<$<COMPILE_LANGUAGE:C>:-Werror>)
	endif()
endif()

# Static libmedia (Vulkan Video H.264 with h264sw fallback), reusing the fetched Vulkan headers.
set(LIBMEDIA_VULKAN_INCLUDE "${MIKUTERM_VULKAN_INCLUDE}" CACHE PATH "" FORCE)
set(LIBMEDIA_DISTFILES "${MIKUTERM_DISTFILES}" CACHE PATH "" FORCE)
add_subdirectory(${MIKUTERM_ROOT}/apps/libmedia ${CMAKE_CURRENT_BINARY_DIR}/libmedia)

# The libraries, each a static library of the tree's files.
add_library(mikuterm_z STATIC ${MIKUTERM_Z_SOURCES})
add_library(mikuterm_png STATIC ${MIKUTERM_PNG_SOURCES})
add_library(mikuterm_truetype STATIC ${MIKUTERM_TRUETYPE_SOURCES})
add_library(mikuterm_crypto STATIC ${MIKUTERM_CRYPTO_SOURCES} ${MIKUTERM_CRYPTO_OS_SOURCES})
add_library(mikuterm_ssl STATIC ${MIKUTERM_SSL_SOURCES})
add_library(mikuterm_rdp STATIC ${MIKUTERM_RDP_SOURCES})
add_library(mikuterm_ssh STATIC ${MIKUTERM_SSH_SOURCES})
add_library(mikuterm_keiland STATIC ${MIKUTERM_KEILAND_SOURCES})
add_library(mikuterm_terminal STATIC ${MIKUTERM_TERMINAL_SOURCES})
foreach(library z png truetype crypto ssl rdp ssh keiland terminal)
	target_link_libraries(mikuterm_${library} PRIVATE mikuterm_flags)
endforeach()
target_link_libraries(mikuterm_png PUBLIC mikuterm_z)
target_link_libraries(mikuterm_ssl PUBLIC mikuterm_crypto)
target_link_libraries(mikuterm_ssh PUBLIC mikuterm_crypto)
target_link_libraries(mikuterm_keiland PUBLIC mikuterm_truetype mikuterm_png)
target_link_libraries(mikuterm_terminal PUBLIC mikuterm_keiland mikuterm_ssh)

# The system's libraries the program imports (each a DLL that comes with Windows, and the UCRT).
set(MIKUTERM_SYSTEM_LIBRARIES
	ws2_32 iphlpapi bcrypt crypt32 dwmapi shcore imm32 shell32 ole32 uuid gdi32 user32 advapi32)

# The program, with its features (1 or 0): no login shell (WSL is Windows' own), ProxyCommand and GitHub Codespaces.
add_executable(mikuterm WIN32 ${MIKUTERM_SOURCES})
target_compile_definitions(mikuterm PRIVATE
	MIKUTERM_HAVE_WSL=1
	MIKUTERM_HAVE_OPENER=1
	MIKUTERM_HAVE_X11=1
	MIKUTERM_HAVE_KEY_PASTE=0
	MIKUTERM_HAVE_LOGIN_SHELL=0
	MIKUTERM_HAVE_PROXY_COMMAND=1
	MIKUTERM_HAVE_CODESPACES=1
	MIKUTERM_HAVE_LIBMEDIA=1
	MIKUTERM_HAVE_SFTP_DRAG=1
	MIKUTERM_HAVE_EXTDISP=1)
target_link_libraries(mikuterm PRIVATE mikuterm_flags mikuterm_terminal mikuterm_ssh mikuterm_rdp mikuterm_ssl libmedia ${MIKUTERM_SYSTEM_LIBRARIES})
set_source_files_properties(${MIKUTERM_DIR}/resources/mikuterm.rc PROPERTIES
	OBJECT_DEPENDS "${MIKUTERM_DIR}/resources/mikuterm.manifest;${MIKUTERM_DIR}/resources/mikuterm.ico")
target_include_directories(mikuterm PRIVATE ${MIKUTERM_DIR}/resources ${MIKUTERM_COMMON_DIR})

#
# The link: the C runtime's own parts static (the program needs no DLL of the toolchain), the
# entry wWinMain, the debug information kept apart, and the system's DLLs looked for in System32
# alone (no DLL next to the program is loaded in their place).
#
if(MIKUTERM_MSVC_STYLE)
	set_property(TARGET mikuterm PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	target_link_options(mikuterm PRIVATE /DEPENDENTLOADFLAG:0x800)
else()
	target_compile_definitions(mikuterm PRIVATE MIKUTERM_MANIFEST_RESOURCE)
	target_link_options(mikuterm PRIVATE -static -municode -Wl,--dependent-load-flag=0x800)
endif()

# TRE regular expressions for vi, with the Windows UTF-8 mbtowc shim.
set(MIKUTERM_VI_TRE_SOURCES
	${MIKUTERM_ROOT}/src/libc/regex/regcomp.c
	${MIKUTERM_ROOT}/src/libc/regex/regerror.c
	${MIKUTERM_ROOT}/src/libc/regex/regexec.c
	${MIKUTERM_ROOT}/src/libc/regex/tre-mem.c
	${MIKUTERM_DIR}/vi-tre-win32.c)
add_library(vi_tre STATIC ${MIKUTERM_VI_TRE_SOURCES})
target_include_directories(vi_tre PRIVATE ${MIKUTERM_ROOT} ${MIKUTERM_VI_INCLUDE} ${MIKUTERM_DIR})
target_compile_definitions(vi_tre PRIVATE VI_TRE_BUILD _WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE)
if(MIKUTERM_MSVC_STYLE)
	target_compile_options(vi_tre PRIVATE /std:c11 /W3 /utf-8 /FI${MIKUTERM_DIR}/vi-tre-win32.h)
	target_compile_definitions(vi_tre PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
	if(MIKUTERM_WERROR)
		target_compile_options(vi_tre PRIVATE /WX)
	endif()
else()
	target_compile_options(vi_tre PRIVATE
		-std=gnu11 -Wall -Wextra -Wno-unknown-warning-option -Wno-maybe-uninitialized -Wno-pointer-to-int-cast
		-include ${MIKUTERM_DIR}/vi-tre-win32.h)
	if(MIKUTERM_WERROR)
		target_compile_options(vi_tre PRIVATE -Werror)
	endif()
endif()

# The embedded vi editor (runs standalone in Windows Console or as mikuterm's child over pipes).
set(MIKUTERM_VI_SOURCES
	${MIKUTERM_ROOT}/userland/base/vi/main.c
	${MIKUTERM_ROOT}/userland/base/vi/command.c
	${MIKUTERM_ROOT}/userland/base/vi/ex.c
	${MIKUTERM_ROOT}/userland/base/vi/search.c
	${MIKUTERM_ROOT}/userland/base/vi/screen.c
	${MIKUTERM_ROOT}/userland/base/vi/terminal.c
	${MIKUTERM_ROOT}/userland/base/vi/buffer.c
	${MIKUTERM_ROOT}/userland/base/vi/undo.c
	${MIKUTERM_ROOT}/userland/base/vi/file.c
	${MIKUTERM_ROOT}/userland/base/common/terminfo.c
	${MIKUTERM_ROOT}/userland/desktop/terminal/width.c
	${MIKUTERM_DIR}/vi-terminal-win32.c
	${MIKUTERM_DIR}/vi-file-win32.c)
add_executable(vi ${MIKUTERM_VI_SOURCES})
target_include_directories(vi PRIVATE ${MIKUTERM_ROOT} ${MIKUTERM_VI_INCLUDE})
target_link_libraries(vi PRIVATE mikuterm_flags vi_tre)
if(MIKUTERM_MSVC_STYLE)
	set_property(TARGET vi PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	target_link_options(vi PRIVATE /DEPENDENTLOADFLAG:0x800)
else()
	target_link_options(vi PRIVATE -static -Wl,--dependent-load-flag=0x800)
endif()

# The fonts of the zip's share\fonts (the names libkeiland and the terminal look for) and their licenses.
set(MIKUTERM_SHARE
	"fonts/keiland.ttf=${MIKUTERM_ROOT}/userland/desktop/fonts/Mahora-Regular.ttf"
	"fonts/keiland-bold.ttf=${MIKUTERM_ROOT}/userland/desktop/fonts/Mahora-Bold.ttf"
	"fonts/keiland-mono.ttf=${MIKUTERM_ROOT}/userland/desktop/fonts/Mahora-Mono.ttf"
	"fonts/keiland-fallback-mono.ttf=${MIKUTERM_ROOT}/userland/desktop/fonts/JetBrainsMono-Regular.ttf"
	"fonts/keiland-fallback.ttf=${MIKUTERM_ROOT}/userland/desktop/fonts/DroidSansFallbackFull.ttf"
	"licenses/README.txt=${MIKUTERM_DIR}/files/licenses-README.txt"
	"licenses/zedBSD-LICENSE.txt=${MIKUTERM_ROOT}/LICENSE"
	"licenses/Mahora-LICENSE.txt=${MIKUTERM_ROOT}/userland/desktop/fonts/Mahora-LICENSE.txt"
	"licenses/JetBrainsMono-OFL.txt=${MIKUTERM_ROOT}/userland/desktop/fonts/JetBrainsMono-OFL.txt"
	"licenses/DroidSansFallback-LICENSE.txt=${MIKUTERM_ROOT}/userland/desktop/fonts/DroidSansFallback-LICENSE.txt"
	"licenses/NotoColorEmoji-OFL-1.1.txt=${MIKUTERM_ROOT}/userland/packages/fonts/noto-color-emoji/OFL-1.1.txt"
	"licenses/Unifont-OFL-1.1.txt=${MIKUTERM_ROOT}/userland/packages/fonts/unifont/OFL-1.1.txt"
	"licenses/Unicode-License-V3.txt=${MIKUTERM_ROOT}/userland/base/licenses/browser/Unicode-License-V3"
	"licenses/TRE-LICENSE.txt=${MIKUTERM_ROOT}/src/libc/regex/LICENSE")

# The folder's own files: the guide, the samples a user edits, and where a user puts fonts.
set(MIKUTERM_FOLDER_FILES
	"README.txt=${MIKUTERM_DIR}/files/README.txt"
	"config/mikuterm.conf=${MIKUTERM_DIR}/files/config/mikuterm.conf"
	"ssh/config=${MIKUTERM_DIR}/files/ssh/config"
	"fonts/README.txt=${MIKUTERM_DIR}/files/fonts/README.txt")

# Noto Color Emoji, as its package records it (version, address, SHA-256).
set(MIKUTERM_NOTO_PACKAGE ${MIKUTERM_ROOT}/userland/packages/fonts/noto-color-emoji/Makefile)
file(STRINGS ${MIKUTERM_NOTO_PACKAGE} noto_archive REGEX "^EXT_noto-color-emoji_ARCHIVE := ")
file(STRINGS ${MIKUTERM_NOTO_PACKAGE} noto_url REGEX "^EXT_noto-color-emoji_URL := ")
file(STRINGS ${MIKUTERM_NOTO_PACKAGE} noto_sha256 REGEX "^EXT_noto-color-emoji_SHA256 := ")
string(REGEX REPLACE "^[^=]*= " "" MIKUTERM_NOTO_ARCHIVE "${noto_archive}")
string(REGEX REPLACE "^[^=]*= " "" MIKUTERM_NOTO_URL "${noto_url}")
string(REGEX REPLACE "^[^=]*= " "" MIKUTERM_NOTO_SHA256 "${noto_sha256}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MIKUTERM_NOTO_PACKAGE})
set(MIKUTERM_NOTO_FILE ${CMAKE_CURRENT_BINARY_DIR}/sources/${MIKUTERM_NOTO_ARCHIVE})
add_custom_command(OUTPUT ${MIKUTERM_NOTO_FILE}
	COMMAND ${CMAKE_COMMAND}
		-DURL=${MIKUTERM_NOTO_URL}
		-DSHA256=${MIKUTERM_NOTO_SHA256}
		-DLOCAL=${MIKUTERM_DISTFILES}/${MIKUTERM_NOTO_ARCHIVE}
		-DOUTPUT=${MIKUTERM_NOTO_FILE}
		-P ${MIKUTERM_DIR}/cmake/fetch.cmake
	DEPENDS ${MIKUTERM_NOTO_PACKAGE} ${MIKUTERM_DIR}/cmake/fetch.cmake
	COMMENT "Noto Color Emoji, checked against its SHA-256"
	VERBATIM)

# GNU Unifont, as its package records it (version, address, SHA-256).
set(MIKUTERM_UNIFONT_PACKAGE ${MIKUTERM_ROOT}/userland/packages/fonts/unifont/Makefile)
file(STRINGS ${MIKUTERM_UNIFONT_PACKAGE} unifont_archive REGEX "^EXT_unifont_ARCHIVE := ")
file(STRINGS ${MIKUTERM_UNIFONT_PACKAGE} unifont_url REGEX "^EXT_unifont_URL := ")
file(STRINGS ${MIKUTERM_UNIFONT_PACKAGE} unifont_sha256 REGEX "^EXT_unifont_SHA256 := ")
string(REGEX REPLACE "^[^=]*= " "" MIKUTERM_UNIFONT_ARCHIVE "${unifont_archive}")
string(REGEX REPLACE "^[^=]*= " "" MIKUTERM_UNIFONT_URL "${unifont_url}")
string(REGEX REPLACE "^[^=]*= " "" MIKUTERM_UNIFONT_SHA256 "${unifont_sha256}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MIKUTERM_UNIFONT_PACKAGE})
set(MIKUTERM_UNIFONT_FILE ${CMAKE_CURRENT_BINARY_DIR}/sources/${MIKUTERM_UNIFONT_ARCHIVE})
add_custom_command(OUTPUT ${MIKUTERM_UNIFONT_FILE}
	COMMAND ${CMAKE_COMMAND}
		-DURL=${MIKUTERM_UNIFONT_URL}
		-DSHA256=${MIKUTERM_UNIFONT_SHA256}
		-DLOCAL=${MIKUTERM_DISTFILES}/${MIKUTERM_UNIFONT_ARCHIVE}
		-DOUTPUT=${MIKUTERM_UNIFONT_FILE}
		-P ${MIKUTERM_DIR}/cmake/fetch.cmake
	DEPENDS ${MIKUTERM_UNIFONT_PACKAGE} ${MIKUTERM_DIR}/cmake/fetch.cmake
	COMMENT "GNU Unifont, checked against its SHA-256"
	VERBATIM)

#
# The folder the zip holds (mikuterm\ in the build directory): the program without its debug
# information (kept in mikuterm.dbg beside it, or the linker's .pdb), the folder's files, the
# fonts and the licenses.
#
set(MIKUTERM_DIST ${CMAKE_CURRENT_BINARY_DIR}/mikuterm)
set(MIKUTERM_DIST_OUTPUTS ${MIKUTERM_DIST}/mikuterm.exe ${MIKUTERM_DIST}/vi.exe)
if(MIKUTERM_MSVC_STYLE)
	add_custom_command(OUTPUT ${MIKUTERM_DIST}/mikuterm.exe
		COMMAND ${CMAKE_COMMAND} -E make_directory ${MIKUTERM_DIST}
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:mikuterm> ${MIKUTERM_DIST}/mikuterm.exe
		DEPENDS mikuterm
		VERBATIM)
	add_custom_command(OUTPUT ${MIKUTERM_DIST}/vi.exe
		COMMAND ${CMAKE_COMMAND} -E make_directory ${MIKUTERM_DIST}
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:vi> ${MIKUTERM_DIST}/vi.exe
		DEPENDS vi
		VERBATIM)
else()
	add_custom_command(OUTPUT ${MIKUTERM_DIST}/mikuterm.exe
		COMMAND ${CMAKE_COMMAND} -E make_directory ${MIKUTERM_DIST}
		COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE:mikuterm> ${CMAKE_CURRENT_BINARY_DIR}/mikuterm.dbg
		COMMAND ${CMAKE_OBJCOPY} --strip-debug --add-gnu-debuglink=${CMAKE_CURRENT_BINARY_DIR}/mikuterm.dbg
			$<TARGET_FILE:mikuterm> ${MIKUTERM_DIST}/mikuterm.exe
		DEPENDS mikuterm
		VERBATIM)
	add_custom_command(OUTPUT ${MIKUTERM_DIST}/vi.exe
		COMMAND ${CMAKE_COMMAND} -E make_directory ${MIKUTERM_DIST}
		COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE:vi> ${CMAKE_CURRENT_BINARY_DIR}/vi.dbg
		COMMAND ${CMAKE_OBJCOPY} --strip-debug --add-gnu-debuglink=${CMAKE_CURRENT_BINARY_DIR}/vi.dbg
			$<TARGET_FILE:vi> ${MIKUTERM_DIST}/vi.exe
		DEPENDS vi
		VERBATIM)
endif()
foreach(pair IN LISTS MIKUTERM_SHARE MIKUTERM_FOLDER_FILES)
	string(REPLACE "=" ";" parts "${pair}")
	list(GET parts 0 name)
	list(GET parts 1 from)
	if(pair IN_LIST MIKUTERM_SHARE)
		set(name share/${name})
	endif()
	add_custom_command(OUTPUT ${MIKUTERM_DIST}/${name}
		COMMAND ${CMAKE_COMMAND} -E copy ${from} ${MIKUTERM_DIST}/${name}
		DEPENDS ${from}
		VERBATIM)
	list(APPEND MIKUTERM_DIST_OUTPUTS ${MIKUTERM_DIST}/${name})
endforeach()
add_custom_command(OUTPUT ${MIKUTERM_DIST}/share/fonts/keiland-emoji.ttf
	COMMAND ${CMAKE_COMMAND} -E copy ${MIKUTERM_NOTO_FILE} ${MIKUTERM_DIST}/share/fonts/keiland-emoji.ttf
	DEPENDS ${MIKUTERM_NOTO_FILE}
	VERBATIM)
list(APPEND MIKUTERM_DIST_OUTPUTS ${MIKUTERM_DIST}/share/fonts/keiland-emoji.ttf)
add_custom_command(OUTPUT ${MIKUTERM_DIST}/share/fonts/unifont.ttf
	COMMAND ${CMAKE_COMMAND} -E copy ${MIKUTERM_UNIFONT_FILE} ${MIKUTERM_DIST}/share/fonts/unifont.ttf
	DEPENDS ${MIKUTERM_UNIFONT_FILE}
	VERBATIM)
list(APPEND MIKUTERM_DIST_OUTPUTS ${MIKUTERM_DIST}/share/fonts/unifont.ttf)
# Optional pre-staged Virtual-Display-Driver package (vdd\MttVDD.inf, .cat, .dll, vdd_settings.xml, LICENSE).
set(MIKUTERM_VDD_DIR "" CACHE PATH "Optional directory containing signed Virtual-Display-Driver files")
if(NOT MIKUTERM_VDD_DIR)
	if(EXISTS "${MIKUTERM_DIR}/vdd/MttVDD.inf")
		set(MIKUTERM_VDD_DIR "${MIKUTERM_DIR}/vdd")
	elseif(EXISTS "${MIKUTERM_DISTFILES}/vdd/MttVDD.inf")
		set(MIKUTERM_VDD_DIR "${MIKUTERM_DISTFILES}/vdd")
	endif()
endif()
if(MIKUTERM_VDD_DIR AND EXISTS "${MIKUTERM_VDD_DIR}/MttVDD.inf")
	file(GLOB MIKUTERM_VDD_FILES CONFIGURE_DEPENDS "${MIKUTERM_VDD_DIR}/*")
	foreach(vdd_file IN LISTS MIKUTERM_VDD_FILES)
		if(NOT IS_DIRECTORY "${vdd_file}")
			get_filename_component(vdd_name "${vdd_file}" NAME)
			add_custom_command(OUTPUT ${MIKUTERM_DIST}/vdd/${vdd_name}
				COMMAND ${CMAKE_COMMAND} -E make_directory ${MIKUTERM_DIST}/vdd
				COMMAND ${CMAKE_COMMAND} -E copy ${vdd_file} ${MIKUTERM_DIST}/vdd/${vdd_name}
				DEPENDS ${vdd_file}
				VERBATIM)
			list(APPEND MIKUTERM_DIST_OUTPUTS ${MIKUTERM_DIST}/vdd/${vdd_name})
		endif()
	endforeach()
endif()

add_custom_target(dist DEPENDS ${MIKUTERM_DIST_OUTPUTS})

# The zip of the folder, its entries in a fixed order and time (the same files make the same zip).
find_package(Python3 COMPONENTS Interpreter)
set(MIKUTERM_ZIP ${CMAKE_CURRENT_BINARY_DIR}/mikuterm-${PROJECT_VERSION}-${MIKUTERM_ZIP_ARCH}.zip)
if(Python3_Interpreter_FOUND)
	add_custom_command(OUTPUT ${MIKUTERM_ZIP}
		COMMAND ${Python3_EXECUTABLE} -I ${MIKUTERM_ROOT}/apps/browser-win32/tools/make-zip.py ${MIKUTERM_DIST} ${MIKUTERM_ZIP}
		DEPENDS ${MIKUTERM_DIST_OUTPUTS} ${MIKUTERM_ROOT}/apps/browser-win32/tools/make-zip.py
		VERBATIM)
	add_custom_target(zip DEPENDS ${MIKUTERM_ZIP})
endif()
