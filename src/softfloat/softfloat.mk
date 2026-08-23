# Software floating-point support for zedBSD.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The compiler ABI runtime is integer-only.  libc math and string conversion
# share its IEEE packing primitives, so this build has no external numerical
# source dependency.

ZEDBSD_SOFTFLOAT_BUILD_DIR := $(BUILD)/softfloat
ZEDBSD_SOFTFLOAT_CC ?= $(CC)
ZEDBSD_SOFTFLOAT_OBJDUMP ?= objdump

ZEDBSD_COMPILER_RT_SOURCES := src/softfloat/zed-softfloat.c \
	src/softfloat/compiler-runtime.c
ZEDBSD_COMPILER_RT_OBJECTS := $(patsubst src/softfloat/%.c,\
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
$(ZEDBSD_SOFTFLOAT_OBJECTS): libc/include/errno.h

$(ZEDBSD_SOFTFLOAT_BUILD_DIR)/%.o: src/softfloat/%.c \
	src/softfloat/zed-softfloat.h
	@mkdir -p $(ZEDBSD_SOFTFLOAT_BUILD_DIR)
	$(ZEDBSD_SOFTFLOAT_CC) -nostdinc -Ilibc/include -I. \
		$(ZEDBSD_SOFTFLOAT_CFLAGS) \
		$(ZEDBSD_SOFTFLOAT_DEPFLAGS) -c $< -o $@

$(ZEDBSD_FLOAT_PARSE_OBJECT): libc/float-parse.c \
	src/softfloat/zed-softfloat.h
	@mkdir -p $(ZEDBSD_SOFTFLOAT_BUILD_DIR)
	$(ZEDBSD_SOFTFLOAT_CC) -nostdinc -Ilibc/include -I. \
		$(ZEDBSD_SOFTFLOAT_CFLAGS) $(ZEDBSD_SOFTFLOAT_DEPFLAGS) \
		-c $< -o $@

$(ZEDBSD_LIBM_OBJECT): libc/math.c src/softfloat/zed-softfloat.h
	@mkdir -p $(ZEDBSD_SOFTFLOAT_BUILD_DIR)
	$(ZEDBSD_SOFTFLOAT_CC) -nostdinc -Ilibc/include -I. \
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

softfloat-host-test: zed-softfloat-core-test zed-softfloat128-core-test \
	float-parse-host-test math-host-test
	@echo "zedBSD independent numerical runtime tests: PASS"

$(BUILD)/tests/zed-softfloat-core-test: tests/zed-softfloat-core-test.c \
	src/softfloat/zed-softfloat.c src/softfloat/zed-softfloat.h libc/fenv.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -Ilibc/include -I. \
		tests/zed-softfloat-core-test.c src/softfloat/zed-softfloat.c \
		libc/fenv.c -o $@

zed-softfloat-core-test: $(BUILD)/tests/zed-softfloat-core-test
	$(BUILD)/tests/zed-softfloat-core-test
	@echo "zedBSD integer soft-float core tests: PASS"

$(BUILD)/tests/float-parse-host-test: tests/float-parse-host-test.c \
	libc/float-parse.c src/softfloat/zed-softfloat.c \
	src/softfloat/zed-softfloat.h libc/fenv.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -Ilibc/include -I. \
		tests/float-parse-host-test.c libc/float-parse.c \
		src/softfloat/zed-softfloat.c libc/fenv.c -o $@

float-parse-host-test: $(BUILD)/tests/float-parse-host-test
	$(BUILD)/tests/float-parse-host-test
	@echo "zedBSD floating string conversion tests: PASS"

$(BUILD)/tests/math-host-test: tests/math-host-test.c libc/math.c \
	src/softfloat/zed-softfloat.c src/softfloat/zed-softfloat.h libc/fenv.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -fno-builtin -Wall -Wextra -Werror \
		-Ilibc/include -I. tests/math-host-test.c libc/math.c \
		src/softfloat/zed-softfloat.c libc/fenv.c -o $@

math-host-test: $(BUILD)/tests/math-host-test
	$(BUILD)/tests/math-host-test
	@echo "zedBSD math tests: PASS"

$(BUILD)/tests/zed-softfloat128-core-test: \
	tests/zed-softfloat128-core-test.c src/softfloat/zed-softfloat128.c \
	src/softfloat/zed-softfloat128.h src/softfloat/zed-softfloat.c \
	src/softfloat/zed-softfloat.h libc/fenv.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -Ilibc/include -I. \
		tests/zed-softfloat128-core-test.c src/softfloat/zed-softfloat128.c \
		src/softfloat/zed-softfloat.c libc/fenv.c -o $@

zed-softfloat128-core-test: $(BUILD)/tests/zed-softfloat128-core-test
	$(BUILD)/tests/zed-softfloat128-core-test
	@echo "zedBSD binary128 core tests: PASS"

.PHONY: softfloat-objects softfloat-opcode-check softfloat-host-test \
	zed-softfloat-core-test zed-softfloat128-core-test \
	float-parse-host-test math-host-test
