# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# mikuterm's native library for one ABI.  It is configured with the NDK's toolchain file
# by the top of the build (apps/terminal-android), once for each ABI.  MIKUTERM_ROOT is
# the root of the tree.
#

cmake_minimum_required(VERSION 3.20)
project(mikuterm-native LANGUAGES C)

if(NOT MIKUTERM_ROOT)
	get_filename_component(MIKUTERM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
endif()
file(TO_CMAKE_PATH "${MIKUTERM_ROOT}" MIKUTERM_ROOT)

# The trial body of the native thread, which paints the window one colour (MIKUTERM_ANDROID_HELLO),
# and the connection that shows back what is typed (MIKUTERM_ANDROID_ECHO), for trying the terminal
# without a network.
option(MIKUTERM_ANDROID_HELLO "Build the trial body of the native thread" OFF)
option(MIKUTERM_ANDROID_ECHO "Open tabs with a connection that shows back what is typed" OFF)

# The lists mikuterm's builds share, and the reader of the tree's sources.mk lists.
include(${MIKUTERM_ROOT}/apps/mikuterm-common/cmake/mikuterm-sources.cmake)
include(${MIKUTERM_ROOT}/apps/libkeiland/cmake/keiland-sources.cmake)

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

# libkeiland: the files every system shares with the POSIX platform layer, and the Android layer.
set(MIKUTERM_KEILAND_COMMON ${KEILAND_COMMON_SOURCES} ${KEILAND_POSIX_SOURCES})
list(TRANSFORM MIKUTERM_KEILAND_COMMON PREPEND ${MIKUTERM_ROOT}/)
set(MIKUTERM_KEILAND_ANDROID ${KEILAND_ANDROID_SOURCES})
list(TRANSFORM MIKUTERM_KEILAND_ANDROID PREPEND ${MIKUTERM_ROOT}/)

# The compat headers (<compat/zlib/zlib.h> and the others), copied from zedBSD's C library side so
# that the rest of that side is not on the include path.
set(MIKUTERM_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
file(GLOB_RECURSE MIKUTERM_COMPAT_HEADERS CONFIGURE_DEPENDS RELATIVE ${MIKUTERM_ROOT}/include/libc
	${MIKUTERM_ROOT}/include/libc/compat/*.h)
foreach(header IN LISTS MIKUTERM_COMPAT_HEADERS)
	configure_file(${MIKUTERM_ROOT}/include/libc/${header} ${MIKUTERM_INCLUDE}/${header} COPYONLY)
endforeach()

# The version, for Help > About.
set(MIKUTERM_VERSION 0.1.0 CACHE STRING "The program's version, given by the top of the build")
string(REPLACE "." ";" version_parts ${MIKUTERM_VERSION})
list(GET version_parts 0 PROJECT_VERSION_MAJOR)
list(GET version_parts 1 PROJECT_VERSION_MINOR)
list(GET version_parts 2 PROJECT_VERSION_PATCH)
set(PROJECT_VERSION ${MIKUTERM_VERSION})
configure_file(${MIKUTERM_VERSION_IN} ${MIKUTERM_INCLUDE}/mikuterm-version.h @ONLY)

#
# The flags every C file of the build is compiled with: GNU's and POSIX's interfaces, the tree's
# paths from its top, the copied compat headers; C11 with GNU's extensions.
#
add_library(mikuterm_flags INTERFACE)
target_include_directories(mikuterm_flags INTERFACE
	${MIKUTERM_ROOT}
	${MIKUTERM_ROOT}/userland/desktop/include
	${MIKUTERM_INCLUDE})
target_compile_definitions(mikuterm_flags INTERFACE _GNU_SOURCE)
target_compile_options(mikuterm_flags INTERFACE
	$<$<COMPILE_LANGUAGE:C>:-std=gnu11 -Wall -Wextra -Werror -fvisibility=hidden>)

# The X server core (CPU rendering, no Wayland backend).
set(MIKUTERM_XSERVER_NAMES
	client glyphs keymap protocol rootless selection window resource event property region
	render-cpu render-vulkan present-vulkan gc draw shape colormap colornames input wm font cursor
	xkb xinput rooted extension shape-ext xfixes sync render-ext render-composite coverage randr
	xinerama win32/scale)
list(TRANSFORM MIKUTERM_XSERVER_NAMES PREPEND ${MIKUTERM_ROOT}/userland/desktop/xserver/ OUTPUT_VARIABLE MIKUTERM_XSERVER_SOURCES)
list(TRANSFORM MIKUTERM_XSERVER_SOURCES APPEND .c)

# The libraries, each a static library of the tree's files.
add_library(mikuterm_z STATIC ${MIKUTERM_Z_SOURCES})
add_library(mikuterm_png STATIC ${MIKUTERM_PNG_SOURCES})
add_library(mikuterm_truetype STATIC ${MIKUTERM_TRUETYPE_SOURCES})
add_library(mikuterm_crypto STATIC ${MIKUTERM_CRYPTO_SOURCES} ${MIKUTERM_CRYPTO_OS_SOURCES})
add_library(mikuterm_ssl STATIC ${MIKUTERM_SSL_SOURCES})
add_library(mikuterm_rdp STATIC ${MIKUTERM_RDP_SOURCES})
add_library(mikuterm_h264sw STATIC ${MIKUTERM_H264SW_SOURCES})
add_library(mikuterm_ssh STATIC ${MIKUTERM_SSH_SOURCES})
add_library(keiland_common STATIC ${MIKUTERM_KEILAND_COMMON})
add_library(keiland_android STATIC ${MIKUTERM_KEILAND_ANDROID})
add_library(mikuterm_terminal STATIC ${MIKUTERM_TERMINAL_SOURCES})
add_library(mikuterm_xserver STATIC ${MIKUTERM_XSERVER_SOURCES})
add_library(mikuterm_common STATIC ${MIKUTERM_COMMON_SOURCES} ${MIKUTERM_COMMON_SFTP_SOURCES} ${MIKUTERM_COMMON_NO_WSL_SOURCES}
	${MIKUTERM_COMMON_RDP_SOURCES} ${MIKUTERM_COMMON_X11_SOURCES} ${MIKUTERM_COMMON_X11_THREAD_SOURCES})
foreach(library mikuterm_z mikuterm_png mikuterm_truetype mikuterm_crypto mikuterm_ssl mikuterm_rdp mikuterm_h264sw
	mikuterm_ssh keiland_common keiland_android mikuterm_terminal mikuterm_xserver mikuterm_common)
	target_link_libraries(${library} PRIVATE mikuterm_flags)
endforeach()
target_include_directories(mikuterm_h264sw PRIVATE ${MIKUTERM_ROOT}/userland/desktop/libmedia)
target_include_directories(keiland_android PRIVATE ${MIKUTERM_ROOT}/apps/libkeiland/android)
target_link_libraries(mikuterm_png PUBLIC mikuterm_z)
target_link_libraries(mikuterm_ssl PUBLIC mikuterm_crypto)
target_link_libraries(mikuterm_ssh PUBLIC mikuterm_crypto)
target_link_libraries(keiland_common PUBLIC mikuterm_truetype mikuterm_png)
target_link_libraries(keiland_android PUBLIC keiland_common)
target_link_libraries(mikuterm_terminal PUBLIC keiland_android mikuterm_ssh)
target_link_libraries(mikuterm_xserver PUBLIC keiland_common)

# The files every mikuterm shares (the settings, the connections, the launch screen and the settings' tab).  This
# build has no WSL, X11 forwarding in a thread and no local programs; Windows' _stricmp is the C library's strcasecmp.
target_include_directories(mikuterm_common PRIVATE ${MIKUTERM_COMMON_DIR})
target_compile_definitions(mikuterm_common PRIVATE MIKUTERM_HAVE_WSL=0 MIKUTERM_HAVE_OPENER=0 MIKUTERM_HAVE_X11=1 MIKUTERM_HAVE_KEY_PASTE=1 MIKUTERM_HAVE_SFTP_DRAG=0
	_stricmp=strcasecmp)
target_link_libraries(mikuterm_common PUBLIC mikuterm_terminal mikuterm_xserver)

set(MIKUTERM_ANDROID_SOURCES ${MIKUTERM_ROOT}/apps/terminal-android/jni/entry.c)
if(MIKUTERM_ANDROID_HELLO)
	list(APPEND MIKUTERM_ANDROID_SOURCES ${MIKUTERM_ROOT}/apps/terminal-android/jni/hello.c)
	set(MIKUTERM_ANDROID_HELLO_VALUE 1)
else()
	list(APPEND MIKUTERM_ANDROID_SOURCES
		${MIKUTERM_ROOT}/apps/terminal-android/jni/mikuterm-jni.c
		${MIKUTERM_ROOT}/apps/terminal-android/jni/mikuterm-android.c
		${MIKUTERM_ROOT}/apps/terminal-android/jni/places-android.c
		${MIKUTERM_ROOT}/apps/terminal-android/jni/credential-android.c
		${MIKUTERM_ROOT}/apps/terminal-android/jni/sftp-local-android.c
		${MIKUTERM_ROOT}/apps/terminal-android/native/rdp-mediacodec.c)
	set(MIKUTERM_ANDROID_HELLO_VALUE 0)
endif()
if(MIKUTERM_ANDROID_ECHO)
	list(APPEND MIKUTERM_ANDROID_SOURCES ${MIKUTERM_ROOT}/apps/terminal-android/jni/echo-connection.c)
	set(MIKUTERM_ANDROID_ECHO_VALUE 1)
else()
	set(MIKUTERM_ANDROID_ECHO_VALUE 0)
endif()

add_library(mikuterm SHARED ${MIKUTERM_ANDROID_SOURCES})
target_include_directories(mikuterm PRIVATE
	${MIKUTERM_ROOT}
	${MIKUTERM_ROOT}/userland/desktop/include
	${MIKUTERM_ROOT}/apps/libkeiland/android
	${MIKUTERM_COMMON_DIR}
	${MIKUTERM_INCLUDE})
target_compile_definitions(mikuterm PRIVATE
	_GNU_SOURCE
	MIKUTERM_ANDROID_HELLO=${MIKUTERM_ANDROID_HELLO_VALUE}
	MIKUTERM_ANDROID_ECHO=${MIKUTERM_ANDROID_ECHO_VALUE}
	MIKUTERM_HAVE_WSL=0
	MIKUTERM_HAVE_OPENER=0
	MIKUTERM_HAVE_X11=1
	MIKUTERM_HAVE_KEY_PASTE=1
	MIKUTERM_HAVE_SFTP_DRAG=0
	_stricmp=strcasecmp)
target_compile_options(mikuterm PRIVATE -std=gnu11 -Wall -Wextra -Werror -fvisibility=hidden)
target_link_libraries(mikuterm PRIVATE mikuterm_common mikuterm_terminal mikuterm_rdp mikuterm_ssl mikuterm_h264sw keiland_android mikuterm_xserver)

# The system's native-activity loader looks the entry point up in the library, and the runtime
# looks JNI_OnLoad up, so both stay in the library's symbols even though nothing in the library
# calls them.  Pages are 16 KB aligned for devices with 16 KB pages.
target_link_libraries(mikuterm PRIVATE android log mediandk)
target_link_options(mikuterm PRIVATE
	-Wl,-u,ANativeActivity_onCreate
	-Wl,-u,JNI_OnLoad
	-Wl,-z,max-page-size=16384
	-Wl,--gc-sections)
