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

  set(crt0 "${ZEDBSD_SYSROOT}/usr/lib/crt0.o")
  set(libc_bundle "${ZEDBSD_SYSROOT}/usr/lib/libc.o")
  set(runtime_bundle
      "${ZEDBSD_SYSROOT}/usr/lib/libzedbsd-compiler-rt.o")
  set(llvm_builtins
      "${ZEDBSD_SYSROOT}/usr/lib/libclang_rt.builtins.a")
  set(linker_script
      "${ZEDBSD_SYSROOT}/usr/lib/zedbsd/${zedbsd_arch}/user.ld")
  foreach(input IN ITEMS
      "${crt0}" "${libc_bundle}" "${runtime_bundle}" "${llvm_builtins}"
      "${linker_script}")
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
  target_link_libraries("${target}" PRIVATE
    "${crt0}"
    "${libc_bundle}"
    "${runtime_bundle}"
    "${llvm_builtins}"
  )
  target_link_options("${target}" PRIVATE
    -nostdlib
    -static
    -no-pie
    "LINKER:-m,${zedbsd_emulation}"
    "LINKER:--gc-sections"
    "LINKER:--build-id=none"
    "LINKER:-z,max-page-size=4096"
    "LINKER:-z,stack-size=0x100000"
    "LINKER:-T,${linker_script}"
  )
  set_property(TARGET "${target}" APPEND PROPERTY LINK_DEPENDS
    "${crt0}"
    "${libc_bundle}"
    "${runtime_bundle}"
    "${llvm_builtins}"
    "${linker_script}"
  )
endfunction()
