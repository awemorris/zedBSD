# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Static library build for libmedia (userland/desktop/libmedia and userland/desktop/mediafile).
#
# Reusable via add_subdirectory() by apps/terminal-win32 and other CMake builds.
# On Windows, media-os-win32.c replaces media-os-posix.c and vulkan-1.dll is loaded
# dynamically at runtime through LoadLibraryExW (no link-time import library).
#

cmake_minimum_required(VERSION 3.20)
project(libmedia LANGUAGES C)

get_filename_component(LIBMEDIA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(LIBMEDIA_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(LIBMEDIA_VULKAN_INCLUDE "" CACHE PATH "A directory holding vulkan/vulkan.h (empty: fetch the headers)")
set(LIBMEDIA_DISTFILES "${LIBMEDIA_ROOT}/build/distfiles" CACHE PATH "Where downloaded files may already be")

if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
	set(LIBMEDIA_MSVC_STYLE ON)
	set(LIBMEDIA_WERROR_DEFAULT OFF)
else()
	set(LIBMEDIA_MSVC_STYLE OFF)
	set(LIBMEDIA_WERROR_DEFAULT ON)
endif()
option(LIBMEDIA_WERROR "Make every warning an error in libmedia" ${LIBMEDIA_WERROR_DEFAULT})

# Reads LIBMEDIA_SOURCES from userland/desktop/libmedia/sources.mk.
file(READ ${LIBMEDIA_ROOT}/userland/desktop/libmedia/sources.mk libmedia_sources_mk)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LIBMEDIA_ROOT}/userland/desktop/libmedia/sources.mk)
string(REGEX REPLACE "\\\\\n" " " libmedia_sources_mk "${libmedia_sources_mk}")
string(REGEX MATCH "(^|\n)LIBMEDIA_SOURCES[ \t]*:=([^\n]*)" libmedia_sources_found "${libmedia_sources_mk}")
if(NOT libmedia_sources_found)
	message(FATAL_ERROR "userland/desktop/libmedia/sources.mk has no LIBMEDIA_SOURCES")
endif()
string(REGEX MATCHALL "[^ \t]+\\.c" LIBMEDIA_CORE_SOURCES "${CMAKE_MATCH_2}")

# Substitutes the OS layer on Windows.
if(WIN32)
	list(REMOVE_ITEM LIBMEDIA_CORE_SOURCES userland/desktop/libmedia/media-os-posix.c)
	list(FILTER LIBMEDIA_CORE_SOURCES EXCLUDE REGEX "^\\$\\(LIBMEDIA_OS_SOURCES\\)$")
	list(PREPEND LIBMEDIA_CORE_SOURCES userland/desktop/libmedia/media-os-win32.c)
else()
	list(FILTER LIBMEDIA_CORE_SOURCES EXCLUDE REGEX "^\\$\\(LIBMEDIA_OS_SOURCES\\)$")
	list(PREPEND LIBMEDIA_CORE_SOURCES userland/desktop/libmedia/media-os-posix.c)
endif()
list(TRANSFORM LIBMEDIA_CORE_SOURCES PREPEND ${LIBMEDIA_ROOT}/)

# Reuses caller-provided Vulkan headers when set; otherwise fetches vulkan-sdk-1.4.309.0.
set(LIBMEDIA_VULKAN_SDK_TAG vulkan-sdk-1.4.309.0)
set(LIBMEDIA_VULKAN_SHA256 2bc1b4127950badc80212abf1edfa5c3b5032f3425edf37255863ba7592c1969)
if(NOT LIBMEDIA_VULKAN_INCLUDE)
	set(tarball ${CMAKE_CURRENT_BINARY_DIR}/sources/Vulkan-Headers-${LIBMEDIA_VULKAN_SDK_TAG}.tar.gz)
	set(local ${LIBMEDIA_DISTFILES}/Vulkan-Headers-${LIBMEDIA_VULKAN_SDK_TAG}.tar.gz)
	if(NOT EXISTS ${tarball} AND EXISTS ${local})
		file(SHA256 ${local} local_sha256)
		if(local_sha256 STREQUAL LIBMEDIA_VULKAN_SHA256)
			configure_file(${local} ${tarball} COPYONLY)
		endif()
	endif()
	if(NOT EXISTS ${tarball})
		file(DOWNLOAD
			https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/${LIBMEDIA_VULKAN_SDK_TAG}.tar.gz
			${tarball}
			EXPECTED_HASH SHA256=${LIBMEDIA_VULKAN_SHA256}
			TLS_VERIFY ON)
	endif()
	file(ARCHIVE_EXTRACT INPUT ${tarball} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/vulkan-headers
		PATTERNS Vulkan-Headers-${LIBMEDIA_VULKAN_SDK_TAG}/include)
	set(LIBMEDIA_VULKAN_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/vulkan-headers/Vulkan-Headers-${LIBMEDIA_VULKAN_SDK_TAG}/include)
endif()

add_library(libmedia STATIC ${LIBMEDIA_CORE_SOURCES})
add_library(media ALIAS libmedia)
set_target_properties(libmedia PROPERTIES OUTPUT_NAME media)

target_include_directories(libmedia PUBLIC
	${LIBMEDIA_ROOT}
	${LIBMEDIA_ROOT}/userland/desktop/include)
target_include_directories(libmedia PRIVATE
	${LIBMEDIA_ROOT}/userland/desktop/libmedia
	${LIBMEDIA_VULKAN_INCLUDE})

if(WIN32)
	target_compile_definitions(libmedia PRIVATE
		_WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE)
endif()

if(LIBMEDIA_MSVC_STYLE)
	target_compile_options(libmedia PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11 /W3 /utf-8>)
	target_compile_definitions(libmedia PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
	if(LIBMEDIA_WERROR)
		target_compile_options(libmedia PRIVATE $<$<COMPILE_LANGUAGE:C>:/WX>)
	endif()
else()
	target_compile_options(libmedia PRIVATE
		$<$<COMPILE_LANGUAGE:C>:-std=gnu11 -Wall -Wextra -Wno-unknown-warning-option -Wno-maybe-uninitialized>)
	if(LIBMEDIA_WERROR)
		target_compile_options(libmedia PRIVATE $<$<COMPILE_LANGUAGE:C>:-Werror>)
	endif()
endif()
