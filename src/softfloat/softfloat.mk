# Software floating-point support for zedBSD.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# GCC soft-fp files are compiled directly from the GCC 14.3 source tree and
# musl math and scanner files from the musl 1.2.6 source tree; both default
# to the vendor/ submodules and can be pointed at existing checkouts with
# ZEDBSD_GCC_ROOT / ZEDBSD_MUSL_ROOT.  Their original source notices remain
# authoritative; the binary distribution carries GCC COPYING.LIB, musl
# COPYRIGHT, and the Noct license.  Keep both lists explicit for provenance
# review.

ZEDBSD_SOFTFLOAT_BUILD_DIR := $(BUILD)/softfloat
ZEDBSD_GCC_ROOT ?= vendor/gcc
ZEDBSD_MUSL_ROOT ?= vendor/musl
ZEDBSD_SOFTFLOAT_CC ?= $(CC)
ZEDBSD_SOFTFLOAT_OBJDUMP ?= objdump

ZEDBSD_GCC_SOFTFP_REL := \
	adddf3.c addsf3.c divdf3.c divsf3.c eqdf2.c eqsf2.c \
	extendsfdf2.c fixdfdi.c fixdfsi.c fixsfdi.c fixsfsi.c \
	fixunsdfdi.c fixunsdfsi.c fixunssfdi.c fixunssfsi.c \
	floatdidf.c floatdisf.c floatsidf.c floatsisf.c \
	floatundidf.c floatundisf.c floatunsidf.c floatunsisf.c \
	gedf2.c gesf2.c ledf2.c lesf2.c muldf3.c mulsf3.c \
	subdf3.c subsf3.c truncdfsf2.c unorddf2.c unordsf2.c

