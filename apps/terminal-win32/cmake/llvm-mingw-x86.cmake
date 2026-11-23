# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

# mikuterm for Windows on x64 with llvm-mingw: cmake -DCMAKE_TOOLCHAIN_FILE=<this> -DLLVM_MINGW=<root>.
set(MIKUTERM_ARCH i686)
include(${CMAKE_CURRENT_LIST_DIR}/llvm-mingw.cmake)
