# Deterministic zedBSD target sysroots (x86_64, i386, arm64, riscv64 and armv7).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ZEDBSD_SYSROOT_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_SYSROOT_X86_64 := $(abspath $(ZEDBSD_LLVM_ROOT)/build/x86_64/sysroot)
ZEDBSD_SYSROOT_I386 := $(abspath $(ZEDBSD_LLVM_ROOT)/build/i386/sysroot)
ZEDBSD_SYSROOT_ARM64 := $(abspath $(ZEDBSD_LLVM_ROOT)/build/arm64/sysroot)
ZEDBSD_SYSROOT_RISCV64 := $(abspath $(ZEDBSD_LLVM_ROOT)/build/riscv64/sysroot)
ZEDBSD_SYSROOT_ARMV7 := $(abspath $(ZEDBSD_LLVM_ROOT)/build/armv7/sysroot)
ZEDBSD_SYSROOT_CLANG := $(ZEDBSD_LLVM_INSTALL)/bin/clang
ZEDBSD_SYSROOT_AR := $(ZEDBSD_LLVM_INSTALL)/bin/llvm-ar
ZEDBSD_SYSROOT_LD := $(ZEDBSD_LLVM_INSTALL)/bin/ld.lld
ZEDBSD_SYSROOT_READELF := $(ZEDBSD_LLVM_INSTALL)/bin/llvm-readelf
ZEDBSD_SYSROOT_NM := $(ZEDBSD_LLVM_INSTALL)/bin/llvm-nm

# This is the existing static user ABI gathered into one archive.  Keep the
# manifest here so platform makefiles consume a sysroot instead of maintaining
# subtly different private copies of libc.
ZEDBSD_SYSROOT_LIBC_SOURCES := \
	userland/base/libc/posix.c \
	userland/base/libc/dlfcn.c \
	userland/base/libc/static-tls.c \
	userland/base/libc/poll.c \
	userland/base/libc/termios.c \
	userland/base/libc/pthread.c \
	userland/base/libc/timer.c \
	userland/base/libc/shm.c \
	userland/base/libc/semaphore.c \
	userland/base/libc/mqueue.c \
	userland/base/libc/socket.c \
	userland/base/libc/resolver.c \
	userland/base/libc/resolver-dns.c \
	userland/base/libc/signal.c \
	userland/base/libc/account.c \
	userland/base/libc/crypt.c \
	userland/base/libc/utmpx.c \
	src/libc/heap.c src/libc/string.c src/libc/ctype.c src/libc/locale.c src/libc/wide.c \
	src/libc/int64.c src/libc/strto.c src/libc/format.c src/libc/stdio.c \
	$(ZEDBSD_LIBC_USER_EXTRA_SOURCES)

ZEDBSD_SYSROOT_COMPILER_RT_SOURCES := \
	src/libc/softfloat.c \
	src/libc/compiler-runtime.c \
	$(ZEDBSD_LIBM_SOURCES) src/libc/float-parse.c

# AArch64 differs from x86 in two places.  clang has no __builtin_setjmp
# there, so the C library carries setjmp in assembly; and long double is
# IEEE binary128, whose arithmetic the compiler runtime supplies in software.
ZEDBSD_SYSROOT_ARM64_LIBC_SOURCES := src/libc/setjmp-aarch64.S
ZEDBSD_SYSROOT_ARM64_COMPILER_RT_SOURCES := \
	src/libc/softfloat128.c src/libc/compiler-runtime128.c

# 64-bit RISC-V is like AArch64 in both places: setjmp is in assembly, and
# long double is IEEE binary128 in software.
ZEDBSD_SYSROOT_RISCV64_LIBC_SOURCES := src/libc/setjmp-riscv64.S
ZEDBSD_SYSROOT_RISCV64_COMPILER_RT_SOURCES := \
	src/libc/softfloat128.c src/libc/compiler-runtime128.c

# 32-bit ARM carries setjmp in assembly as well.  Its long double is the
# 64-bit double, and the floating-point unit does the arithmetic, so the
# compiler runtime needs nothing beyond the common sources.
ZEDBSD_SYSROOT_ARMV7_LIBC_SOURCES := src/libc/setjmp-armv7.S

