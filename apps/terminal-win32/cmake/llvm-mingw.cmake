# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# The cross build of mikuterm with llvm-mingw (clang, lld, the UCRT): the
# toolchain file of one architecture sets MIKUTERM_ARCH (x86_64 or aarch64)
# and includes this one.  LLVM_MINGW names llvm-mingw's root, given with
# -DLLVM_MINGW=<root> or in the environment.
#

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR ${MIKUTERM_ARCH})

if(NOT LLVM_MINGW)
	set(LLVM_MINGW "$ENV{LLVM_MINGW}")
endif()
if(NOT LLVM_MINGW)
	message(FATAL_ERROR "llvm-mingw's root is not known: pass -DLLVM_MINGW=<root> or set LLVM_MINGW")
endif()

# The test compilations CMake makes see the same choices.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LLVM_MINGW MIKUTERM_ARCH)

set(MIKUTERM_TRIPLE ${MIKUTERM_ARCH}-w64-mingw32)
set(CMAKE_C_COMPILER ${LLVM_MINGW}/bin/${MIKUTERM_TRIPLE}-clang)
set(CMAKE_RC_COMPILER ${LLVM_MINGW}/bin/${MIKUTERM_TRIPLE}-windres)
set(CMAKE_AR ${LLVM_MINGW}/bin/llvm-ar)
set(CMAKE_RANLIB ${LLVM_MINGW}/bin/llvm-ranlib)
set(CMAKE_OBJCOPY ${LLVM_MINGW}/bin/${MIKUTERM_TRIPLE}-objcopy)

set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW}/${MIKUTERM_TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
