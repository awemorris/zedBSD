# Deterministic zedBSD x86 target sysroots.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ZEDBSD_SYSROOT_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_SYSROOT_AMD64 := $(abspath $(ZEDBSD_LLVM_ROOT)/build/amd64/sysroot)
ZEDBSD_SYSROOT_I386 := $(abspath $(ZEDBSD_LLVM_ROOT)/build/i386/sysroot)
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
	libc/heap.c libc/string.c libc/ctype.c libc/locale.c libc/wide.c \
	libc/int64.c libc/strto.c libc/format.c libc/stdio.c \
	$(ZEDBSD_LIBC_USER_EXTRA_SOURCES)

ZEDBSD_SYSROOT_COMPILER_RT_SOURCES := \
	src/softfloat/zed-softfloat.c \
	src/softfloat/compiler-runtime.c \
	libc/math.c libc/float-parse.c

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

# A clean cache-based bootstrap has build/llvm but no extracted LLVM source.
# Give each compiler-rt input a real generating prerequisite so parallel Make
# completes the verified source extraction before it diagnoses a missing file.
$(ZEDBSD_SYSROOT_LLVM_BUILTIN_SOURCES): | $(ZEDBSD_LLVM_SOURCE_STAMP)
	@test -f '$@'

ZEDBSD_SYSROOT_PUBLIC_HEADERS := $(shell \
	find libc/include include/uapi -type f -print | LC_ALL=C sort)
ZEDBSD_SYSROOT_LINKER_SCRIPTS := \
	platform/amd64/user.ld platform/amd64/vmunix.ld \
	platform/pcat/user.ld platform/pcat/vmunix.ld \
	platform/pc98/noct-user.ld platform/pc98/user-init.ld \
	platform/pc98/stage2.ld \
	bootloader/pcat/stage1.ld bootloader/pcat/stage2.ld \
	bootloader/pcat/bootzbsd.ld \
	bootloader/pc98/stage1.ld bootloader/pc98/stage2.ld
ZEDBSD_SYSROOT_INPUTS := $(ZEDBSD_SYSROOT_LIBC_SOURCES) \
	$(ZEDBSD_SYSROOT_COMPILER_RT_SOURCES) \
	$(ZEDBSD_SYSROOT_LLVM_BUILTIN_SOURCES) \
	$(ZEDBSD_SYSROOT_PUBLIC_HEADERS) $(ZEDBSD_SYSROOT_LINKER_SCRIPTS) \
	src/crt/crt0-amd64.S src/crt/crt1-amd64.S \
	src/crt/crt0.S src/crt/crt1-i386.S \
	include/hal/arch.h include/hal/arch/amd64.h include/hal/arch/i386.h