# The Cortex-A9 with its VFPv3 and NEON unit, the ARM instruction set, the
# thread pointer in TPIDRURO and no unaligned access the compiler would
# make up.  The 32-bit user interface's packed structures (struct stat)
# hold 64-bit members on 4-byte boundaries, which -Wunaligned-access
# reports; every load and store ARMv7 makes of them, ldrd and strd
# included, needs only that alignment, so the warning is not taken, as in
# the kernel (platform/armv7/vmunix.mk).
ZEDBSD_SYSROOT_ARMV7_CODE_FLAGS := -march=armv7-a -mcpu=cortex-a9 -marm \
	-mfloat-abi=hard -mfpu=neon-vfpv3 -mtp=cp15 -mno-unaligned-access \
	-Wno-unaligned-access

# zedBSD owns its floating-point compiler ABI so i386 kernels do not acquire
# an x87 dependency. Integer compiler builtins come from the same verified LLVM
# release used to build Clang. This explicit manifest is the supported ABI and
# source/license audit boundary.
ZEDBSD_SYSROOT_LLVM_BUILTIN_NAMES := \
	absvdi2.c absvsi2.c absvti2.c \
	addvdi3.c addvsi3.c addvti3.c \
	ashldi3.c ashlti3.c ashrdi3.c ashrti3.c \
	bswapdi2.c bswapsi2.c clzdi2.c clzsi2.c clzti2.c \
	cmpdi2.c cmpti2.c ctzdi2.c ctzsi2.c ctzti2.c \
	divdi3.c divmoddi4.c divmodsi4.c divmodti4.c divsi3.c divti3.c \
	ffsdi2.c ffssi2.c ffsti2.c int_util.c lshrdi3.c lshrti3.c \
	moddi3.c modsi3.c modti3.c \
	muldi3.c mulodi4.c mulosi4.c muloti4.c multi3.c \
	mulvdi3.c mulvsi3.c mulvti3.c \
	negdi2.c negti2.c negvdi2.c negvsi2.c negvti2.c \
	paritydi2.c paritysi2.c parityti2.c \
	popcountdi2.c popcountsi2.c popcountti2.c \
	subvdi3.c subvsi3.c subvti3.c ucmpdi2.c ucmpti2.c \
	udivdi3.c udivmoddi4.c udivmodsi4.c udivmodti4.c \
	udivsi3.c udivti3.c umoddi3.c umodsi3.c umodti3.c
ZEDBSD_SYSROOT_LLVM_BUILTIN_SOURCES := $(addprefix \
	$(ZEDBSD_LLVM_SOURCE)/compiler-rt/lib/builtins/,\
	$(ZEDBSD_SYSROOT_LLVM_BUILTIN_NAMES))
# AArch64 keeps its instruction cache coherent with data only on request, so
# a program that writes code (a JIT) calls __clear_cache, which the compiler
# runtime provides.  The kernel lets user code clean and invalidate the
# caches itself (SCTLR_EL1.UCI and UCT).
ZEDBSD_SYSROOT_ARM64_LLVM_BUILTIN_SOURCES := \
	$(ZEDBSD_LLVM_SOURCE)/compiler-rt/lib/builtins/clear_cache.c
# The Cortex-A9 has no divide instruction in the ARM instruction set, and
# its floating-point unit converts between floating point and 32-bit
# integers only.  The compiler therefore calls the run-time ABI's helpers
# (__aeabi_idivmod, __aeabi_uldivmod, __aeabi_l2d, __aeabi_d2lz and their
# kin) for those operations, and the memory helpers (__aeabi_memcpy and
# the others, which pass on to the C library's functions) for block
# copies.  The integer helpers of the common list give the division's
# remaining entry points their run-time ABI names.  The helpers keep the
# base calling convention, with floating-point values in core registers,
# as the run-time ABI requires; COMPILER_RT_ARMHF_TARGET gives the plain
# functions the floating-point unit's convention, which code built with
# -mfloat-abi=hard calls them with.
ZEDBSD_SYSROOT_ARMV7_LLVM_BUILTIN_NAMES := \
	arm/aeabi_idivmod.S arm/aeabi_uidivmod.S \
	arm/aeabi_ldivmod.S arm/aeabi_uldivmod.S \
	arm/aeabi_memcpy.S arm/aeabi_memmove.S arm/aeabi_memset.S \
	arm/aeabi_memcmp.S arm/aeabi_div0.c \
	fixdfdi.c fixunsdfdi.c floatdidf.c floatundidf.c \
	fixsfdi.c fixunssfdi.c floatdisf.c floatundisf.c
