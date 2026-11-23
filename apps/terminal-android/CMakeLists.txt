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

# For trying the program in parts (each is passed on to the native build): the trial body that
# paints the window one colour, and the connection that shows back what is typed.
option(MIKUTERM_ANDROID_HELLO "Build the trial body of the native thread" OFF)
option(MIKUTERM_ANDROID_ECHO "Open tabs with a connection that shows back what is typed" OFF)

# The version code grows with the version: major * 10000 + minor * 100 + patch.
math(EXPR MIKUTERM_VERSION_CODE
	"${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")

include(ExternalProject)

# One native library for each ABI: native/ is configured by the NDK's toolchain file in its
# own folder, and the stripped library is copied to where the apk's lib/ folder is made.
set(MIKUTERM_APK_DIR ${CMAKE_BINARY_DIR}/apk)
set(MIKUTERM_NATIVE_TARGETS "")
foreach(abi ${MIKUTERM_ANDROID_ABIS})
	ExternalProject_Add(native-${abi}
		SOURCE_DIR ${MIKUTERM_DIR}/native
		BINARY_DIR ${CMAKE_BINARY_DIR}/native/${abi}
		CMAKE_GENERATOR ${CMAKE_GENERATOR}
		CMAKE_ARGS
			-DCMAKE_TOOLCHAIN_FILE=${ANDROID_NDK_TOOLCHAIN_FILE}
			-DANDROID_ABI=${abi}
			-DANDROID_PLATFORM=android-28
			-DCMAKE_BUILD_TYPE=Release
			-DMIKUTERM_ROOT=${MIKUTERM_ROOT}
			-DMIKUTERM_VERSION=${PROJECT_VERSION}
			-DMIKUTERM_ANDROID_HELLO=${MIKUTERM_ANDROID_HELLO}
			-DMIKUTERM_ANDROID_ECHO=${MIKUTERM_ANDROID_ECHO}
		BUILD_ALWAYS ON
		INSTALL_COMMAND
			${CMAKE_COMMAND} -E make_directory ${MIKUTERM_APK_DIR}/lib/${abi}
			COMMAND ${LLVM_STRIP} --strip-unneeded
				-o ${MIKUTERM_APK_DIR}/lib/${abi}/libmikuterm.so
				${CMAKE_BINARY_DIR}/native/${abi}/libmikuterm.so)
	list(APPEND MIKUTERM_NATIVE_TARGETS native-${abi})
endforeach()
add_custom_target(native DEPENDS ${MIKUTERM_NATIVE_TARGETS})

