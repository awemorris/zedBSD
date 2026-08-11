# Software floating-point support for Boots.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# GCC soft-fp files are compiled directly from the GCC 14.3 source tree and
# musl math and scanner files from the musl 1.2.6 source tree; both default
# to the vendor/ submodules and can be pointed at existing checkouts with
# BOOTS_GCC_ROOT / BOOTS_MUSL_ROOT.  Their original source notices remain
# authoritative; the binary distribution carries GCC COPYING.LIB, musl
# COPYRIGHT, and the Noct license.  Keep both lists explicit for provenance
# review.

BOOTS_SOFTFLOAT_BUILD_DIR := $(BUILD)/softfloat
BOOTS_GCC_ROOT ?= vendor/gcc
BOOTS_MUSL_ROOT ?= vendor/musl
BOOTS_SOFTFLOAT_CC ?= $(CC)
BOOTS_SOFTFLOAT_OBJDUMP ?= objdump

BOOTS_GCC_SOFTFP_REL := \
	adddf3.c addsf3.c divdf3.c divsf3.c eqdf2.c eqsf2.c \
	extendsfdf2.c fixdfdi.c fixdfsi.c fixsfdi.c fixsfsi.c \
	fixunsdfdi.c fixunsdfsi.c fixunssfdi.c fixunssfsi.c \
	floatdidf.c floatdisf.c floatsidf.c floatsisf.c \
	floatundidf.c floatundisf.c floatunsidf.c floatunsisf.c \
	gedf2.c gesf2.c ledf2.c lesf2.c muldf3.c mulsf3.c \
	subdf3.c subsf3.c truncdfsf2.c unorddf2.c unordsf2.c

BOOTS_MUSL_MATH_REL := \
	sinf.c cosf.c tanf.c sqrtf.c \
	__sindf.c __cosdf.c __tandf.c __rem_pio2f.c __rem_pio2_large.c \
	__math_invalidf.c sqrt_data.c fmod.c scalbn.c floor.c

BOOTS_GCC_SOFTFP_OBJECTS := $(addprefix $(BOOTS_SOFTFLOAT_BUILD_DIR)/gcc-,\
	$(BOOTS_GCC_SOFTFP_REL:.c=.o))
BOOTS_MUSL_MATH_OBJECTS := $(addprefix $(BOOTS_SOFTFLOAT_BUILD_DIR)/musl-,\
	$(BOOTS_MUSL_MATH_REL:.c=.o))
BOOTS_MUSL_SCAN_OBJECTS := \
	$(BOOTS_SOFTFLOAT_BUILD_DIR)/musl-shgetc.o \
	$(BOOTS_SOFTFLOAT_BUILD_DIR)/musl-floatscan.o \
	$(BOOTS_SOFTFLOAT_BUILD_DIR)/musl-strtod.o
BOOTS_SOFTFLOAT_COMPAT_OBJECT := \
	$(BOOTS_SOFTFLOAT_BUILD_DIR)/musl-compat.o
BOOTS_SOFTFLOAT_OBJECTS := $(BOOTS_GCC_SOFTFP_OBJECTS) \
	$(BOOTS_MUSL_MATH_OBJECTS) $(BOOTS_MUSL_SCAN_OBJECTS) \
	$(BOOTS_SOFTFLOAT_COMPAT_OBJECT)

BOOTS_GCC_SOFTFP_CPPFLAGS := \
	-nostdinc -Ilibc/include -I. \
	-I$(BOOTS_GCC_ROOT)/include -I$(BOOTS_GCC_ROOT)/libgcc \
	-I$(BOOTS_GCC_ROOT)/libgcc/config/i386 \
	-I$(BOOTS_GCC_ROOT)/libgcc/soft-fp -D_SOFT_FLOAT

BOOTS_MUSL_CPPFLAGS := \
	-nostdinc -Isoftfloat/include -Ilibc/include -I. \
	-I$(BOOTS_MUSL_ROOT)/src/internal -I$(BOOTS_MUSL_ROOT)/src/math

BOOTS_SOFTFLOAT_CFLAGS := $(BOOTS_LIBC_CFLAGS) -mlong-double-64
BOOTS_MUSL_CFLAGS := $(BOOTS_SOFTFLOAT_CFLAGS) \
	-Wno-error=unused-but-set-variable

# GCC soft-fp intentionally shares a signed/unsigned conversion macro.  GCC
# diagnoses its dead sign test for the four unsigned input translations.
$(BOOTS_SOFTFLOAT_BUILD_DIR)/gcc-floatundidf.o \
$(BOOTS_SOFTFLOAT_BUILD_DIR)/gcc-floatundisf.o \
$(BOOTS_SOFTFLOAT_BUILD_DIR)/gcc-floatunsidf.o \
$(BOOTS_SOFTFLOAT_BUILD_DIR)/gcc-floatunsisf.o: \
	BOOTS_SOFTFLOAT_WARNING_EXCEPTIONS := -Wno-error=type-limits

$(BOOTS_SOFTFLOAT_BUILD_DIR)/gcc-%.o: \
	$(BOOTS_GCC_ROOT)/libgcc/soft-fp/%.c
	@mkdir -p $(BOOTS_SOFTFLOAT_BUILD_DIR)
	$(BOOTS_SOFTFLOAT_CC) $(BOOTS_GCC_SOFTFP_CPPFLAGS) \
		$(BOOTS_SOFTFLOAT_CFLAGS) \
		$(BOOTS_SOFTFLOAT_WARNING_EXCEPTIONS) -c $< -o $@