ZEDBSD_SYSROOT_ARMV7_LLVM_BUILTIN_SOURCES := $(addprefix \
	$(ZEDBSD_LLVM_SOURCE)/compiler-rt/lib/builtins/,\
	$(ZEDBSD_SYSROOT_ARMV7_LLVM_BUILTIN_NAMES))
ZEDBSD_SYSROOT_ARMV7_LLVM_BUILTIN_FLAGS := -DCOMPILER_RT_ARMHF_TARGET

# A clean cache-based bootstrap has build/llvm but no extracted LLVM source.
# Give each compiler-rt input a real generating prerequisite so parallel Make
# completes the verified source extraction before it diagnoses a missing file.
$(ZEDBSD_SYSROOT_LLVM_BUILTIN_SOURCES) \
$(ZEDBSD_SYSROOT_ARM64_LLVM_BUILTIN_SOURCES) \
$(ZEDBSD_SYSROOT_ARMV7_LLVM_BUILTIN_SOURCES): | $(ZEDBSD_LLVM_SOURCE_STAMP)
	@test -f '$@'

# The desktop's public headers (keiland.h, the Wayland client headers and the
# others in userland/desktop/include) are not the C library's, but they go to
# the same usr/include, where the desktop's programs find them (WS104).
ZEDBSD_SYSROOT_PUBLIC_HEADERS := $(shell \
	find include/libc include/libc include/uapi userland/desktop/include \
	-type f ! -name '*~' -print | LC_ALL=C sort)
# The names the public headers have under a sysroot's usr/include, the same
# mapping as the header loop of the rule below.  They come from the manifest,
# not from a sysroot on disk: a list read from the disk while make parses is
# the old sysroot's, and when a header is renamed or moved the rule below
# replaces that sysroot later in the same make, so whatever copies the listed
# files afterwards names a file that is gone (BUG-084).
ZEDBSD_SYSROOT_INCLUDE_NAMES := $(patsubst include/libc/%,%,\
	$(patsubst include/uapi/%,uapi/%,\
	$(patsubst userland/desktop/include/%,%,$(ZEDBSD_SYSROOT_PUBLIC_HEADERS))))
ZEDBSD_SYSROOT_LINKER_SCRIPTS := \
	platform/x86_64/user.ld platform/x86_64/vmunix.ld \
	platform/pcat/user.ld platform/pcat/vmunix.ld \
	platform/pc98/noct-user.ld platform/pc98/user-init.ld \
	platform/pc98/stage2.ld platform/arm64/user.ld platform/riscv64/user.ld \
	platform/armv7/user.ld \
	bootloader/pcat/stage1.ld bootloader/pcat/stage2.ld \
	bootloader/pcat/bootzbsd.ld \
	bootloader/pc98/stage1.ld bootloader/pc98/stage2.ld
ZEDBSD_SYSROOT_INPUTS := $(ZEDBSD_SYSROOT_LIBC_SOURCES) \
	$(ZEDBSD_SYSROOT_COMPILER_RT_SOURCES) $(ZEDBSD_LIBM_HEADERS) \
	$(ZEDBSD_SYSROOT_ARM64_LIBC_SOURCES) \
	$(ZEDBSD_SYSROOT_ARM64_COMPILER_RT_SOURCES) \
	$(ZEDBSD_SYSROOT_RISCV64_LIBC_SOURCES) \
	$(ZEDBSD_SYSROOT_ARMV7_LIBC_SOURCES) \
	$(ZEDBSD_SYSROOT_LLVM_BUILTIN_SOURCES) \
	$(ZEDBSD_SYSROOT_ARM64_LLVM_BUILTIN_SOURCES) \
	$(ZEDBSD_SYSROOT_ARMV7_LLVM_BUILTIN_SOURCES) \
	$(ZEDBSD_SYSROOT_PUBLIC_HEADERS) $(ZEDBSD_SYSROOT_LINKER_SCRIPTS) \
	src/libc/crt/crt0-x86_64.S src/libc/crt/crt1-x86_64.S \
	src/libc/crt/crt0-i386.S src/libc/crt/crt1-i386.S \
	src/libc/crt/crt0-aarch64.S src/libc/crt/crt1-aarch64.S \
	src/libc/crt/crt0-riscv64.S src/libc/crt/crt1-riscv64.S \
	src/libc/crt/crt0-armv7.S src/libc/crt/crt1-armv7.S \
	include/hal/arch.h include/hal/arch/x86_64.h include/hal/arch/i386.h \
	include/hal/arch/aarch64.h include/hal/arch/riscv64.h \
	include/hal/arch/armv7.h

