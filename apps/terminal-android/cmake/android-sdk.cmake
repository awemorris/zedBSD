# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Where the Android SDK, the NDK and the tools of the build are, found in one place.
# ANDROID_SDK_ROOT and ANDROID_NDK_ROOT are given with -D, or come from the environment
# (ANDROID_SDK_ROOT, then ANDROID_HOME).  The build-tools and the platform are named by
# version.  Nothing is downloaded: what is missing is named and the configure stops.
#

option(MIKUTERM_ASSETS_ONLY "Only configure the assets target" OFF)
if(MIKUTERM_ASSETS_ONLY)
	return()
endif()

set(ANDROID_PLATFORM_VERSION "35" CACHE STRING "The SDK's platform (API level) the Java code is compiled against")

if(NOT ANDROID_SDK_ROOT)
	if(DEFINED ENV{ANDROID_SDK_ROOT})
		set(ANDROID_SDK_ROOT "$ENV{ANDROID_SDK_ROOT}")
	elseif(DEFINED ENV{ANDROID_HOME})
		set(ANDROID_SDK_ROOT "$ENV{ANDROID_HOME}")
	endif()
endif()
if(NOT ANDROID_SDK_ROOT OR NOT IS_DIRECTORY "${ANDROID_SDK_ROOT}")
	message(FATAL_ERROR "The Android SDK is not known: pass -DANDROID_SDK_ROOT=<root> or set ANDROID_SDK_ROOT")
endif()
set(ANDROID_SDK_ROOT "${ANDROID_SDK_ROOT}" CACHE PATH "The Android SDK's root" FORCE)

if(NOT ANDROID_BUILD_TOOLS_VERSION OR NOT EXISTS "${ANDROID_SDK_ROOT}/build-tools/${ANDROID_BUILD_TOOLS_VERSION}")
	file(GLOB _build_tools_dirs RELATIVE "${ANDROID_SDK_ROOT}/build-tools" "${ANDROID_SDK_ROOT}/build-tools/*")
	if(_build_tools_dirs)
		list(SORT _build_tools_dirs COMPARE NATURAL)
		list(POP_BACK _build_tools_dirs _latest_build_tools)
		set(ANDROID_BUILD_TOOLS_VERSION "${_latest_build_tools}" CACHE STRING "The SDK's build-tools version" FORCE)
	else()
		set(ANDROID_BUILD_TOOLS_VERSION "35.0.0" CACHE STRING "The SDK's build-tools version")
	endif()
else()
	set(ANDROID_BUILD_TOOLS_VERSION "${ANDROID_BUILD_TOOLS_VERSION}" CACHE STRING "The SDK's build-tools version")
endif()

if(NOT ANDROID_NDK_ROOT)
	if(DEFINED ENV{ANDROID_NDK_ROOT})
		set(ANDROID_NDK_ROOT "$ENV{ANDROID_NDK_ROOT}")
	endif()
endif()
if(NOT ANDROID_NDK_ROOT OR NOT EXISTS "${ANDROID_NDK_ROOT}/source.properties")
	message(FATAL_ERROR "The Android NDK is not known: pass -DANDROID_NDK_ROOT=<root> (r28), the folder that holds source.properties")
endif()
file(STRINGS "${ANDROID_NDK_ROOT}/source.properties" ANDROID_NDK_REVISION_LINE REGEX "^Pkg.Revision")
string(REGEX REPLACE "^Pkg.Revision *= *" "" ANDROID_NDK_VERSION "${ANDROID_NDK_REVISION_LINE}")
if(NOT ANDROID_NDK_VERSION MATCHES "^28\\.")
	message(FATAL_ERROR "The NDK is version '${ANDROID_NDK_VERSION}', not r28 (28.x): ${ANDROID_NDK_ROOT}")
endif()
set(ANDROID_NDK_ROOT "${ANDROID_NDK_ROOT}" CACHE PATH "The Android NDK's root (r28)" FORCE)
set(ANDROID_NDK_TOOLCHAIN_FILE "${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake")
file(GLOB _prebuilt "${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt/*")
if(_prebuilt)
	list(GET _prebuilt 0 _first_prebuilt)
	set(ANDROID_NDK_BIN "${_first_prebuilt}/bin")
else()
	set(ANDROID_NDK_BIN "${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt/linux-x86_64/bin")
endif()
if(NOT EXISTS "${ANDROID_NDK_TOOLCHAIN_FILE}")
	message(FATAL_ERROR "The NDK has no toolchain file: ${ANDROID_NDK_TOOLCHAIN_FILE}")
endif()

set(ANDROID_BUILD_TOOLS "${ANDROID_SDK_ROOT}/build-tools/${ANDROID_BUILD_TOOLS_VERSION}")
set(ANDROID_JAR "${ANDROID_SDK_ROOT}/platforms/android-${ANDROID_PLATFORM_VERSION}/android.jar")
if(NOT EXISTS "${ANDROID_JAR}")
	message(FATAL_ERROR "android.jar of the platform is missing: ${ANDROID_JAR}")
endif()

# The SDK's own tools are looked for in the build-tools folder only.
foreach(tool aapt2 d8 zipalign apksigner dexdump)
	string(TOUPPER ${tool} var)
	find_program(${var} NAMES ${tool} ${tool}.exe ${tool}.bat ${tool}.cmd PATHS "${ANDROID_BUILD_TOOLS}" NO_DEFAULT_PATH)
	if(NOT ${var})
		message(FATAL_ERROR "${tool} is missing in ${ANDROID_BUILD_TOOLS}")
	endif()
endforeach()
find_program(ADB NAMES adb adb.exe PATHS "${ANDROID_SDK_ROOT}/platform-tools" NO_DEFAULT_PATH)
find_program(LLVM_STRIP NAMES llvm-strip llvm-strip.exe PATHS "${ANDROID_NDK_BIN}" NO_DEFAULT_PATH)
if(NOT LLVM_STRIP)
	message(FATAL_ERROR "llvm-strip is missing in ${ANDROID_NDK_BIN}")
endif()

# The host's tools.
foreach(tool javac keytool zip)
	string(TOUPPER ${tool} var)
	find_program(${var} ${tool})
	if(NOT ${var})
		message(FATAL_ERROR "${tool} is missing on this machine")
	endif()
endforeach()