$(BOOTS_SOFTFLOAT_BUILD_DIR)/musl-%.o: \
	$(BOOTS_MUSL_ROOT)/src/math/%.c
	@mkdir -p $(BOOTS_SOFTFLOAT_BUILD_DIR)
	$(BOOTS_SOFTFLOAT_CC) $(BOOTS_MUSL_CPPFLAGS) \
		$(BOOTS_MUSL_CFLAGS) -c $< -o $@

$(BOOTS_SOFTFLOAT_BUILD_DIR)/musl-shgetc.o: \
	$(BOOTS_MUSL_ROOT)/src/internal/shgetc.c \
	softfloat/musl-floatscan.h
	@mkdir -p $(BOOTS_SOFTFLOAT_BUILD_DIR)
	$(BOOTS_SOFTFLOAT_CC) $(BOOTS_MUSL_CPPFLAGS) \
		$(BOOTS_MUSL_CFLAGS) \
		-Wno-error=parentheses \
		-include softfloat/musl-floatscan.h -c $< -o $@

$(BOOTS_SOFTFLOAT_BUILD_DIR)/musl-floatscan.o: \
	$(BOOTS_MUSL_ROOT)/src/internal/floatscan.c \
	softfloat/musl-floatscan.h
	@mkdir -p $(BOOTS_SOFTFLOAT_BUILD_DIR)
	$(BOOTS_SOFTFLOAT_CC) $(BOOTS_MUSL_CPPFLAGS) \
		$(BOOTS_MUSL_CFLAGS) \
		-Wno-error=parentheses -Wno-error=sign-compare \
		-include softfloat/musl-floatscan.h -c $< -o $@

$(BOOTS_SOFTFLOAT_BUILD_DIR)/musl-strtod.o: \
	$(BOOTS_MUSL_ROOT)/src/stdlib/strtod.c \
	softfloat/musl-floatscan.h
	@mkdir -p $(BOOTS_SOFTFLOAT_BUILD_DIR)
	$(BOOTS_SOFTFLOAT_CC) $(BOOTS_MUSL_CPPFLAGS) \
		$(BOOTS_MUSL_CFLAGS) \
		-include softfloat/musl-floatscan.h -c $< -o $@

$(BOOTS_SOFTFLOAT_COMPAT_OBJECT): softfloat/musl-compat.c \
	softfloat/musl-floatscan.h
	@mkdir -p $(BOOTS_SOFTFLOAT_BUILD_DIR)
	$(BOOTS_SOFTFLOAT_CC) $(BOOTS_MUSL_CPPFLAGS) \
		$(BOOTS_MUSL_CFLAGS) \
		-include softfloat/musl-floatscan.h -c $< -o $@

softfloat-objects: $(BOOTS_SOFTFLOAT_OBJECTS)

softfloat-opcode-check: softfloat-objects
	@if $(BOOTS_SOFTFLOAT_OBJDUMP) -d --no-show-raw-insn \
		$(BOOTS_SOFTFLOAT_OBJECTS) | \
		grep -E '(^[[:space:]]*[0-9a-f]+:[[:space:]]+f[a-z0-9]+[[:space:]])|\b(bswap|cmpxchg|xadd|cmov[a-z]*|rdtsc|ud2|cpuid|fx[a-z]+|movaps|movups|xmm[0-9]|ymm[0-9]|zmm[0-9])\b'; then \
		echo "ERROR: soft-float objects contain a post-i386 opcode" >&2; \
		exit 1; \
	fi
	@echo "Boots soft-float i386 opcode check: PASS"

$(BUILD)/tests/softfloat-host-test: tests/softfloat-host-test.c \
	$(BOOTS_LIBC_OBJECTS) $(BOOTS_SOFTFLOAT_OBJECTS) src/kern/fs.c \
	src/kern/namespace.c $(BUILD)/src/kern/disk.o \
	$(BUILD)/src/kern/inode.o $(BUILD)/src/kern/file.o \
	$(BUILD)/src/kern/namecache.o $(BUILD)/src/kern/namei.o \
	$(BUILD)/src/kern/mount.o $(BUILD)/src/kern/rootfs.o
	@mkdir -p $(dir $@)
	$(HOSTCC) $(BOOTS_LIBC_CPPFLAGS) $(BOOTS_SOFTFLOAT_CFLAGS) \
		-no-pie \
		tests/softfloat-host-test.c src/kern/fs.c src/kern/namespace.c \
		$(BUILD)/src/kern/disk.o $(BUILD)/src/kern/inode.o \
		$(BUILD)/src/kern/file.o $(BUILD)/src/kern/namecache.o \
		$(BUILD)/src/kern/namei.o $(BUILD)/src/kern/mount.o \
		$(BUILD)/src/kern/rootfs.o \
		$(BOOTS_LIBC_OBJECTS) \
		$(BOOTS_SOFTFLOAT_OBJECTS) -o $@

softfloat-host-test: $(BUILD)/tests/softfloat-host-test
	$(BUILD)/tests/softfloat-host-test
	@echo "Boots soft-float known-vector tests: PASS"

.PHONY: softfloat-objects softfloat-opcode-check softfloat-host-test