# $(1) sysroot, $(2) its name, $(3) target triple, $(4) code generation
# flags, $(5) HAL architecture macro, $(6) crt0 and $(7) crt1 sources,
# $(8) and $(9) the linker script the smoke program is linked with, $(10) the
# ELF machine llvm-readelf names, $(11) user ABI macros, $(12) the C library's
# assembly sources, $(13) the compiler runtime's floating-point ABI flags and
# $(14) its sources beyond the common ones, $(15) the LLVM builtins beyond
# the common ones and $(16) the flags the LLVM builtins alone are built with.

define ZEDBSD_BUILD_SYSROOT
$(1)/.zedbsd-sysroot-complete: $(ZEDBSD_SYSROOT_INPUTS) \
	$(ZEDBSD_LLVM_INSTALL_STAMP)
	@set -eu; \
	destination='$(1)'; parent=$$$${destination%/*}; \
	mkdir -p "$$$$parent"; \
	temporary=$$$$(mktemp -d "$$$$parent/.sysroot-$(2).XXXXXX"); \
	cleanup() { if test -n "$$$$temporary" && test -d "$$$$temporary"; then find "$$$$temporary" -depth -delete; fi; }; \
	trap cleanup EXIT HUP INT TERM; \
	mkdir -p "$$$$temporary/usr/include" "$$$$temporary/usr/lib" \
		"$$$$temporary/usr/lib/zedbsd/x86_64" \
		"$$$$temporary/usr/lib/zedbsd/pcat" \
		"$$$$temporary/usr/lib/zedbsd/pc98" \
		"$$$$temporary/usr/lib/zedbsd/arm64" \
		"$$$$temporary/usr/lib/zedbsd/riscv64" \
		"$$$$temporary/usr/lib/zedbsd/armv7" "$$$$temporary/obj"; \
	for header in $$(ZEDBSD_SYSROOT_PUBLIC_HEADERS); do \
		case "$$$$header" in \
		include/libc/*) relative=$$$${header#include/libc/} ;; \
		include/libc/*) relative=$$$${header#include/libc/} ;; \
		include/uapi/*) relative=$$$${header#include/} ;; \
		userland/desktop/include/*) relative=$$$${header#userland/desktop/include/} ;; \
		*) echo "sysroot: non-public header in manifest: $$$$header" >&2; exit 1 ;; \
		esac; \
		case "$$$$relative" in */*) mkdir -p "$$$$temporary/usr/include/$$$${relative%/*}" ;; esac; \
		cp "$$$$header" "$$$$temporary/usr/include/$$$$relative"; \
	done; \
	for source in $$(ZEDBSD_SYSROOT_LIBC_SOURCES) $(12); do \
		object="$$$$temporary/obj/$$$$source.o"; mkdir -p "$$$${object%/*}"; \
		'$(ZEDBSD_SYSROOT_CLANG)' --target='$(3)' --sysroot="$$$$temporary" \
			$(4) -D$(5) $(11) -DZEDBSD_STATIC_TLS -nostdinc -Iinclude/libc -Iinclude -Isrc -I. \
			-ffreestanding -fno-builtin -fno-pic -fno-pie \
			-fno-stack-protector -fno-asynchronous-unwind-tables \
			-fno-unwind-tables -fno-common -fno-strict-aliasing \
			-ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror \
			-c "$$$$source" -o "$$$$object"; \
	done; \
	'$(ZEDBSD_SYSROOT_AR)' rcsD "$$$$temporary/usr/lib/libc.a" \
		$$$$(find "$$$$temporary/obj" -type f -name '*.o' -print | LC_ALL=C sort); \
	'$(ZEDBSD_SYSROOT_LD)' -r --whole-archive \
		"$$$$temporary/usr/lib/libc.a" --no-whole-archive \
		-o "$$$$temporary/usr/lib/libc.o"; \
	find "$$$$temporary/obj" -depth -delete; mkdir -p "$$$$temporary/obj"; \
	for source in $$(ZEDBSD_SYSROOT_COMPILER_RT_SOURCES) $(14); do \
		object="$$$$temporary/obj/$$$$source.o"; mkdir -p "$$$${object%/*}"; \
		'$(ZEDBSD_SYSROOT_CLANG)' --target='$(3)' --sysroot="$$$$temporary" \
			$(4) -D$(5) $(11) -DZEDBSD_STATIC_TLS -nostdinc -Iinclude/libc -Iinclude -Isrc -I. \
			-ffreestanding -fno-builtin -fno-pic -fno-pie \
			-fno-stack-protector -fno-asynchronous-unwind-tables \
			-fno-unwind-tables -fno-common -fno-strict-aliasing \
			$(13) -ffunction-sections -fdata-sections \
			-Os -Wall -Wextra -Werror -c "$$$$source" -o "$$$$object"; \
	done; \
	'$(ZEDBSD_SYSROOT_AR)' rcsD "$$$$temporary/usr/lib/libzedbsd-compiler-rt.a" \
		$$$$(find "$$$$temporary/obj" -type f -name '*.o' -print | LC_ALL=C sort); \
	'$(ZEDBSD_SYSROOT_LD)' -r --whole-archive \
		"$$$$temporary/usr/lib/libzedbsd-compiler-rt.a" --no-whole-archive \
		-o "$$$$temporary/usr/lib/libzedbsd-compiler-rt.o"; \
	find "$$$$temporary/obj" -depth -delete; mkdir -p "$$$$temporary/obj"; \
	for source in $$(ZEDBSD_SYSROOT_LLVM_BUILTIN_SOURCES) $(15); do \
		object="$$$$temporary/obj/$$$${source##*/}.o"; \
		'$(ZEDBSD_SYSROOT_CLANG)' --target='$(3)' --sysroot="$$$$temporary" \
			$(4) -D$(5) $(11) $(16) -nostdinc -isystem "$$$$temporary/usr/include" \
			-I'$(ZEDBSD_LLVM_SOURCE)/compiler-rt/lib/builtins' \
			-ffreestanding -fno-builtin -fno-pic -fno-pie \
			-fno-stack-protector -fno-asynchronous-unwind-tables \
			-fno-unwind-tables -fno-common -fno-strict-aliasing \
			-ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror \
			-Wno-unused-parameter \
			-c "$$$$source" -o "$$$$object"; \
	done; \
	'$(ZEDBSD_SYSROOT_AR)' rcsD "$$$$temporary/usr/lib/libclang_rt.builtins.a" \
		$$$$(find "$$$$temporary/obj" -type f \( -name '*.c.o' -o -name '*.S.o' \) -print | LC_ALL=C sort); \
	'$(ZEDBSD_SYSROOT_CLANG)' --target='$(3)' --sysroot="$$$$temporary" \
		$(4) -nostdinc -Iinclude -D$(5) $(11) \
		-ffreestanding -fno-pic -fno-pie -fno-stack-protector \
		-c '$(6)' -o "$$$$temporary/usr/lib/crt0.o"; \
	'$(ZEDBSD_SYSROOT_CLANG)' --target='$(3)' --sysroot="$$$$temporary" \
		$(4) -nostdinc -Iinclude -D$(5) $(11) \
		-ffreestanding -fno-pic -fno-pie -fno-stack-protector \
		-c '$(7)' -o "$$$$temporary/usr/lib/crt1.o"; \
	: 'zedBSD keeps the mathematics in the C library, which every link' \
	  'already has.  An empty libm is left here so that -lm, which a' \
	  'portable program written against the standard passes, finds a' \
	  'library and contributes nothing, rather than stopping the link' \
	  'over a library that was never separate on this system.'; \
	'$(ZEDBSD_SYSROOT_AR)' rcsD "$$$$temporary/usr/lib/libm.a"; \
	cp platform/x86_64/user.ld platform/x86_64/vmunix.ld \
		"$$$$temporary/usr/lib/zedbsd/x86_64/"; \
	cp platform/pcat/user.ld platform/pcat/vmunix.ld \
		bootloader/pcat/stage1.ld bootloader/pcat/stage2.ld \
		bootloader/pcat/bootzbsd.ld "$$$$temporary/usr/lib/zedbsd/pcat/"; \
	cp platform/pc98/noct-user.ld platform/pc98/user-init.ld \
		bootloader/pc98/stage1.ld "$$$$temporary/usr/lib/zedbsd/pc98/"; \
	cp platform/pc98/stage2.ld \
		"$$$$temporary/usr/lib/zedbsd/pc98/platform-stage2.ld"; \
	cp platform/arm64/user.ld "$$$$temporary/usr/lib/zedbsd/arm64/"; \
	cp platform/riscv64/user.ld "$$$$temporary/usr/lib/zedbsd/riscv64/"; \
	cp platform/armv7/user.ld "$$$$temporary/usr/lib/zedbsd/armv7/"; \
	cp bootloader/pc98/stage2.ld \
		"$$$$temporary/usr/lib/zedbsd/pc98/bootloader-stage2.ld"; \
	printf '%s\n' 'arch=$(2)' 'triple=$(3)' \
		'llvm=$(ZEDBSD_LLVM_VERSION)' 'patch=$(ZEDBSD_LLVM_PATCH_LEVEL)' \
		> "$$$$temporary/.zedbsd-sysroot-identity"; \
	printf '%s\n' '#include <stdint.h>' 'int main(void) { return (int)sizeof(uintptr_t); }' \
		> "$$$$temporary/smoke.c"; \
	'$(ZEDBSD_SYSROOT_CLANG)' --target='$(3)' --sysroot="$$$$temporary" \
		$(4) -D$(5) $(11) -nostdinc -isystem "$$$$temporary/usr/include" \
		-ffreestanding -fno-pic -fno-pie -fno-stack-protector \
		-c "$$$$temporary/smoke.c" -o "$$$$temporary/smoke.o"; \
	'$(ZEDBSD_SYSROOT_CLANG)' --target='$(3)' --sysroot="$$$$temporary" \
		$(4) -nostdlib -static -Wl,--build-id=none \
		-Wl,-T,"$$$$temporary/usr/lib/zedbsd/$(8)/$(9)" \
		"$$$$temporary/usr/lib/crt0.o" "$$$$temporary/smoke.o" \
		-Wl,--start-group "$$$$temporary/usr/lib/libc.a" \
		"$$$$temporary/usr/lib/libzedbsd-compiler-rt.a" \
		"$$$$temporary/usr/lib/libclang_rt.builtins.a" -Wl,--end-group \
		-o "$$$$temporary/smoke.elf"; \
	'$(ZEDBSD_SYSROOT_READELF)' -h "$$$$temporary/smoke.elf" | \
		grep -F 'Machine:                           $(10)' >/dev/null; \
	if '$(ZEDBSD_SYSROOT_NM)' -u "$$$$temporary/smoke.elf" | grep .; then \
		echo 'sysroot: smoke link retained undefined symbols' >&2; exit 1; \
	fi; \
	find "$$$$temporary/obj" -depth -delete; \
	(cd "$$$$temporary" && \
		find . -type f ! -name '.zedbsd-sysroot-manifest' -print0 | \
		LC_ALL=C sort -z | xargs -0 sha256sum) \
		> "$$$$temporary/.zedbsd-sysroot-manifest"; \
	touch "$$$$temporary/.zedbsd-generated-sysroot" \
		"$$$$temporary/.zedbsd-sysroot-complete"; \
	if test -e "$$$$destination"; then \
		test -f "$$$$destination/.zedbsd-generated-sysroot" || \
			{ echo "sysroot: refusing to replace unmanaged tree: $$$$destination" >&2; exit 1; }; \
		find "$$$$destination" -depth -delete; \
	fi; \
	mv "$$$$temporary" "$$$$destination"; temporary=; trap - EXIT HUP INT TERM