define ZEDBSD_BUILD_X86_SYSROOT
$(1)/.zedbsd-sysroot-complete: $(ZEDBSD_SYSROOT_INPUTS) \
	$(ZEDBSD_LLVM_INSTALL_STAMP)
	@set -eu; \
	destination='$(1)'; parent=$$$${destination%/*}; \
	mkdir -p "$$$$parent"; \
	temporary=$$$$(mktemp -d "$$$$parent/.sysroot-$(2).XXXXXX"); \
	cleanup() { if test -n "$$$$temporary" && test -d "$$$$temporary"; then find "$$$$temporary" -depth -delete; fi; }; \
	trap cleanup EXIT HUP INT TERM; \
	mkdir -p "$$$$temporary/usr/include" "$$$$temporary/usr/lib" \
		"$$$$temporary/usr/lib/zedbsd/amd64" \
		"$$$$temporary/usr/lib/zedbsd/pcat" \
		"$$$$temporary/usr/lib/zedbsd/pc98" "$$$$temporary/obj"; \
	for header in $$(ZEDBSD_SYSROOT_PUBLIC_HEADERS); do \
		case "$$$$header" in \
		libc/include/*) relative=$$$${header#libc/include/} ;; \
		include/uapi/*) relative=$$$${header#include/uapi/} ;; \
		*) echo "sysroot: non-public header in manifest: $$$$header" >&2; exit 1 ;; \
		esac; \
		case "$$$$relative" in */*) mkdir -p "$$$$temporary/usr/include/$$$${relative%/*}" ;; esac; \
		cp "$$$$header" "$$$$temporary/usr/include/$$$$relative"; \
	done; \
	for source in $$(ZEDBSD_SYSROOT_LIBC_SOURCES); do \
		object="$$$$temporary/obj/$$$$source.o"; mkdir -p "$$$${object%/*}"; \
		'$(ZEDBSD_SYSROOT_CLANG)' --target='$(3)' --sysroot="$$$$temporary" \
			$(4) -D$(5) $(11) -nostdinc -Ilibc/include -Iinclude/uapi -Iinclude -Isrc -I. \
			-ffreestanding -fno-builtin -fno-pic -fno-pie \
			-fno-stack-protector -fno-asynchronous-unwind-tables \
			-fno-unwind-tables -fno-common -fno-strict-aliasing \
			-ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror \
			-c "$$$$source" -o "$$$$object"; \
	done; \
	'$(ZEDBSD_SYSROOT_AR)' rcsD "$$$$temporary/usr/lib/libc.a" \
		$$$$(find "$$$$temporary/obj" -type f -name '*.c.o' -print | LC_ALL=C sort); \
	'$(ZEDBSD_SYSROOT_LD)' -r --whole-archive \
		"$$$$temporary/usr/lib/libc.a" --no-whole-archive \
		-o "$$$$temporary/usr/lib/libc.o"; \
	find "$$$$temporary/obj" -depth -delete; mkdir -p "$$$$temporary/obj"; \
	for source in $$(ZEDBSD_SYSROOT_COMPILER_RT_SOURCES); do \
		object="$$$$temporary/obj/$$$$source.o"; mkdir -p "$$$${object%/*}"; \
		'$(ZEDBSD_SYSROOT_CLANG)' --target='$(3)' --sysroot="$$$$temporary" \
			$(4) -D$(5) $(11) -nostdinc -Ilibc/include -Iinclude/uapi -Iinclude -Isrc -I. \
			-ffreestanding -fno-builtin -fno-pic -fno-pie \
			-fno-stack-protector -fno-asynchronous-unwind-tables \
			-fno-unwind-tables -fno-common -fno-strict-aliasing \
			-mlong-double-64 -ffunction-sections -fdata-sections \
			-Os -Wall -Wextra -Werror -c "$$$$source" -o "$$$$object"; \
	done; \
	'$(ZEDBSD_SYSROOT_AR)' rcsD "$$$$temporary/usr/lib/libzedbsd-compiler-rt.a" \
		$$$$(find "$$$$temporary/obj" -type f -name '*.c.o' -print | LC_ALL=C sort); \
	'$(ZEDBSD_SYSROOT_LD)' -r --whole-archive \
		"$$$$temporary/usr/lib/libzedbsd-compiler-rt.a" --no-whole-archive \
		-o "$$$$temporary/usr/lib/libzedbsd-compiler-rt.o"; \
	find "$$$$temporary/obj" -depth -delete; mkdir -p "$$$$temporary/obj"; \
	for source in $$(ZEDBSD_SYSROOT_LLVM_BUILTIN_SOURCES); do \
		object="$$$$temporary/obj/$$$${source##*/}.o"; \
		'$(ZEDBSD_SYSROOT_CLANG)' --target='$(3)' --sysroot="$$$$temporary" \
			$(4) -D$(5) $(11) -nostdinc -isystem "$$$$temporary/usr/include" \
			-I'$(ZEDBSD_LLVM_SOURCE)/compiler-rt/lib/builtins' \
			-ffreestanding -fno-builtin -fno-pic -fno-pie \
			-fno-stack-protector -fno-asynchronous-unwind-tables \
			-fno-unwind-tables -fno-common -fno-strict-aliasing \
			-ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror \
			-Wno-unused-parameter \
			-c "$$$$source" -o "$$$$object"; \
	done; \
	'$(ZEDBSD_SYSROOT_AR)' rcsD "$$$$temporary/usr/lib/libclang_rt.builtins.a" \
		$$$$(find "$$$$temporary/obj" -type f -name '*.c.o' -print | LC_ALL=C sort); \
	'$(ZEDBSD_SYSROOT_CLANG)' --target='$(3)' --sysroot="$$$$temporary" \
		$(4) -nostdinc -Iinclude -Iinclude/uapi -D$(5) $(11) \
		-ffreestanding -fno-pic -fno-pie -fno-stack-protector \
		-c '$(6)' -o "$$$$temporary/usr/lib/crt0.o"; \
	'$(ZEDBSD_SYSROOT_CLANG)' --target='$(3)' --sysroot="$$$$temporary" \
		$(4) -nostdinc -Iinclude -Iinclude/uapi -D$(5) $(11) \
		-ffreestanding -fno-pic -fno-pie -fno-stack-protector \
		-c '$(7)' -o "$$$$temporary/usr/lib/crt1.o"; \
	cp platform/amd64/user.ld platform/amd64/vmunix.ld \
		"$$$$temporary/usr/lib/zedbsd/amd64/"; \
	cp platform/pcat/user.ld platform/pcat/vmunix.ld \
		bootloader/pcat/stage1.ld bootloader/pcat/stage2.ld \
		bootloader/pcat/bootzbsd.ld "$$$$temporary/usr/lib/zedbsd/pcat/"; \
	cp platform/pc98/noct-user.ld platform/pc98/user-init.ld \
		bootloader/pc98/stage1.ld "$$$$temporary/usr/lib/zedbsd/pc98/"; \
	cp platform/pc98/stage2.ld \
		"$$$$temporary/usr/lib/zedbsd/pc98/platform-stage2.ld"; \
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
		$(4) -nostdlib -static -no-pie -Wl,--build-id=none \
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