# Each top-level source file is one architecture-neutral musl libm component.
# The vendor subtree is pinned, and architecture-specific subdirectories are
# deliberately excluded so the list remains portable across zedBSD HALs.
ZEDBSD_MUSL_MATH_REL := $(filter-out \
	exp10.c exp10f.c exp10l.c \
	finite.c finitef.c scalb.c scalbf.c significand.c significandf.c \
	sincos.c sincosf.c sincosl.c, \
	$(filter-out __math_divzero.c __math_divzerof.c __math_invalid.c \
	__math_invalidf.c __math_invalidl.c __math_oflow.c __math_oflowf.c \
	__math_uflow.c __math_uflowf.c __math_xflow.c __math_xflowf.c, \
	$(notdir $(sort $(wildcard $(ZEDBSD_MUSL_ROOT)/src/math/*.c)))))

ZEDBSD_GCC_SOFTFP_OBJECTS := $(addprefix $(ZEDBSD_SOFTFLOAT_BUILD_DIR)/gcc-,\
	$(ZEDBSD_GCC_SOFTFP_REL:.c=.o))
ZEDBSD_MUSL_MATH_OBJECTS := $(addprefix $(ZEDBSD_SOFTFLOAT_BUILD_DIR)/musl-,\
	$(ZEDBSD_MUSL_MATH_REL:.c=.o)) \
	$(ZEDBSD_SOFTFLOAT_BUILD_DIR)/musl-math-errors.o
ZEDBSD_MUSL_SCAN_OBJECTS := \
	$(ZEDBSD_SOFTFLOAT_BUILD_DIR)/musl-shgetc.o \
	$(ZEDBSD_SOFTFLOAT_BUILD_DIR)/musl-floatscan.o \
	$(ZEDBSD_SOFTFLOAT_BUILD_DIR)/musl-strtod.o
ZEDBSD_SOFTFLOAT_COMPAT_OBJECT := \
	$(ZEDBSD_SOFTFLOAT_BUILD_DIR)/musl-compat.o
ZEDBSD_SOFTFLOAT_OBJECTS := $(ZEDBSD_GCC_SOFTFP_OBJECTS) \
	$(ZEDBSD_MUSL_MATH_OBJECTS) $(ZEDBSD_MUSL_SCAN_OBJECTS) \
	$(ZEDBSD_SOFTFLOAT_COMPAT_OBJECT)

ZEDBSD_GCC_SOFTFP_CPPFLAGS := \
	-nostdinc -Ilibc/include -I. \
	-I$(ZEDBSD_GCC_ROOT)/include -I$(ZEDBSD_GCC_ROOT)/libgcc \
	-I$(ZEDBSD_GCC_ROOT)/libgcc/config/i386 \
	-I$(ZEDBSD_GCC_ROOT)/libgcc/soft-fp -D_SOFT_FLOAT

ZEDBSD_MUSL_CPPFLAGS := \
	-nostdinc -Isrc/softfloat/include -Ilibc/include -I. \
	-I$(ZEDBSD_MUSL_ROOT)/src/internal -I$(ZEDBSD_MUSL_ROOT)/src/math \
	-include src/softfloat/include/features.h

ZEDBSD_SOFTFLOAT_CFLAGS := $(ZEDBSD_LIBC_CFLAGS) -mlong-double-64
ZEDBSD_MUSL_CFLAGS := $(ZEDBSD_SOFTFLOAT_CFLAGS) \
	-Wno-error=unused-but-set-variable -Wno-error=parentheses \
	-Wno-error=unknown-pragmas -Wno-error=maybe-uninitialized \
	-Wno-error=unused-parameter
ZEDBSD_SOFTFLOAT_DEPFLAGS := -MMD -MP

# These objects are built by custom rules rather than the generic C rule.  Keep
# an explicit dependency on the public errno ABI so an existing build tree is
# rebuilt when errno changes, and emit normal compiler dependency files for
# subsequent header changes.
$(ZEDBSD_SOFTFLOAT_OBJECTS): libc/include/errno.h

# GCC soft-fp intentionally shares a signed/unsigned conversion macro.  GCC
# diagnoses its dead sign test for the four unsigned input translations.
$(ZEDBSD_SOFTFLOAT_BUILD_DIR)/gcc-floatundidf.o \
$(ZEDBSD_SOFTFLOAT_BUILD_DIR)/gcc-floatundisf.o \
$(ZEDBSD_SOFTFLOAT_BUILD_DIR)/gcc-floatunsidf.o \
$(ZEDBSD_SOFTFLOAT_BUILD_DIR)/gcc-floatunsisf.o: \
	ZEDBSD_SOFTFLOAT_WARNING_EXCEPTIONS := -Wno-error=type-limits

$(ZEDBSD_SOFTFLOAT_BUILD_DIR)/gcc-%.o: \
	$(ZEDBSD_GCC_ROOT)/libgcc/soft-fp/%.c
	@mkdir -p $(ZEDBSD_SOFTFLOAT_BUILD_DIR)
	$(ZEDBSD_SOFTFLOAT_CC) $(ZEDBSD_GCC_SOFTFP_CPPFLAGS) \
		$(ZEDBSD_SOFTFLOAT_CFLAGS) \
		$(ZEDBSD_SOFTFLOAT_WARNING_EXCEPTIONS) \
		$(ZEDBSD_SOFTFLOAT_DEPFLAGS) -c $< -o $@

$(ZEDBSD_SOFTFLOAT_BUILD_DIR)/musl-%.o: \
	$(ZEDBSD_MUSL_ROOT)/src/math/%.c
	@mkdir -p $(ZEDBSD_SOFTFLOAT_BUILD_DIR)
	$(ZEDBSD_SOFTFLOAT_CC) $(ZEDBSD_MUSL_CPPFLAGS) \
		$(ZEDBSD_MUSL_CFLAGS) $(ZEDBSD_SOFTFLOAT_DEPFLAGS) \
		-c $< -o $@

$(ZEDBSD_SOFTFLOAT_BUILD_DIR)/musl-math-errors.o: \
	src/softfloat/musl-math-errors.c
	@mkdir -p $(ZEDBSD_SOFTFLOAT_BUILD_DIR)
	$(ZEDBSD_SOFTFLOAT_CC) $(ZEDBSD_MUSL_CPPFLAGS) \
		$(ZEDBSD_MUSL_CFLAGS) $(ZEDBSD_SOFTFLOAT_DEPFLAGS) \
		-c $< -o $@

$(ZEDBSD_SOFTFLOAT_BUILD_DIR)/musl-shgetc.o: \
	$(ZEDBSD_MUSL_ROOT)/src/internal/shgetc.c \
	src/softfloat/musl-floatscan.h
	@mkdir -p $(ZEDBSD_SOFTFLOAT_BUILD_DIR)
	$(ZEDBSD_SOFTFLOAT_CC) $(ZEDBSD_MUSL_CPPFLAGS) \
		$(ZEDBSD_MUSL_CFLAGS) \
		-Wno-error=parentheses \
		$(ZEDBSD_SOFTFLOAT_DEPFLAGS) \
		-include src/softfloat/musl-floatscan.h -c $< -o $@

$(ZEDBSD_SOFTFLOAT_BUILD_DIR)/musl-floatscan.o: \
	$(ZEDBSD_MUSL_ROOT)/src/internal/floatscan.c \
	src/softfloat/musl-floatscan.h
	@mkdir -p $(ZEDBSD_SOFTFLOAT_BUILD_DIR)
	$(ZEDBSD_SOFTFLOAT_CC) $(ZEDBSD_MUSL_CPPFLAGS) \
		$(ZEDBSD_MUSL_CFLAGS) \
		-Wno-error=parentheses -Wno-error=sign-compare \
		$(ZEDBSD_SOFTFLOAT_DEPFLAGS) \
		-include src/softfloat/musl-floatscan.h -c $< -o $@

$(ZEDBSD_SOFTFLOAT_BUILD_DIR)/musl-strtod.o: \
	$(ZEDBSD_MUSL_ROOT)/src/stdlib/strtod.c \
	src/softfloat/musl-floatscan.h
	@mkdir -p $(ZEDBSD_SOFTFLOAT_BUILD_DIR)
	$(ZEDBSD_SOFTFLOAT_CC) $(ZEDBSD_MUSL_CPPFLAGS) \
		$(ZEDBSD_MUSL_CFLAGS) \
		$(ZEDBSD_SOFTFLOAT_DEPFLAGS) \
		-include src/softfloat/musl-floatscan.h -c $< -o $@

$(ZEDBSD_SOFTFLOAT_COMPAT_OBJECT): src/softfloat/musl-compat.c \
	src/softfloat/musl-floatscan.h
	@mkdir -p $(ZEDBSD_SOFTFLOAT_BUILD_DIR)
	$(ZEDBSD_SOFTFLOAT_CC) $(ZEDBSD_MUSL_CPPFLAGS) \
		$(ZEDBSD_MUSL_CFLAGS) \
		$(ZEDBSD_SOFTFLOAT_DEPFLAGS) \
		-include src/softfloat/musl-floatscan.h -c $< -o $@

softfloat-objects: $(ZEDBSD_SOFTFLOAT_OBJECTS)

softfloat-opcode-check: softfloat-objects
	@if $(ZEDBSD_SOFTFLOAT_OBJDUMP) -d --no-show-raw-insn \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) | \
		grep -E '(^[[:space:]]*[0-9a-f]+:[[:space:]]+f[a-z0-9]+[[:space:]])|\b(bswap|cmpxchg|xadd|cmov[a-z]*|rdtsc|ud2|cpuid|fx[a-z]+|movaps|movups|xmm[0-9]|ymm[0-9]|zmm[0-9])\b'; then \
		echo "ERROR: soft-float objects contain a post-i386 opcode" >&2; \
		exit 1; \
	fi
	@echo "zedBSD soft-float i386 opcode check: PASS"

$(BUILD)/tests/softfloat-host-test: tests/softfloat-host-test.c \
	$(ZEDBSD_LIBC_OBJECTS) $(ZEDBSD_SOFTFLOAT_OBJECTS) src/kern/fs.c \
	src/kern/namespace.c $(BUILD)/src/kern/disk.o \
	$(BUILD)/src/kern/buf.o \
	$(BUILD)/src/kern/inode.o $(BUILD)/src/kern/file.o \
	$(BUILD)/src/kern/namecache.o $(BUILD)/src/kern/namei.o \
	$(BUILD)/src/kern/mount.o $(BUILD)/src/kern/rootfs.o \
	tests/vfs-host-stubs.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(ZEDBSD_LIBC_CPPFLAGS) $(ZEDBSD_SOFTFLOAT_CFLAGS) \
		-no-pie \
		tests/softfloat-host-test.c src/kern/fs.c src/kern/namespace.c \
		$(BUILD)/src/kern/disk.o $(BUILD)/src/kern/buf.o \
		$(BUILD)/src/kern/inode.o \
		$(BUILD)/src/kern/file.o $(BUILD)/src/kern/namecache.o \
		$(BUILD)/src/kern/namei.o $(BUILD)/src/kern/mount.o \
		$(BUILD)/src/kern/rootfs.o \
		tests/vfs-host-stubs.c \
		$(ZEDBSD_LIBC_OBJECTS) \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@

softfloat-host-test: $(BUILD)/tests/softfloat-host-test
	$(BUILD)/tests/softfloat-host-test
	@echo "zedBSD soft-float known-vector tests: PASS"

.PHONY: softfloat-objects softfloat-opcode-check softfloat-host-test