endef

$(eval $(call ZEDBSD_BUILD_SYSROOT,$(ZEDBSD_SYSROOT_X86_64),x86_64,x86_64-unknown-zedbsd,-m64 -march=x86-64 -mno-red-zone,HAL_ARCH_X86_64,src/libc/crt/crt0-x86_64.S,src/libc/crt/crt1-x86_64.S,x86_64,user.ld,Advanced Micro Devices X86-64,-DKERN_USER_ABI_LP64,,-mlong-double-64,))
$(eval $(call ZEDBSD_BUILD_SYSROOT,$(ZEDBSD_SYSROOT_I386),i386,i386-unknown-zedbsd,-m32 -march=i386 -msoft-float -mno-mmx -mno-sse -mno-sse2,HAL_ARCH_I386,src/libc/crt/crt0-i386.S,src/libc/crt/crt1-i386.S,pcat,user.ld,Intel 80386,,,-mlong-double-64,))
$(eval $(call ZEDBSD_BUILD_SYSROOT,$(ZEDBSD_SYSROOT_ARM64),arm64,aarch64-unknown-zedbsd,-march=armv8-a -mno-outline-atomics,HAL_ARCH_ARM64,src/libc/crt/crt0-aarch64.S,src/libc/crt/crt1-aarch64.S,arm64,user.ld,AArch64,-DKERN_USER_ABI_AARCH64 -DKERN_USER_ABI_LP64,$(ZEDBSD_SYSROOT_ARM64_LIBC_SOURCES),,$(ZEDBSD_SYSROOT_ARM64_COMPILER_RT_SOURCES),$(ZEDBSD_SYSROOT_ARM64_LLVM_BUILTIN_SOURCES)))
$(eval $(call ZEDBSD_BUILD_SYSROOT,$(ZEDBSD_SYSROOT_RISCV64),riscv64,riscv64-unknown-zedbsd,-march=rv64gc -mabi=lp64d -mno-relax,HAL_ARCH_RISCV64,src/libc/crt/crt0-riscv64.S,src/libc/crt/crt1-riscv64.S,riscv64,user.ld,RISC-V,-DKERN_USER_ABI_RISCV64 -DKERN_USER_ABI_LP64,$(ZEDBSD_SYSROOT_RISCV64_LIBC_SOURCES),,$(ZEDBSD_SYSROOT_RISCV64_COMPILER_RT_SOURCES),))
$(eval $(call ZEDBSD_BUILD_SYSROOT,$(ZEDBSD_SYSROOT_ARMV7),armv7,armv7-unknown-zedbsd-eabihf,$(ZEDBSD_SYSROOT_ARMV7_CODE_FLAGS),HAL_ARCH_ARMV7,src/libc/crt/crt0-armv7.S,src/libc/crt/crt1-armv7.S,armv7,user.ld,ARM,-DKERN_USER_ABI_ARMV7,$(ZEDBSD_SYSROOT_ARMV7_LIBC_SOURCES),,,$(ZEDBSD_SYSROOT_ARMV7_LLVM_BUILTIN_SOURCES),$(ZEDBSD_SYSROOT_ARMV7_LLVM_BUILTIN_FLAGS)))

