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

add_library(mikuterm SHARED ${MIKUTERM_ROOT}/apps/terminal-android/jni/entry.c)
target_compile_options(mikuterm PRIVATE -Wall -Wextra -Werror)

# The system's native-activity loader looks the entry point up in the library, so it stays
# in the library's symbols even though nothing in the library calls it.  Pages are 16 KB
# aligned for devices with 16 KB pages.
target_link_libraries(mikuterm PRIVATE android log)
target_link_options(mikuterm PRIVATE
	-Wl,-u,ANativeActivity_onCreate
	-Wl,-z,max-page-size=16384
	-Wl,--gc-sections)