# The Java: the classes of libkeiland's Android layer and of mikuterm, compiled against the
# platform's android.jar.  Each run of javac makes all the classes again.
file(GLOB_RECURSE MIKUTERM_JAVA_SOURCES CONFIGURE_DEPENDS
	${MIKUTERM_ROOT}/apps/libkeiland/android/java/*.java
	${MIKUTERM_DIR}/java/*.java)
set(MIKUTERM_CLASSES_DIR ${CMAKE_BINARY_DIR}/classes)
add_custom_command(
	OUTPUT ${CMAKE_BINARY_DIR}/classes.stamp
	COMMAND ${CMAKE_COMMAND} -E make_directory ${MIKUTERM_CLASSES_DIR}
	COMMAND ${JAVAC} --release 11 -classpath ${ANDROID_JAR} -Xlint:all -Werror
		-d ${MIKUTERM_CLASSES_DIR} ${MIKUTERM_JAVA_SOURCES}
	COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/classes.stamp
	DEPENDS ${MIKUTERM_JAVA_SOURCES}
	COMMENT "Compiling the Java"
	VERBATIM)
add_custom_target(classes DEPENDS ${CMAKE_BINARY_DIR}/classes.stamp)

# The classes as one dex file, the format the runtime loads.
set(MIKUTERM_DEX_DIR ${CMAKE_BINARY_DIR}/dex)
add_custom_command(
	OUTPUT ${MIKUTERM_DEX_DIR}/classes.dex
	COMMAND ${CMAKE_COMMAND} -E make_directory ${MIKUTERM_DEX_DIR}
	COMMAND ${CMAKE_COMMAND} -DCLASSES_DIR=${MIKUTERM_CLASSES_DIR} -DD8=${D8} -DANDROID_JAR=${ANDROID_JAR}
		-DOUT_DIR=${MIKUTERM_DEX_DIR} -P ${MIKUTERM_DIR}/cmake/run-d8.cmake
	DEPENDS ${CMAKE_BINARY_DIR}/classes.stamp ${MIKUTERM_DIR}/cmake/run-d8.cmake
	COMMENT "Making classes.dex"
	VERBATIM)
add_custom_target(dex DEPENDS ${MIKUTERM_DEX_DIR}/classes.dex)

# The program's read-only data (the fonts and their licenses) as the apk's assets; the program
# unpacks it into its own folder when the version in share.version changes.
set(MIKUTERM_SHARE
	"fonts/keiland.ttf=${MIKUTERM_ROOT}/userland/desktop/fonts/Mahora-Regular.ttf"
	"fonts/keiland-bold.ttf=${MIKUTERM_ROOT}/userland/desktop/fonts/Mahora-Bold.ttf"
	"fonts/keiland-mono.ttf=${MIKUTERM_ROOT}/userland/desktop/fonts/Mahora-Mono.ttf"
	"fonts/keiland-fallback-mono.ttf=${MIKUTERM_ROOT}/userland/desktop/fonts/JetBrainsMono-Regular.ttf"
	"fonts/keiland-fallback.ttf=${MIKUTERM_ROOT}/userland/desktop/fonts/DroidSansFallbackFull.ttf"
	"licenses/zedBSD-LICENSE.txt=${MIKUTERM_ROOT}/LICENSE"
	"licenses/Mahora-LICENSE.txt=${MIKUTERM_ROOT}/userland/desktop/fonts/Mahora-LICENSE.txt"
	"licenses/JetBrainsMono-OFL.txt=${MIKUTERM_ROOT}/userland/desktop/fonts/JetBrainsMono-OFL.txt"
	"licenses/DroidSansFallback-LICENSE.txt=${MIKUTERM_ROOT}/userland/desktop/fonts/DroidSansFallback-LICENSE.txt")
set(MIKUTERM_ASSETS_DIR ${CMAKE_BINARY_DIR}/assets)
set(MIKUTERM_SHARE_SOURCES "")
foreach(pair IN LISTS MIKUTERM_SHARE)
	string(REGEX REPLACE "^[^=]*=" "" source "${pair}")
	list(APPEND MIKUTERM_SHARE_SOURCES ${source})
endforeach()
add_custom_command(
	OUTPUT ${MIKUTERM_ASSETS_DIR}/share.version
	COMMAND ${CMAKE_COMMAND} "-DFILES=${MIKUTERM_SHARE}" -DOUT=${MIKUTERM_ASSETS_DIR} -DVERSION=${PROJECT_VERSION}
		-P ${MIKUTERM_DIR}/cmake/make-assets.cmake
	DEPENDS ${MIKUTERM_SHARE_SOURCES} ${MIKUTERM_DIR}/cmake/make-assets.cmake
	COMMENT "Making the assets"
	VERBATIM)
add_custom_target(assets DEPENDS ${MIKUTERM_ASSETS_DIR}/share.version)

# The resources, compiled, and the apk that holds the manifest and the resources only.  A debug
# build (the default) makes the apk debuggable.
option(MIKUTERM_DEBUGGABLE "Make the apk debuggable" ON)
set(MIKUTERM_BASE_APK ${CMAKE_BINARY_DIR}/base.apk)
file(GLOB_RECURSE MIKUTERM_RES_FILES CONFIGURE_DEPENDS ${MIKUTERM_DIR}/res/*)
add_custom_command(
	OUTPUT ${CMAKE_BINARY_DIR}/res.zip
	COMMAND ${AAPT2} compile --dir ${MIKUTERM_DIR}/res -o ${CMAKE_BINARY_DIR}/res.zip
	DEPENDS ${MIKUTERM_RES_FILES}
	COMMENT "Compiling the resources"
	VERBATIM)
add_custom_target(res DEPENDS ${CMAKE_BINARY_DIR}/res.zip)

set(MIKUTERM_AAPT2_DEBUG "")
if(MIKUTERM_DEBUGGABLE)
	set(MIKUTERM_AAPT2_DEBUG --debug-mode)
endif()
add_custom_command(
	OUTPUT ${MIKUTERM_BASE_APK}
	COMMAND ${AAPT2} link -I ${ANDROID_JAR} --manifest ${MIKUTERM_DIR}/AndroidManifest.xml
		--min-sdk-version 28 --target-sdk-version 35
		--version-code ${MIKUTERM_VERSION_CODE} --version-name ${PROJECT_VERSION}
		${MIKUTERM_AAPT2_DEBUG} -A ${MIKUTERM_ASSETS_DIR} -o ${MIKUTERM_BASE_APK} ${CMAKE_BINARY_DIR}/res.zip
	DEPENDS ${CMAKE_BINARY_DIR}/res.zip ${MIKUTERM_DIR}/AndroidManifest.xml ${MIKUTERM_ASSETS_DIR}/share.version
	COMMENT "Linking the manifest and the resources"
	VERBATIM)
add_custom_target(base-apk DEPENDS ${MIKUTERM_BASE_APK})

# The apk before signing: the base apk with classes.dex (compressed) and the native libraries
# (stored, so they are mapped from the apk), then aligned so that the libraries start on 16 KB
# boundaries.  These steps always run (they take a moment): the libraries are made by other
# builds that this build cannot see the files of, so the files' times say nothing.
set(MIKUTERM_UNALIGNED_APK ${CMAKE_BINARY_DIR}/unaligned.apk)
set(MIKUTERM_ALIGNED_APK ${CMAKE_BINARY_DIR}/aligned.apk)
add_custom_target(aligned-apk
	COMMAND ${CMAKE_COMMAND} -E copy ${MIKUTERM_BASE_APK} ${MIKUTERM_UNALIGNED_APK}
	COMMAND ${CMAKE_COMMAND} -E copy ${MIKUTERM_DEX_DIR}/classes.dex ${MIKUTERM_APK_DIR}/classes.dex
	COMMAND ${ZIP} -X ${MIKUTERM_UNALIGNED_APK} classes.dex
	COMMAND ${ZIP} -X -0 -r ${MIKUTERM_UNALIGNED_APK} lib
	COMMAND ${ZIPALIGN} -P 16 -f 4 ${MIKUTERM_UNALIGNED_APK} ${MIKUTERM_ALIGNED_APK}
	WORKING_DIRECTORY ${MIKUTERM_APK_DIR}
	DEPENDS ${MIKUTERM_BASE_APK} ${MIKUTERM_DEX_DIR}/classes.dex
	COMMENT "Adding the dex and the native libraries, and aligning"
	VERBATIM)
add_dependencies(aligned-apk native)

include(${MIKUTERM_DIR}/cmake/sign.cmake)

# Putting the apk on a device or emulator and starting it.  MIKUTERM_ADB_SERIAL picks the
# device (empty: adb's only one).
set(MIKUTERM_ADB_SERIAL "" CACHE STRING "The adb serial of the device to install on")
set(MIKUTERM_ADB_DEVICE "")
if(MIKUTERM_ADB_SERIAL)
	set(MIKUTERM_ADB_DEVICE -s ${MIKUTERM_ADB_SERIAL})
endif()
add_custom_target(install-apk
	COMMAND ${ADB} ${MIKUTERM_ADB_DEVICE} install -r ${MIKUTERM_APK}
	DEPENDS apk
	VERBATIM)
add_custom_target(run
	COMMAND ${ADB} ${MIKUTERM_ADB_DEVICE} shell am start -n io.noctvm.mikuterm/.MikutermActivity
	VERBATIM)