.PHONY: sysroot-x86_64 sysroot-i386 sysroot-arm64 sysroot-riscv64 sysroot-armv7 sysroots
sysroot-x86_64: $(ZEDBSD_SYSROOT_X86_64)/.zedbsd-sysroot-complete
sysroot-i386: $(ZEDBSD_SYSROOT_I386)/.zedbsd-sysroot-complete
sysroot-arm64: $(ZEDBSD_SYSROOT_ARM64)/.zedbsd-sysroot-complete
sysroot-riscv64: $(ZEDBSD_SYSROOT_RISCV64)/.zedbsd-sysroot-complete
sysroot-armv7: $(ZEDBSD_SYSROOT_ARMV7)/.zedbsd-sysroot-complete
sysroots: sysroot-x86_64 sysroot-i386 sysroot-arm64 sysroot-riscv64 sysroot-armv7

$(ZEDBSD_SYSROOT_X86_64)/usr/lib/crt0.o \
$(ZEDBSD_SYSROOT_X86_64)/usr/lib/crt1.o \
$(ZEDBSD_SYSROOT_X86_64)/usr/lib/libc.a \
$(ZEDBSD_SYSROOT_X86_64)/usr/lib/libc.o \
$(ZEDBSD_SYSROOT_X86_64)/usr/lib/libzedbsd-compiler-rt.a \
$(ZEDBSD_SYSROOT_X86_64)/usr/lib/libzedbsd-compiler-rt.o \
$(ZEDBSD_SYSROOT_X86_64)/usr/lib/libclang_rt.builtins.a: \
	$(ZEDBSD_SYSROOT_X86_64)/.zedbsd-sysroot-complete
	@test -f '$@'

