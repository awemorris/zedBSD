# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# mikuterm (Micro Unicode Terminal) for Linux: zedBSD's terminal (userland/desktop/terminal) on
# libkeiland's Wayland backend, with zedBSD's own SSH client engine (userland/base/ssh) for each
# tab, as one program for a Wayland session (GNOME, KDE Plasma, Sway, Weston).
#
# The build is its own: it reads no Makefile of the tree but the source lists the libraries keep
# (sources.mk), and names the rest here.  mikuterm's files are the Windows program's
# (apps/terminal-win32) but for the ones that speak to the system, which are this directory's.
#
#   cmake -S apps/terminal-linux -B <build>
#   cmake --build <build> -j4
#   <build>/bin/mikuterm                      (runs from the build directory)
#   cmake --install <build> --prefix <prefix> (bin/mikuterm, share/mikuterm, and the desktop's
#                                              entry, icons and metainfo of resources/freedesktop)
#   -DMIKUTERM_FLATPAK=ON                     (Flatpak's build: no login shell, ProxyCommand or gh)
#
# The frames are painted on the processor and shown by libkeiland in shared memory.  libkeiland
# keeps its Vulkan surface, so the build reads Vulkan's headers (MIKUTERM_VULKAN_INCLUDE, or the
# release's tarball: a copy in MIKUTERM_DISTFILES first, else fetched, checked against its
# SHA-256) and links the system's Vulkan loader (libvulkan.so.1).  The Wayland client library is
# the tree's (userland/desktop/libwayland), linked in.  The fonts are the tree's, and Noto Color
# Emoji, fetched and checked against the SHA-256 its package records (a copy in
# MIKUTERM_DISTFILES is used first); nothing fetched goes into the tree.
#
# The user's settings, hosts and keys are in $XDG_CONFIG_HOME/mikuterm (~/.config/mikuterm), in
# the Windows program's folder's form: config/mikuterm.conf, ssh/config, ssh/known_hosts,
# fonts/.  The fonts mikuterm brings are in share/mikuterm beside the program's bin.
#

cmake_minimum_required(VERSION 3.20)
project(mikuterm VERSION 0.1.0 LANGUAGES C)

