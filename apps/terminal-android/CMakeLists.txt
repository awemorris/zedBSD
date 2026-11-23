# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# mikuterm for Android: the terminal (userland/desktop/terminal) on libkeiland's Android layer,
# as one apk.  This file only drives the parts: it has no compiler of its own.  Each ABI's
# native library is built by native/ with the NDK's toolchain, the Java by javac and d8, the
# manifest and the resources by aapt2, and the apk is aligned and signed with the SDK's tools.
# No Gradle.
#
#   cmake -S apps/terminal-android -B build/android -G Ninja \
#     -DANDROID_SDK_ROOT=$HOME/opt/android-sdk -DANDROID_NDK_ROOT=$HOME/opt/android-ndk-r28c
#   cmake --build build/android --target apk
#
# The apk is signed with a debug key made in the build folder unless MIKUTERM_KEYSTORE names one.
#

cmake_minimum_required(VERSION 3.20)
project(mikuterm-android VERSION 0.1.0 LANGUAGES NONE)

get_filename_component(MIKUTERM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(MIKUTERM_DIR ${CMAKE_CURRENT_SOURCE_DIR})

include(${MIKUTERM_DIR}/cmake/android-sdk.cmake)

set(MIKUTERM_ANDROID_ABIS "arm64-v8a;x86_64" CACHE STRING "The ABIs the apk carries")

# The version code grows with the version: major * 10000 + minor * 100 + patch.
math(EXPR MIKUTERM_VERSION_CODE
	"${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")