$(ZEDBSD_SYSROOT_I386)/usr/lib/crt0.o \
$(ZEDBSD_SYSROOT_I386)/usr/lib/crt1.o \
$(ZEDBSD_SYSROOT_I386)/usr/lib/libc.a \
$(ZEDBSD_SYSROOT_I386)/usr/lib/libc.o \
$(ZEDBSD_SYSROOT_I386)/usr/lib/libzedbsd-compiler-rt.a \
$(ZEDBSD_SYSROOT_I386)/usr/lib/libzedbsd-compiler-rt.o \
$(ZEDBSD_SYSROOT_I386)/usr/lib/libclang_rt.builtins.a: \
	$(ZEDBSD_SYSROOT_I386)/.zedbsd-sysroot-complete
	@test -f '$@'

$(ZEDBSD_SYSROOT_ARM64)/usr/lib/crt0.o \
$(ZEDBSD_SYSROOT_ARM64)/usr/lib/crt1.o \
$(ZEDBSD_SYSROOT_ARM64)/usr/lib/libc.a \
$(ZEDBSD_SYSROOT_ARM64)/usr/lib/libc.o \
$(ZEDBSD_SYSROOT_ARM64)/usr/lib/libzedbsd-compiler-rt.a \
$(ZEDBSD_SYSROOT_ARM64)/usr/lib/libzedbsd-compiler-rt.o \
$(ZEDBSD_SYSROOT_ARM64)/usr/lib/libclang_rt.builtins.a: \
	$(ZEDBSD_SYSROOT_ARM64)/.zedbsd-sysroot-complete
	@test -f '$@'