get_filename_component(MIKUTERM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(MIKUTERM_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(MIKUTERM_WIN32_DIR ${MIKUTERM_ROOT}/apps/terminal-win32)

# The application's ID: the Wayland app_id, the desktop entry's and the metainfo's name.
set(MIKUTERM_APP_ID io.noctvm.mikuterm)

set(MIKUTERM_VULKAN_INCLUDE "" CACHE PATH "A directory holding vulkan/vulkan.h (empty: fetch the headers)")
set(MIKUTERM_DISTFILES "${MIKUTERM_ROOT}/build/distfiles" CACHE PATH "Where downloaded files may already be")
option(MIKUTERM_WERROR "Make every warning an error" ON)

#
# Flatpak's build: mikuterm in the sandbox runs no program of the computer's (a login shell, a
# ProxyCommand's program, GitHub's gh), only its SSH connections and its own X server.  ON sets the
# features below to 0.
#
option(MIKUTERM_FLATPAK "Build for Flatpak: no login shell, ProxyCommand or GitHub Codespaces" OFF)
if(MIKUTERM_FLATPAK)
	set(MIKUTERM_LOCAL_PROGRAMS 0)
else()
	set(MIKUTERM_LOCAL_PROGRAMS 1)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "The kind of build" FORCE)
endif()

# The lists mikuterm's builds share, and the reader of the tree's sources.mk lists.
include(${MIKUTERM_ROOT}/apps/mikuterm-common/cmake/mikuterm-sources.cmake)

mikuterm_read_sources(MIKUTERM_TRUETYPE_SOURCES userland/desktop/libtruetype/sources.mk LIBTRUETYPE_SOURCES)
mikuterm_read_sources(MIKUTERM_PNG_SOURCES userland/desktop/libpng-compat/sources.mk LIBPNG_COMPAT_SOURCES)
mikuterm_read_sources(MIKUTERM_Z_SOURCES userland/base/libz-compat/sources.mk LIBZ_COMPAT_SOURCES)
mikuterm_read_sources(MIKUTERM_CRYPTO_SOURCES userland/base/libcrypto-compat/sources.mk LIBCRYPTO_COMPAT_SOURCES)
mikuterm_read_sources(MIKUTERM_CRYPTO_OS_SOURCES userland/base/libcrypto-compat/sources.mk LIBCRYPTO_COMPAT_POSIX_SOURCES)
mikuterm_read_sources(MIKUTERM_SSL_SOURCES userland/base/libssl-compat/sources.mk LIBSSL_COMPAT_SOURCES)
mikuterm_read_sources(MIKUTERM_RDP_SOURCES userland/desktop/librdp/sources.mk LIBRDP_SOURCES)
mikuterm_read_sources(MIKUTERM_MEDIA_ALL_SOURCES userland/desktop/libmedia/sources.mk LIBMEDIA_SOURCES)
set(MIKUTERM_H264SW_SOURCES "")
foreach(src IN LISTS MIKUTERM_MEDIA_ALL_SOURCES)
	if(src MATCHES "/(h264sw-|vcodec-bitreader|vcodec-nal|vcodec-cabac|bits\\.c)" AND NOT src MATCHES "/(h264sw-backend|h264sw-enc)\\.c$")
		list(APPEND MIKUTERM_H264SW_SOURCES ${src})
	endif()
endforeach()
list(APPEND MIKUTERM_H264SW_SOURCES ${MIKUTERM_ROOT}/userland/desktop/libmedia/media-os-posix.c)
mikuterm_read_sources(MIKUTERM_KEILAND_SOURCES userland/desktop/libkeiland/sources.mk LIBKEILAND_SOURCES)

# The tree's Wayland client library and the protocols libkeiland speaks.
set(MIKUTERM_WAYLAND_NAMES
	client proxy wire event protocol utility menu-protocol titlebar-protocol glass-protocol
	subsurface-protocol data-device-protocol primary-selection-protocol tablet-protocol
	touch-protocol text-input-protocol input-method-protocol virtual-keyboard-protocol
	ime-status-protocol keyboard-inset-protocol edit-protocol activation-protocol
	content-type-protocol theme-protocol fractional-scale-protocol viewporter-protocol)
list(TRANSFORM MIKUTERM_WAYLAND_NAMES PREPEND ${MIKUTERM_ROOT}/userland/desktop/libwayland/ OUTPUT_VARIABLE MIKUTERM_WAYLAND_SOURCES)
list(TRANSFORM MIKUTERM_WAYLAND_SOURCES APPEND .c)

# mikuterm's own files: this directory's that speak to the system, and the ones every build shares
# (apps/mikuterm-common: this build has X11 forwarding and no WSL).
set(MIKUTERM_SOURCES
	${MIKUTERM_DIR}/main.c
	${MIKUTERM_DIR}/folder.c
	${MIKUTERM_DIR}/credential.c
	${MIKUTERM_DIR}/x11-spawn-posix.c
	${MIKUTERM_DIR}/x11-child.c
	${MIKUTERM_COMMON_SOURCES}
	${MIKUTERM_COMMON_RDP_SOURCES}
	${MIKUTERM_COMMON_X11_SOURCES}
	${MIKUTERM_COMMON_NO_WSL_SOURCES})

# The login shell, ProxyCommand and GitHub Codespaces (gh): the files that run the computer's programs.
if(MIKUTERM_LOCAL_PROGRAMS)
	list(APPEND MIKUTERM_SOURCES
		${MIKUTERM_DIR}/login-shell-posix.c
		${MIKUTERM_DIR}/program-posix.c
		${MIKUTERM_DIR}/proxy-posix.c
		${MIKUTERM_COMMON_PROGRAM_SOURCES})
endif()

#
# The X server of X11 forwarding (mikuterm --x11-server): the core of zedBSD's X server (without
# GLX: x11-noglx.c), and the backend its windows are shown with, the Wayland desktop's.  The tabs'
# scale is the Windows backend's arithmetic (pure).
#
set(MIKUTERM_X11_NAMES
	client glyphs keymap protocol rootless selection window resource event property region
	render-cpu render-vulkan present-vulkan gc draw shape colormap colornames input wm font cursor
	xkb xinput rooted extension shape-ext xfixes sync render-ext render-composite coverage randr
	xinerama wayland win32/scale)
list(TRANSFORM MIKUTERM_X11_NAMES PREPEND ${MIKUTERM_ROOT}/userland/desktop/xserver/ OUTPUT_VARIABLE MIKUTERM_X11_SOURCES)
list(TRANSFORM MIKUTERM_X11_SOURCES APPEND .c)

# The compat headers (<compat/zlib/zlib.h> and the others), copied from zedBSD's C library side so
# that the rest of that side is not on the include path (it would hide the C library's).
set(MIKUTERM_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
file(GLOB_RECURSE MIKUTERM_COMPAT_HEADERS CONFIGURE_DEPENDS RELATIVE ${MIKUTERM_ROOT}/include/libc
	${MIKUTERM_ROOT}/include/libc/compat/*.h)
foreach(header IN LISTS MIKUTERM_COMPAT_HEADERS)
	configure_file(${MIKUTERM_ROOT}/include/libc/${header} ${MIKUTERM_INCLUDE}/${header} COPYONLY)
endforeach()

# The version, for Help > About.
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

# The system's Vulkan loader (libvulkan1; the development link libvulkan.so is not needed).
find_library(MIKUTERM_VULKAN_LIBRARY NAMES vulkan libvulkan.so.1 REQUIRED)
find_package(Threads REQUIRED)

#
# The flags every C file of the build is compiled with: GNU's and POSIX's interfaces, the tree's
# paths from its top, the desktop's headers (the tree's Wayland headers before the system's), the
# copied compat headers and Vulkan's; where the program's data is installed; C11 with GNU's
# extensions.
#
include(GNUInstallDirs)
add_library(mikuterm_flags INTERFACE)
target_include_directories(mikuterm_flags INTERFACE
	${MIKUTERM_ROOT}
	${MIKUTERM_ROOT}/userland/desktop/include
	${MIKUTERM_INCLUDE}
	${MIKUTERM_VULKAN_INCLUDE})
target_compile_definitions(mikuterm_flags INTERFACE
	_GNU_SOURCE
	KEILAND_BINDIR="${CMAKE_INSTALL_FULL_BINDIR}"
	KEILAND_LIBEXECDIR="${CMAKE_INSTALL_FULL_LIBEXECDIR}"
	KEILAND_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/mikuterm"
	KEILAND_SYSCONFDIR="${CMAKE_INSTALL_FULL_SYSCONFDIR}")
# -Wno-format-truncation: GCC's guess that a bounded snprintf may cut its text (the texts are cut on purpose).
target_compile_options(mikuterm_flags INTERFACE
	$<$<COMPILE_LANGUAGE:C>:-std=gnu11 -Wall -Wextra -Wno-format-truncation>)
if(MIKUTERM_WERROR)
	target_compile_options(mikuterm_flags INTERFACE $<$<COMPILE_LANGUAGE:C>:-Werror>)
endif()

# The libraries, each a static library of the tree's files.
add_library(mikuterm_z STATIC ${MIKUTERM_Z_SOURCES})
add_library(mikuterm_png STATIC ${MIKUTERM_PNG_SOURCES})
add_library(mikuterm_truetype STATIC ${MIKUTERM_TRUETYPE_SOURCES})
add_library(mikuterm_crypto STATIC ${MIKUTERM_CRYPTO_SOURCES} ${MIKUTERM_CRYPTO_OS_SOURCES})
add_library(mikuterm_ssl STATIC ${MIKUTERM_SSL_SOURCES})
add_library(mikuterm_rdp STATIC ${MIKUTERM_RDP_SOURCES})
add_library(mikuterm_h264sw STATIC ${MIKUTERM_H264SW_SOURCES})
add_library(mikuterm_ssh STATIC ${MIKUTERM_SSH_SOURCES})
add_library(mikuterm_wayland STATIC ${MIKUTERM_WAYLAND_SOURCES})
add_library(mikuterm_keiland STATIC ${MIKUTERM_KEILAND_SOURCES})
add_library(mikuterm_terminal STATIC ${MIKUTERM_TERMINAL_SOURCES})
add_library(mikuterm_xserver STATIC ${MIKUTERM_X11_SOURCES})
foreach(library z png truetype crypto ssl rdp h264sw ssh wayland keiland terminal xserver)
	target_link_libraries(mikuterm_${library} PRIVATE mikuterm_flags)
endforeach()
target_include_directories(mikuterm_h264sw PRIVATE ${MIKUTERM_ROOT}/userland/desktop/libmedia)
target_link_libraries(mikuterm_png PUBLIC mikuterm_z)
target_link_libraries(mikuterm_ssl PUBLIC mikuterm_crypto)
target_link_libraries(mikuterm_ssh PUBLIC mikuterm_crypto)
target_link_libraries(mikuterm_keiland PUBLIC mikuterm_truetype mikuterm_png mikuterm_wayland
	${MIKUTERM_VULKAN_LIBRARY} Threads::Threads ${CMAKE_DL_LIBS} m)
target_link_libraries(mikuterm_terminal PUBLIC mikuterm_keiland mikuterm_ssh)
target_link_libraries(mikuterm_xserver PUBLIC mikuterm_keiland)

#
# The program, in bin/ of the build directory with its data in share/mikuterm/ beside it, as it is
# installed.  The features (1 or 0): no WSL row on the launch screen, the settings' opener of files
# and X11 forwarding, and the launch screen's Login Shell, ssh_config's ProxyCommand and GitHub
# Codespaces.
#
add_executable(mikuterm ${MIKUTERM_SOURCES})
target_link_libraries(mikuterm PRIVATE mikuterm_flags mikuterm_terminal mikuterm_ssh mikuterm_rdp mikuterm_ssl mikuterm_h264sw mikuterm_xserver)
target_include_directories(mikuterm PRIVATE ${MIKUTERM_COMMON_DIR})
# Windows' _stricmp is the C library's strcasecmp (<strings.h>, which <string.h> brings with _GNU_SOURCE).
target_compile_definitions(mikuterm PRIVATE MIKUTERM_HAVE_WSL=0 MIKUTERM_HAVE_OPENER=1 MIKUTERM_HAVE_X11=1 MIKUTERM_HAVE_KEY_PASTE=0 MIKUTERM_APP_ID="${MIKUTERM_APP_ID}" _stricmp=strcasecmp
	MIKUTERM_HAVE_LOGIN_SHELL=${MIKUTERM_LOCAL_PROGRAMS}
	MIKUTERM_HAVE_PROXY_COMMAND=${MIKUTERM_LOCAL_PROGRAMS}
	MIKUTERM_HAVE_CODESPACES=${MIKUTERM_LOCAL_PROGRAMS})
set_target_properties(mikuterm PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
install(TARGETS mikuterm RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The fonts of share/mikuterm/fonts (the names libkeiland and the terminal look for), their licenses and the samples.
set(MIKUTERM_SHARE
	"fonts/keiland.ttf=${MIKUTERM_ROOT}/userland/desktop/fonts/Mahora-Regular.ttf"
	"fonts/keiland-bold.ttf=${MIKUTERM_ROOT}/userland/desktop/fonts/Mahora-Bold.ttf"
	"fonts/keiland-mono.ttf=${MIKUTERM_ROOT}/userland/desktop/fonts/Mahora-Mono.ttf"
	"fonts/keiland-fallback-mono.ttf=${MIKUTERM_ROOT}/userland/desktop/fonts/JetBrainsMono-Regular.ttf"
	"fonts/keiland-fallback.ttf=${MIKUTERM_ROOT}/userland/desktop/fonts/DroidSansFallbackFull.ttf"
	"licenses/README.txt=${MIKUTERM_WIN32_DIR}/files/licenses-README.txt"
	"licenses/zedBSD-LICENSE.txt=${MIKUTERM_ROOT}/LICENSE"
	"licenses/Mahora-LICENSE.txt=${MIKUTERM_ROOT}/userland/desktop/fonts/Mahora-LICENSE.txt"
	"licenses/JetBrainsMono-OFL.txt=${MIKUTERM_ROOT}/userland/desktop/fonts/JetBrainsMono-OFL.txt"
	"licenses/DroidSansFallback-LICENSE.txt=${MIKUTERM_ROOT}/userland/desktop/fonts/DroidSansFallback-LICENSE.txt"
	"licenses/NotoColorEmoji-OFL-1.1.txt=${MIKUTERM_ROOT}/userland/packages/fonts/noto-color-emoji/OFL-1.1.txt"
	"licenses/Unifont-OFL-1.1.txt=${MIKUTERM_ROOT}/userland/packages/fonts/unifont/OFL-1.1.txt"
	"licenses/Unicode-License-V3.txt=${MIKUTERM_ROOT}/userland/base/licenses/browser/Unicode-License-V3"
	"examples/mikuterm.conf=${MIKUTERM_WIN32_DIR}/files/config/mikuterm.conf"
	"examples/ssh-config=${MIKUTERM_WIN32_DIR}/files/ssh/config")

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
		-P ${MIKUTERM_WIN32_DIR}/cmake/fetch.cmake
	DEPENDS ${MIKUTERM_NOTO_PACKAGE} ${MIKUTERM_WIN32_DIR}/cmake/fetch.cmake
	COMMENT "Noto Color Emoji, checked against its SHA-256"
	VERBATIM)
list(APPEND MIKUTERM_SHARE "fonts/keiland-emoji.ttf=${MIKUTERM_NOTO_FILE}")

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
		-P ${MIKUTERM_WIN32_DIR}/cmake/fetch.cmake
	DEPENDS ${MIKUTERM_UNIFONT_PACKAGE} ${MIKUTERM_WIN32_DIR}/cmake/fetch.cmake
	COMMENT "GNU Unifont, checked against its SHA-256"
	VERBATIM)
list(APPEND MIKUTERM_SHARE "fonts/unifont.ttf=${MIKUTERM_UNIFONT_FILE}")

# The data beside the program in the build directory (share/mikuterm), and installed the same way.
set(MIKUTERM_DATA ${CMAKE_CURRENT_BINARY_DIR}/share/mikuterm)
set(MIKUTERM_DATA_OUTPUTS)
foreach(pair IN LISTS MIKUTERM_SHARE)
	string(REPLACE "=" ";" parts "${pair}")
	list(GET parts 0 name)
	list(GET parts 1 from)
	add_custom_command(OUTPUT ${MIKUTERM_DATA}/${name}
		COMMAND ${CMAKE_COMMAND} -E copy ${from} ${MIKUTERM_DATA}/${name}
		DEPENDS ${from}
		VERBATIM)
	list(APPEND MIKUTERM_DATA_OUTPUTS ${MIKUTERM_DATA}/${name})
	get_filename_component(directory ${name} DIRECTORY)
	install(FILES ${MIKUTERM_DATA}/${name} DESTINATION ${CMAKE_INSTALL_DATADIR}/mikuterm/${directory})
endforeach()
add_custom_target(mikuterm_data ALL DEPENDS ${MIKUTERM_DATA_OUTPUTS})

# The desktop's entry, the metainfo the software centres read, and the icon at each size the Windows program's has.
install(FILES ${MIKUTERM_DIR}/resources/freedesktop/${MIKUTERM_APP_ID}.desktop
	DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${MIKUTERM_DIR}/resources/freedesktop/${MIKUTERM_APP_ID}.metainfo.xml
	DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
foreach(size 16 24 32 48 64 256 512)
	install(FILES ${MIKUTERM_WIN32_DIR}/resources/icon/mikuterm-${size}.png
		DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
		RENAME ${MIKUTERM_APP_ID}.png)
endforeach()
