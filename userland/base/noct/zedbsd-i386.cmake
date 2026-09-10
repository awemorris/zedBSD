# Noct cross build for zedBSD i386 ILP32.

set(ZEDBSD TRUE)
set(UNIX TRUE)
set(CMAKE_EXECUTABLE_SUFFIX "")
set(CMAKE_STATIC_LIBRARY_PREFIX "lib")
set(CMAKE_STATIC_LIBRARY_SUFFIX ".a")
set(CMAKE_FIND_LIBRARY_PREFIXES "lib")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
set(CMAKE_DL_LIBS "")
set(CMAKE_SHARED_LIBRARY_SUPPORTED FALSE)

if((NOT DEFINED ZEDBSD_SOURCE_DIR OR ZEDBSD_SOURCE_DIR STREQUAL "") AND
   DEFINED ENV{ZEDBSD_SOURCE_DIR} AND
   NOT "$ENV{ZEDBSD_SOURCE_DIR}" STREQUAL "")
  set(ZEDBSD_SOURCE_DIR "$ENV{ZEDBSD_SOURCE_DIR}" CACHE PATH
      "Absolute path to the zedBSD source tree")
endif()

if(NOT DEFINED ZEDBSD_SOURCE_DIR OR ZEDBSD_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR
    "The zedBSD preset requires ZEDBSD_SOURCE_DIR to name the zedBSD source tree")
endif()
if(NOT IS_ABSOLUTE "${ZEDBSD_SOURCE_DIR}")
  message(FATAL_ERROR
    "ZEDBSD_SOURCE_DIR must be an absolute path: ${ZEDBSD_SOURCE_DIR}")
endif()
if(NOT IS_DIRECTORY "${ZEDBSD_SOURCE_DIR}")
  message(FATAL_ERROR
    "ZEDBSD_SOURCE_DIR is not a directory: ${ZEDBSD_SOURCE_DIR}")
endif()

get_filename_component(ZEDBSD_SOURCE_DIR "${ZEDBSD_SOURCE_DIR}" REALPATH)
set(ZEDBSD_SOURCE_DIR "${ZEDBSD_SOURCE_DIR}" CACHE PATH
    "Absolute path to the zedBSD source tree" FORCE)

set(_NOCT_ZEDBSD_REQUIRED_FILES
  Makefile
  include/hal/arch/i386.h
  include/uapi/zedbsd/system.h
  libc/include/stdint.h
  platform/pcat/user.ld
  userland/base/noct/zedbsd.cmake
)
foreach(_NOCT_ZEDBSD_FILE IN LISTS _NOCT_ZEDBSD_REQUIRED_FILES)
  if(NOT EXISTS "${ZEDBSD_SOURCE_DIR}/${_NOCT_ZEDBSD_FILE}")
    message(FATAL_ERROR
      "ZEDBSD_SOURCE_DIR is missing required file: ${_NOCT_ZEDBSD_FILE}")
  endif()
endforeach()
unset(_NOCT_ZEDBSD_FILE)
unset(_NOCT_ZEDBSD_REQUIRED_FILES)

list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ZEDBSD_SOURCE_DIR)
list(REMOVE_DUPLICATES CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)

foreach(_NOCT_ZEDBSD_ENV IN ITEMS ZEDBSD_LLVM_BIN ZEDBSD_SYSROOT)
  if((NOT DEFINED ${_NOCT_ZEDBSD_ENV} OR
      "${${_NOCT_ZEDBSD_ENV}}" STREQUAL "") AND
     DEFINED ENV{${_NOCT_ZEDBSD_ENV}} AND
     NOT "$ENV{${_NOCT_ZEDBSD_ENV}}" STREQUAL "")
    set(${_NOCT_ZEDBSD_ENV} "$ENV{${_NOCT_ZEDBSD_ENV}}" CACHE PATH
        "zedBSD target toolchain input")
  endif()
  if(NOT DEFINED ${_NOCT_ZEDBSD_ENV} OR
     "${${_NOCT_ZEDBSD_ENV}}" STREQUAL "")
    message(FATAL_ERROR "${_NOCT_ZEDBSD_ENV} is required")
  endif()
  get_filename_component(${_NOCT_ZEDBSD_ENV}
      "${${_NOCT_ZEDBSD_ENV}}" REALPATH)
  set(${_NOCT_ZEDBSD_ENV} "${${_NOCT_ZEDBSD_ENV}}" CACHE PATH
      "zedBSD target toolchain input" FORCE)
endforeach()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
  ZEDBSD_LLVM_BIN ZEDBSD_SYSROOT)
list(REMOVE_DUPLICATES CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)

set(CMAKE_SYSTEM_NAME zedBSD)
set(CMAKE_SYSTEM_PROCESSOR i386)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_COMPILER "${ZEDBSD_LLVM_BIN}/clang" CACHE FILEPATH
    "C compiler for zedBSD i386" FORCE)
set(CMAKE_ASM_COMPILER "${ZEDBSD_LLVM_BIN}/clang" CACHE FILEPATH
    "Assembler driver for zedBSD i386" FORCE)
set(CMAKE_AR "${ZEDBSD_LLVM_BIN}/llvm-ar" CACHE FILEPATH
    "Archiver for zedBSD i386" FORCE)
set(CMAKE_RANLIB "${ZEDBSD_LLVM_BIN}/llvm-ranlib" CACHE FILEPATH
    "Archive indexer for zedBSD i386" FORCE)
set(CMAKE_LINKER "${ZEDBSD_LLVM_BIN}/ld.lld" CACHE FILEPATH
    "Linker for zedBSD i386" FORCE)
set(CMAKE_C_COMPILER_TARGET i386-unknown-zedbsd)
set(CMAKE_ASM_COMPILER_TARGET i386-unknown-zedbsd)
set(CMAKE_SYSROOT "${ZEDBSD_SYSROOT}")

set(CMAKE_C_FLAGS_INIT
  "-m32 -march=i486 -msoft-float -mno-80387 -mno-fp-ret-in-387 -mno-mmx -mno-sse -mno-sse2 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-builtin -fno-common -ffunction-sections -fdata-sections -fno-strict-aliasing -DHAL_ARCH_I386")
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES
  "${ZEDBSD_SYSROOT}/usr/include"
)

set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
set(CMAKE_SKIP_RPATH TRUE)
set(CMAKE_BUILD_RPATH "")
set(CMAKE_INSTALL_RPATH "")

set(CMAKE_FIND_ROOT_PATH "${ZEDBSD_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
