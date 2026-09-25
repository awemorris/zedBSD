# zedBSD-owned sysroot link boundary for the canonical Noct targets.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

function(noct_configure_zedbsd_target target)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "zedBSD Noct target does not exist: ${target}")
  endif()
  if(NOT DEFINED ZEDBSD_SYSROOT OR ZEDBSD_SYSROOT STREQUAL "")
    message(FATAL_ERROR "ZEDBSD_SYSROOT must name the target sysroot")
  endif()

  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "i386")
    set(zedbsd_arch pcat)
    set(zedbsd_compile_options -m32 -march=i486 -msoft-float -mno-80387 -mno-fp-ret-in-387 -mno-mmx -mno-sse -mno-sse2)
    set(zedbsd_definitions HAL_ARCH_I386)
    set(zedbsd_emulation elf_i386)
  else()
    set(zedbsd_arch amd64)
    set(zedbsd_compile_options -m64 -march=x86-64 -mno-red-zone)
    set(zedbsd_definitions HAL_ARCH_AMD64 KERN_USER_ABI_LP64)
    set(zedbsd_emulation elf_x86_64)
  endif()

  # The interpreter is a position-independent executable that loads the
  # shared C library of the build (ZEDBSD_DYNAMIC_DIR) through /lib/ld.so.
  if(NOT DEFINED ENV{ZEDBSD_DYNAMIC_DIR} OR "$ENV{ZEDBSD_DYNAMIC_DIR}" STREQUAL "")
    message(FATAL_ERROR "ZEDBSD_DYNAMIC_DIR must name the build's shared libraries")
  endif()
  set(crt0 "${ZEDBSD_SYSROOT}/usr/lib/crt1.o")
  set(libc_bundle "$ENV{ZEDBSD_DYNAMIC_DIR}/libc.so")
  set(runtime_bundle
      "${ZEDBSD_SYSROOT}/usr/lib/libzedbsd-compiler-rt.o")
  set(llvm_builtins
      "${ZEDBSD_SYSROOT}/usr/lib/libclang_rt.builtins.a")
  foreach(input IN ITEMS
      "${crt0}" "${libc_bundle}" "${runtime_bundle}" "${llvm_builtins}")
    if(NOT EXISTS "${input}")
      message(FATAL_ERROR "zedBSD Noct sysroot input is missing: ${input}")
    endif()
  endforeach()

  target_compile_definitions("${target}" PRIVATE
    ${zedbsd_definitions}
  )
  target_compile_options("${target}" PRIVATE
    ${zedbsd_compile_options}
    -ffreestanding
    -fno-builtin
    -fPIE
    -fno-stack-protector
    -fno-asynchronous-unwind-tables
    -fno-unwind-tables
    -fno-common
    -fno-strict-aliasing
    -ffunction-sections
    -fdata-sections
  )
  # The libraries linked into it are position independent too; this comes
  # after the toolchain's -fno-pie, so it is the one that holds.
  foreach(library IN ITEMS noct noctapi)
    if(TARGET "${library}")
      target_compile_options("${library}" PRIVATE -fPIE)
    endif()
  endforeach()

  # The shared C library carries the soft-float runtime; the static runtime
  # bundle is not position independent and is left out.
  target_link_libraries("${target}" PRIVATE
    "${crt0}"
    "${libc_bundle}"
    "${llvm_builtins}"
  )
  target_link_options("${target}" PRIVATE
    -nostdlib
    -pie
    "LINKER:-m,${zedbsd_emulation}"
    "LINKER:--gc-sections"
    "LINKER:--build-id=none"
    "LINKER:--no-relax"
    "LINKER:--hash-style=sysv"
    "LINKER:-z,now"
    "LINKER:-z,relro"
    "LINKER:-z,separate-code"
    "LINKER:-z,stack-size=0x100000"
    "LINKER:--allow-shlib-undefined"
    "LINKER:--dynamic-linker=/lib/ld.so"
  )
  set_property(TARGET "${target}" APPEND PROPERTY LINK_DEPENDS
    "${crt0}"
    "${libc_bundle}"
    "${runtime_bundle}"
    "${llvm_builtins}"
  )
endfunction()