$(eval $(call ZEDBSD_BUILD_X86_SYSROOT,$(ZEDBSD_SYSROOT_AMD64),amd64,x86_64-unknown-zedbsd,-m64 -march=x86-64 -mno-red-zone,HAL_ARCH_AMD64,src/crt/crt0-amd64.S,src/crt/crt1-amd64.S,amd64,user.ld,Advanced Micro Devices X86-64,-DZEDBSD_USER_ABI_LP64))
$(eval $(call ZEDBSD_BUILD_X86_SYSROOT,$(ZEDBSD_SYSROOT_I386),i386,i386-unknown-zedbsd,-m32 -march=i386 -msoft-float -mno-mmx -mno-sse -mno-sse2,HAL_ARCH_I386,src/crt/crt0.S,src/crt/crt1-i386.S,pcat,user.ld,Intel 80386,))

.PHONY: sysroot-amd64 sysroot-i386 sysroots
sysroot-amd64: $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
sysroot-i386: $(ZEDBSD_SYSROOT_I386)/.zedbsd-sysroot-complete
sysroots: sysroot-amd64 sysroot-i386

$(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt0.o \
$(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
$(ZEDBSD_SYSROOT_AMD64)/usr/lib/libc.a \
$(ZEDBSD_SYSROOT_AMD64)/usr/lib/libc.o \
$(ZEDBSD_SYSROOT_AMD64)/usr/lib/libzedbsd-compiler-rt.a \
$(ZEDBSD_SYSROOT_AMD64)/usr/lib/libzedbsd-compiler-rt.o \
$(ZEDBSD_SYSROOT_AMD64)/usr/lib/libclang_rt.builtins.a: \
	$(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
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
