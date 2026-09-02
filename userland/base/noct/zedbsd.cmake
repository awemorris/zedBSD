# zedBSD-owned link inputs for the canonical Noct amd64 target.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

function(noct_configure_zedbsd_target target)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "zedBSD Noct target does not exist: ${target}")
  endif()
  if(NOT DEFINED ZEDBSD_SOURCE_DIR OR ZEDBSD_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR
      "ZEDBSD_SOURCE_DIR must name the zedBSD source tree")
  endif()

  set(root "${ZEDBSD_SOURCE_DIR}")
  set(runtime_target "${target}_zedbsd_runtime")
  set(runtime_sources
    "${root}/src/crt/crt0-amd64.S"
    "${root}/userland/base/libc/posix.c"
    "${root}/userland/base/libc/dlfcn.c"
    "${root}/userland/base/libc/static-tls.c"
    "${root}/userland/base/libc/poll.c"
    "${root}/userland/base/libc/termios.c"
    "${root}/userland/base/libc/pthread.c"
    "${root}/userland/base/libc/timer.c"
    "${root}/userland/base/libc/shm.c"
    "${root}/userland/base/libc/semaphore.c"
    "${root}/userland/base/libc/mqueue.c"
    "${root}/userland/base/libc/socket.c"
    "${root}/userland/base/libc/resolver.c"
    "${root}/userland/base/libc/resolver-dns.c"
    "${root}/userland/base/libc/signal.c"
    "${root}/userland/base/libc/account.c"
    "${root}/userland/base/libc/crypt.c"
    "${root}/userland/base/libc/utmpx.c"
    "${root}/userland/base/libc/atomic-runtime.c"
    "${root}/libc/heap.c"
    "${root}/libc/string.c"
    "${root}/libc/string-extra.c"
    "${root}/libc/ctype.c"
    "${root}/libc/fenv.c"
    "${root}/libc/locale.c"
    "${root}/libc/wide.c"
    "${root}/libc/wide-extra.c"
    "${root}/libc/int64.c"
    "${root}/libc/inttypes.c"
    "${root}/libc/strto.c"
    "${root}/libc/stdlib-extra.c"
    "${root}/libc/time-extra.c"
    "${root}/libc/format.c"
    "${root}/libc/stdio.c"
    "${root}/libc/stdio-extra.c"
    "${root}/libc/setjmp.c"
    "${root}/libc/err.c"
    "${root}/libc/libgen.c"
    "${root}/libc/search.c"
    "${root}/libc/random48.c"
    "${root}/libc/random.c"
    "${root}/libc/xsi-crypto.c"
    "${root}/libc/ftw.c"
    "${root}/libc/ndbm.c"
    "${root}/libc/realpath.c"
    "${root}/libc/tempnam.c"
    "${root}/libc/xsi-process.c"
    "${root}/libc/fmtmsg.c"
    "${root}/libc/syslog.c"
    "${root}/libc/sysv-ipc.c"
    "${root}/libc/catalog.c"
    "${root}/libc/locale-db.c"
    "${root}/libc/fnmatch.c"
    "${root}/libc/regex/regcomp.c"
    "${root}/libc/regex/regexec.c"
    "${root}/libc/regex/regerror.c"
    "${root}/libc/regex/tre-mem.c"
    "${root}/libc/math.c"
    "${root}/libc/float-parse.c"
    "${root}/src/softfloat/zed-softfloat.c"
  )
  set(linker_script "${root}/platform/amd64/user.ld")

  foreach(source IN LISTS runtime_sources)
    if(NOT EXISTS "${source}")
      message(FATAL_ERROR "zedBSD Noct runtime input is missing: ${source}")
    endif()
  endforeach()
  if(NOT EXISTS "${linker_script}")
    message(FATAL_ERROR "zedBSD Noct linker script is missing: ${linker_script}")
  endif()

  if(TARGET "${runtime_target}")
    message(FATAL_ERROR
      "zedBSD Noct runtime target already exists: ${runtime_target}")
  endif()
  add_library("${runtime_target}" OBJECT ${runtime_sources})
  target_include_directories("${runtime_target}" PRIVATE
    "${root}"
    "${root}/include"
    "${root}/include/uapi"
    "${root}/src"
    "${root}/libc/include"
    "${root}/build/amd64"
  )
  target_compile_definitions("${runtime_target}" PRIVATE
    HAL_ARCH_AMD64
    ZEDBSD_USER_ABI_LP64
  )
  target_compile_options("${runtime_target}" PRIVATE
    -nostdinc
    -U__linux__
    -U__linux
    -Ulinux
    -m64
    -march=x86-64
    -mno-red-zone
    -ffreestanding
    -fno-builtin
    -fno-pic
    -fno-pie
    -fno-stack-protector
    -fno-asynchronous-unwind-tables
    -fno-unwind-tables
    -fno-common
    -fno-strict-aliasing
    -ffunction-sections
    -fdata-sections
  )
  set_source_files_properties(
    "${root}/libc/math.c"
    "${root}/libc/float-parse.c"
    "${root}/src/softfloat/zed-softfloat.c"
    PROPERTIES COMPILE_OPTIONS "-mlong-double-64"
  )
  target_sources("${target}" PRIVATE
    "$<TARGET_OBJECTS:${runtime_target}>")
  target_link_options("${target}" PRIVATE
    -nostdlib
    -static
    -no-pie
    "LINKER:-m,elf_x86_64"
    "LINKER:--gc-sections"
    "LINKER:--build-id=none"
    "LINKER:-z,max-page-size=4096"
    "LINKER:-z,stack-size=0x100000"
    "LINKER:-T,${linker_script}"
  )
  set_property(TARGET "${target}" APPEND PROPERTY LINK_DEPENDS
    "${linker_script}")
endfunction()