$(ZEDBSD_SYSROOT_RISCV64)/usr/lib/crt0.o \
$(ZEDBSD_SYSROOT_RISCV64)/usr/lib/crt1.o \
$(ZEDBSD_SYSROOT_RISCV64)/usr/lib/libc.a \
$(ZEDBSD_SYSROOT_RISCV64)/usr/lib/libc.o \
$(ZEDBSD_SYSROOT_RISCV64)/usr/lib/libzedbsd-compiler-rt.a \
$(ZEDBSD_SYSROOT_RISCV64)/usr/lib/libzedbsd-compiler-rt.o \
$(ZEDBSD_SYSROOT_RISCV64)/usr/lib/libclang_rt.builtins.a: \
	$(ZEDBSD_SYSROOT_RISCV64)/.zedbsd-sysroot-complete
	@test -f '$@'

$(ZEDBSD_SYSROOT_ARMV7)/usr/lib/crt0.o \
$(ZEDBSD_SYSROOT_ARMV7)/usr/lib/crt1.o \
$(ZEDBSD_SYSROOT_ARMV7)/usr/lib/libc.a \
$(ZEDBSD_SYSROOT_ARMV7)/usr/lib/libc.o \
$(ZEDBSD_SYSROOT_ARMV7)/usr/lib/libzedbsd-compiler-rt.a \
$(ZEDBSD_SYSROOT_ARMV7)/usr/lib/libzedbsd-compiler-rt.o \
$(ZEDBSD_SYSROOT_ARMV7)/usr/lib/libclang_rt.builtins.a: \
	$(ZEDBSD_SYSROOT_ARMV7)/.zedbsd-sysroot-complete
	@test -f '$@'
