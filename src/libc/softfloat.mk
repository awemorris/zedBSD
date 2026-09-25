# Software floating-point support for zedBSD.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The compiler ABI runtime is integer-only.  libc math and string conversion
# share its IEEE packing primitives, so this build has no external numerical
# source dependency.

ZEDBSD_SOFTFLOAT_BUILD_DIR := $(BUILD)/softfloat
ZEDBSD_SOFTFLOAT_CC ?= $(CC)
ZEDBSD_SOFTFLOAT_OBJDUMP ?= $(if $(OBJDUMP),$(OBJDUMP),objdump)

ZEDBSD_COMPILER_RT_SOURCES := src/libc/softfloat.c \
	src/libc/compiler-runtime.c
ZEDBSD_COMPILER_RT_OBJECTS := $(patsubst src/libc/%.c,\
	$(ZEDBSD_SOFTFLOAT_BUILD_DIR)/%.o,$(ZEDBSD_COMPILER_RT_SOURCES))
ZEDBSD_LIBM_OBJECT := $(ZEDBSD_SOFTFLOAT_BUILD_DIR)/math.o
ZEDBSD_FLOAT_PARSE_OBJECT := $(ZEDBSD_SOFTFLOAT_BUILD_DIR)/float-parse.o
ZEDBSD_SOFTFLOAT_OBJECTS := $(ZEDBSD_COMPILER_RT_OBJECTS) \
	$(ZEDBSD_LIBM_OBJECT) $(ZEDBSD_FLOAT_PARSE_OBJECT)

ZEDBSD_SOFTFLOAT_CFLAGS := $(ZEDBSD_LIBC_CFLAGS) -mlong-double-64
ZEDBSD_SOFTFLOAT_DEPFLAGS := -MMD -MP

# These objects are built by custom rules rather than the generic C rule.  Keep
# an explicit dependency on the public errno ABI so an existing build tree is
# rebuilt when errno changes, and emit normal compiler dependency files for
# subsequent header changes.
$(ZEDBSD_SOFTFLOAT_OBJECTS): include/libc/errno.h

$(ZEDBSD_SOFTFLOAT_BUILD_DIR)/%.o: src/libc/%.c \
	src/libc/softfloat.h
	@mkdir -p $(ZEDBSD_SOFTFLOAT_BUILD_DIR)
	$(ZEDBSD_SOFTFLOAT_CC) -nostdinc -Iinclude/libc -Iinclude -I. \
		$(ZEDBSD_SOFTFLOAT_CFLAGS) \
		$(ZEDBSD_SOFTFLOAT_DEPFLAGS) -c $< -o $@

$(ZEDBSD_FLOAT_PARSE_OBJECT): src/libc/float-parse.c \
	src/libc/softfloat.h
	@mkdir -p $(ZEDBSD_SOFTFLOAT_BUILD_DIR)
	$(ZEDBSD_SOFTFLOAT_CC) -nostdinc -Iinclude/libc -Iinclude -I. \
		$(ZEDBSD_SOFTFLOAT_CFLAGS) $(ZEDBSD_SOFTFLOAT_DEPFLAGS) \
		-c $< -o $@

$(ZEDBSD_LIBM_OBJECT): src/libc/math.c src/libc/softfloat.h
	@mkdir -p $(ZEDBSD_SOFTFLOAT_BUILD_DIR)
	$(ZEDBSD_SOFTFLOAT_CC) -nostdinc -Iinclude/libc -Iinclude -I. \
		$(ZEDBSD_SOFTFLOAT_CFLAGS) $(ZEDBSD_SOFTFLOAT_DEPFLAGS) \
		-c $< -o $@

softfloat-objects: $(ZEDBSD_SOFTFLOAT_OBJECTS)

softfloat-opcode-check: softfloat-objects
	@if $(ZEDBSD_SOFTFLOAT_OBJDUMP) -d --no-show-raw-insn \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) | \
		grep -E '(^[[:space:]]*[0-9a-f]+:[[:space:]]+f[a-z0-9]+[[:space:]])|\b(bswap|cmpxchg|xadd|cmov[a-z]*|rdtsc|ud2|cpuid|fx[a-z]+|movaps|movups|xmm[0-9]|ymm[0-9]|zmm[0-9])\b'; then \
		echo "ERROR: soft-float objects contain a post-i386 opcode" >&2; \
		exit 1; \
	fi
	@echo "zedBSD soft-float i386 opcode check: PASS"

softfloat-host-test: softfloat-core-test softfloat128-core-test \
	float-parse-host-test math-host-test
	@echo "zedBSD independent numerical runtime tests: PASS"

$(BUILD)/tests/softfloat-core-test: tests/softfloat-core-test.c \
	src/libc/softfloat.c src/libc/softfloat.h src/libc/fenv.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -Iinclude/libc -DKERN_UAPI_NATIVE \
		-Iinclude -I. \
		tests/softfloat-core-test.c src/libc/softfloat.c \
		src/libc/fenv.c -o $@

softfloat-core-test: $(BUILD)/tests/softfloat-core-test
	$(BUILD)/tests/softfloat-core-test
	@echo "zedBSD integer soft-float core tests: PASS"

$(BUILD)/tests/float-parse-host-test: tests/float-parse-host-test.c \
	src/libc/float-parse.c src/libc/softfloat.c \
	src/libc/softfloat.h src/libc/fenv.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -Iinclude/libc -DKERN_UAPI_NATIVE \
		-Iinclude -I. \
		tests/float-parse-host-test.c src/libc/float-parse.c \
		src/libc/softfloat.c src/libc/fenv.c -o $@

float-parse-host-test: $(BUILD)/tests/float-parse-host-test
	$(BUILD)/tests/float-parse-host-test
	@echo "zedBSD floating string conversion tests: PASS"

$(BUILD)/tests/math-host-test: tests/math-host-test.c src/libc/math.c \
	src/libc/softfloat.c src/libc/softfloat.h src/libc/fenv.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -fno-builtin -Wall -Wextra -Werror \
		-Iinclude/libc -DKERN_UAPI_NATIVE -Iinclude -I. tests/math-host-test.c src/libc/math.c \
		src/libc/softfloat.c src/libc/fenv.c -o $@

math-host-test: $(BUILD)/tests/math-host-test
	$(BUILD)/tests/math-host-test
	@echo "zedBSD math tests: PASS"

$(BUILD)/tests/softfloat128-core-test: \
	tests/softfloat128-core-test.c src/libc/softfloat128.c \
	src/libc/softfloat128.h src/libc/softfloat.c \
	src/libc/softfloat.h src/libc/fenv.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -Iinclude/libc -DKERN_UAPI_NATIVE \
		-Iinclude -I. \
		tests/softfloat128-core-test.c src/libc/softfloat128.c \
		src/libc/softfloat.c src/libc/fenv.c -o $@

softfloat128-core-test: $(BUILD)/tests/softfloat128-core-test
	$(BUILD)/tests/softfloat128-core-test
	@echo "zedBSD binary128 core tests: PASS"

.PHONY: softfloat-objects softfloat-opcode-check softfloat-host-test \
	softfloat-core-test softfloat128-core-test \
	float-parse-host-test math-host-test
