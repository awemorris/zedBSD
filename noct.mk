# Selected Noct core for zedBSD.
#
# M2 deliberately compiled these objects without linking them into vmunix.
# M3 kept that boundary and performed a relocatable link audit.  M4 linked the
# interpreter into vmunix, M5 added soft-float, and M6 enables the i386 JIT.
# Generated lexer/parser C sources are imported and used directly, so flex and
# bison are not build dependencies.

NOCT_ROOT ?= build/sources/noct
HOLORIS_NOCT := $(NOCT_ROOT)/apps/holoris/holoris.noct
NOCT_ENABLE_JIT ?= 1
NOCT_OPTIMIZE_LEVEL ?= 1
# Compile every zedBSD build with the largest supported reservation.  The
# installed-RAM profile selects a smaller per-VM reservation at runtime.
NOCT_JIT_CODE_MAX ?= 2097152
NOCT_PROFILE := $(if $(filter 1,$(NOCT_ENABLE_JIT)),jit-$(NOCT_JIT_CODE_MAX),nojit)-opt-$(NOCT_OPTIMIZE_LEVEL)
NOCT_BUILD_DIR := $(BUILD)/noct-$(NOCT_PROFILE)
NOCT_CC ?= $(CC)
NOCT_OBJDUMP ?= objdump
NOCT_SIZE ?= size
NOCT_LD ?= $(LD)
NOCT_NM ?= nm

NOCT_SOURCE_REL := \
	src/core/lexer.yy.c \
	src/core/parser.tab.c \
	src/core/ast.c \
	src/core/accel_ops.c \
	src/core/accel_program.c \
	src/core/fast.c \
	src/core/hir.c \
	src/core/hir_loop_analyze.c \
	src/core/hir_doall.c \
	src/core/hir_dosum.c \
	src/core/hir_opt_accel.c \
	src/core/hir_gpu.c \
	src/core/gpu_ir.c \
	src/core/gpu_glsl.c \
	src/core/lir.c \
	src/core/noct.c \
	src/core/noct-api.c \
	src/core/dynlib.c \
	src/core/runtime.c \
	src/core/module.c \
	src/core/interpreter.c \
	src/core/jit.c \
	src/core/execution.c \
	src/core/gc.c \
	src/core/intrinsics.c \
	src/core/objectmodel-st.c \
	src/core/objectmodel-dispatch.c \
	src/core/sha256.c \
	src/repl/repl.c \
	src/api/accel.c \
	src/api/regex.c \
	src/api/api-file.c \
	src/api/api-binary.c \
	src/api/api-term-backend.c \
	src/api/beui-core.c \
	src/api/beui-image.c \
	src/api/api-beui-backend.c \
	src/api/jisx0208.c

NOCT_SOURCES := $(addprefix $(NOCT_ROOT)/,$(NOCT_SOURCE_REL))
$(NOCT_SOURCES): | $(NOCT_SOURCE_STAMP)
	@test -e $@ || { echo "Noct source is missing after clone: $@" >&2; exit 1; }

# Host/graphics tests use files outside NOCT_SOURCE_REL.
$(NOCT_ROOT)/tests/%: | $(NOCT_SOURCE_STAMP)
	@test -e $@ || { echo "Noct test source is missing after clone: $@" >&2; exit 1; }
NOCT_CORE_SOURCES := $(filter $(NOCT_ROOT)/src/core/%,$(NOCT_SOURCES))
NOCT_REPL_SOURCES := $(filter $(NOCT_ROOT)/src/repl/%,$(NOCT_SOURCES))
NOCT_API_SOURCES := $(filter $(NOCT_ROOT)/src/api/%,$(NOCT_SOURCES))
NOCT_OBJECTS := \
	$(patsubst $(NOCT_ROOT)/src/core/%.c,$(NOCT_BUILD_DIR)/%.o,$(NOCT_CORE_SOURCES)) \
	$(patsubst $(NOCT_ROOT)/src/repl/%.c,$(NOCT_BUILD_DIR)/repl-%.o,$(NOCT_REPL_SOURCES)) \
	$(patsubst $(NOCT_ROOT)/src/api/%.c,$(NOCT_BUILD_DIR)/%.o,$(NOCT_API_SOURCES))
-include $(NOCT_OBJECTS:.o=.d)
NOCT_UPSTREAM_COMMIT := $(shell git -C $(NOCT_ROOT) rev-parse HEAD 2>/dev/null || echo unknown)

NOCT_CPPFLAGS := \
	-nostdinc \
	-I. \
	-I$(BUILD) \
	-Ilibc/include \
	-I$(NOCT_ROOT)/include \
	-I$(NOCT_ROOT)/src/core \
	-I$(NOCT_ROOT)/src/api \
	-DNOCT_TARGET_PC98BE \
	-DNOCT_MEMORY_SMALL \
	-DNOCT_FORCE_SOFT_FMAF \
	-DZEDBSD_NOCT_OPTIMIZE_LEVEL=$(NOCT_OPTIMIZE_LEVEL) \
	-DNOCT_JIT_CODE_MAX=$(NOCT_JIT_CODE_MAX) \
	-DZEDBSD_NOCT_JIT_CODE_MAX=$(NOCT_JIT_CODE_MAX) \
	-DHAVE_STDINT_H=1 \
	-DHAVE_INTTYPES_H=1 \
	-DHAVE_SYS_TYPES_H=1 \
	-DHAVE_STDBOOL_H=1

ifeq ($(NOCT_ENABLE_JIT),1)
NOCT_CPPFLAGS += -DNOCT_USE_JIT
endif

NOCT_CFLAGS := \
	-m32 -march=i386 -Os -ffreestanding -fno-builtin \
	-ffunction-sections -fdata-sections \
	-fno-pic -fno-pie -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-isolate-erroneous-paths-dereference -fno-strict-aliasing \
	-msoft-float -mno-80387 -mno-fp-ret-in-387 \
	-mno-mmx -mno-sse -mno-sse2 \
	-Wall -Wextra -Werror

# The approved upstream snapshot predates a warning-clean release build.
# Keep -Werror for every other diagnostic, but leave these exact warnings
# visible in the named translation units so the vendored snapshot can remain
# byte-for-byte identical to upstream.
$(NOCT_BUILD_DIR)/noct.o: NOCT_WARNING_EXCEPTIONS := \
	-Wno-error=unused-parameter
$(NOCT_BUILD_DIR)/runtime.o: NOCT_WARNING_EXCEPTIONS := \
	-Wno-error=maybe-uninitialized
$(NOCT_BUILD_DIR)/dynlib.o: NOCT_WARNING_EXCEPTIONS := \
	-Wno-error=unused-function
$(NOCT_BUILD_DIR)/jit.o: NOCT_WARNING_EXCEPTIONS := \
	-Wno-error=unused-parameter -Wno-error=sign-compare
$(NOCT_BUILD_DIR)/intrinsics.o: NOCT_WARNING_EXCEPTIONS := \
	-Wno-error=type-limits
$(NOCT_BUILD_DIR)/objectmodel-st.o: NOCT_WARNING_EXCEPTIONS := \
	-Wno-error=maybe-uninitialized
$(NOCT_BUILD_DIR)/hir_opt_accel.o: NOCT_WARNING_EXCEPTIONS := \
	-Wno-error=maybe-uninitialized

$(NOCT_BUILD_DIR)/%.o: $(NOCT_ROOT)/src/core/%.c
	@mkdir -p $(NOCT_BUILD_DIR)
	$(NOCT_CC) $(NOCT_CPPFLAGS) $(NOCT_CFLAGS) \
		$(NOCT_WARNING_EXCEPTIONS) -MMD -MP -c $< -o $@

$(NOCT_BUILD_DIR)/api-%.o: $(NOCT_ROOT)/src/api/api-%.c
	@mkdir -p $(NOCT_BUILD_DIR)
	$(NOCT_CC) $(NOCT_CPPFLAGS) $(NOCT_CFLAGS) \
		$(NOCT_WARNING_EXCEPTIONS) -MMD -MP -c $< -o $@

# BeUI's core and its display backends.  The backends a target selects are
# listed by that target's kernel.mk; the rule is shared because they are
# ordinary upstream API sources.
$(NOCT_BUILD_DIR)/beui-%.o: $(NOCT_ROOT)/src/api/beui-%.c
	@mkdir -p $(NOCT_BUILD_DIR)
	$(NOCT_CC) $(NOCT_CPPFLAGS) $(NOCT_CFLAGS) \
		$(NOCT_WARNING_EXCEPTIONS) -MMD -MP -c $< -o $@

$(NOCT_BUILD_DIR)/regex.o: $(NOCT_ROOT)/src/api/regex.c
	@mkdir -p $(NOCT_BUILD_DIR)
	$(NOCT_CC) $(NOCT_CPPFLAGS) $(NOCT_CFLAGS) \
		$(NOCT_WARNING_EXCEPTIONS) -MMD -MP -c $< -o $@

$(NOCT_BUILD_DIR)/accel.o: $(NOCT_ROOT)/src/api/accel.c
	@mkdir -p $(NOCT_BUILD_DIR)
	$(NOCT_CC) $(NOCT_CPPFLAGS) $(NOCT_CFLAGS) \
		$(NOCT_WARNING_EXCEPTIONS) -MMD -MP -c $< -o $@

$(NOCT_BUILD_DIR)/jisx0208.o: $(NOCT_ROOT)/src/api/jisx0208.c
	@mkdir -p $(NOCT_BUILD_DIR)
	$(NOCT_CC) $(NOCT_CPPFLAGS) $(NOCT_CFLAGS) \
		$(NOCT_WARNING_EXCEPTIONS) -MMD -MP -c $< -o $@

$(NOCT_BUILD_DIR)/repl-%.o: $(NOCT_ROOT)/src/repl/%.c
	@mkdir -p $(NOCT_BUILD_DIR)
	$(NOCT_CC) $(NOCT_CPPFLAGS) $(NOCT_CFLAGS) \
		$(NOCT_WARNING_EXCEPTIONS) -MMD -MP -c $< -o $@

noct-objects: $(NOCT_OBJECTS)
	@echo "Noct upstream: $(NOCT_UPSTREAM_COMMIT)"
	@echo "Noct profile: $(NOCT_PROFILE), JIT code arena: $(NOCT_JIT_CODE_MAX) bytes"
	@$(NOCT_SIZE) --totals $(NOCT_OBJECTS) | tail -1

noct-opcode-check: noct-objects
	@# CPUID is reached only after jit_x86_has_cpuid() verifies that EFLAGS.ID
	@# is writable. Old i386 CPUs therefore return with SIMD capabilities 0.
	@if $(NOCT_OBJDUMP) -d --no-show-raw-insn $(NOCT_OBJECTS) | \
		grep -E '(^[[:space:]]*[0-9a-f]+:[[:space:]]+f[a-z0-9]+[[:space:]])|\b(bswap|cmpxchg|xadd|cmov[a-z]*|rdtsc|ud2|fx[a-z]+|movaps|movups|xmm[0-9]|ymm[0-9]|zmm[0-9])\b'; then \
		echo "ERROR: selected Noct objects contain a post-i386 opcode" >&2; \
		exit 1; \
	fi
	@echo "Noct i386 opcode check: PASS"

NOCT_M3_RELOC := $(BUILD)/noct-libc-m3.o
NOCT_M3_UNDEFINED := $(BUILD)/noct-libc-m3.undefined

noct-link-audit: noct-objects libc-objects $(BUILD)/src/kern/env.o \
	$(BUILD)/src/kern/fs.o $(BUILD)/src/kern/namespace.o
	@mkdir -p $(dir $(NOCT_M3_RELOC))
	$(NOCT_LD) -m elf_i386 -r $(NOCT_OBJECTS) $(ZEDBSD_LIBC_OBJECTS) \
		$(BUILD)/src/kern/env.o $(BUILD)/src/kern/fs.o \
		$(BUILD)/src/kern/namespace.o \
		-o $(NOCT_M3_RELOC)
	@$(NOCT_NM) -u $(NOCT_M3_RELOC) | awk '{print $$NF}' | sort -u > \
		$(NOCT_M3_UNDEFINED)
	@if grep -Ev -f libc/deferred-symbols.regex $(NOCT_M3_UNDEFINED) | \
		grep -q .; then \
		echo "ERROR: unexpected undefined symbols in Noct/libc M3 object:" >&2; \
		grep -Ev -f libc/deferred-symbols.regex $(NOCT_M3_UNDEFINED) >&2; \
		exit 1; \
	fi
	@echo "Noct/libc unresolved-symbol audit: PASS"
	@if test -s $(NOCT_M3_UNDEFINED); then \
		echo "Deferred platform/soft-float/math symbols:"; \
		sed 's/^/  /' $(NOCT_M3_UNDEFINED); \
	else \
		echo "Deferred platform/soft-float/math symbols: none"; \
	fi

noct-m3-verify: libc-host-test libc-opcode-check \
	noct-opcode-check noct-link-audit
	@echo "zedBSD M3 historical boundary checks: PASS"

.PHONY: noct-objects noct-opcode-check noct-link-audit noct-m3-verify

# Static ring-3 Noct build.  Keep its target macros and object directory
# separate from the transitional in-kernel PC98BE build until parity is proven.
USER_NOCT_BUILD_DIR := $(BUILD)/noct-user
USER_NOCT_OBJECTS := \
	$(patsubst $(NOCT_ROOT)/src/core/%.c,$(USER_NOCT_BUILD_DIR)/%.o,$(NOCT_CORE_SOURCES)) \
	$(patsubst $(NOCT_ROOT)/src/repl/%.c,$(USER_NOCT_BUILD_DIR)/repl-%.o,$(NOCT_REPL_SOURCES)) \
	$(patsubst $(NOCT_ROOT)/src/api/%.c,$(USER_NOCT_BUILD_DIR)/%.o,$(NOCT_API_SOURCES))
USER_NOCT_CPPFLAGS := -nostdinc -Iuserland/include -Iinclude/uapi -I. \
	-I$(BUILD) -Ilibc/include -I$(NOCT_ROOT)/include \
	-I$(NOCT_ROOT)/src/core -I$(NOCT_ROOT)/src/api \
	-DNOCT_TARGET_POSIX -DNOCT_TARGET_ZEDBSD -DNOCT_MEMORY_SMALL \
	-DNOCT_USE_JIT -DNOCT_FORCE_SOFT_FMAF \
	-DHAVE_STDINT_H=1 -DHAVE_INTTYPES_H=1 \
	-DHAVE_SYS_TYPES_H=1 -DHAVE_STDBOOL_H=1 -U__linux__ -Ulinux
USER_NOCT_CFLAGS := $(NOCT_CFLAGS)

$(USER_NOCT_BUILD_DIR)/noct.o: USER_NOCT_WARN := -Wno-error=unused-parameter
$(USER_NOCT_BUILD_DIR)/runtime.o: USER_NOCT_WARN := -Wno-error=maybe-uninitialized
$(USER_NOCT_BUILD_DIR)/dynlib.o: USER_NOCT_WARN := -Wno-error=unused-function
$(USER_NOCT_BUILD_DIR)/jit.o: USER_NOCT_WARN := -Wno-error=unused-parameter -Wno-error=sign-compare
$(USER_NOCT_BUILD_DIR)/intrinsics.o: USER_NOCT_WARN := -Wno-error=type-limits
$(USER_NOCT_BUILD_DIR)/objectmodel-st.o: USER_NOCT_WARN := -Wno-error=maybe-uninitialized
$(USER_NOCT_BUILD_DIR)/hir_opt_accel.o: USER_NOCT_WARN := -Wno-error=maybe-uninitialized

$(USER_NOCT_BUILD_DIR)/%.o: $(NOCT_ROOT)/src/core/%.c
	@mkdir -p $(USER_NOCT_BUILD_DIR)
	$(NOCT_CC) $(USER_NOCT_CPPFLAGS) $(USER_NOCT_CFLAGS) $(USER_NOCT_WARN) \
		-MMD -MP -c $< -o $@
$(USER_NOCT_BUILD_DIR)/api-%.o: $(NOCT_ROOT)/src/api/api-%.c
	@mkdir -p $(USER_NOCT_BUILD_DIR)
	$(NOCT_CC) $(USER_NOCT_CPPFLAGS) $(USER_NOCT_CFLAGS) $(USER_NOCT_WARN) \
		-MMD -MP -c $< -o $@
$(USER_NOCT_BUILD_DIR)/beui-%.o: $(NOCT_ROOT)/src/api/beui-%.c
	@mkdir -p $(USER_NOCT_BUILD_DIR)
	$(NOCT_CC) $(USER_NOCT_CPPFLAGS) $(USER_NOCT_CFLAGS) $(USER_NOCT_WARN) \
		-MMD -MP -c $< -o $@
$(USER_NOCT_BUILD_DIR)/regex.o: $(NOCT_ROOT)/src/api/regex.c
	@mkdir -p $(USER_NOCT_BUILD_DIR)
	$(NOCT_CC) $(USER_NOCT_CPPFLAGS) $(USER_NOCT_CFLAGS) $(USER_NOCT_WARN) \
		-MMD -MP -c $< -o $@
$(USER_NOCT_BUILD_DIR)/accel.o: $(NOCT_ROOT)/src/api/accel.c
	@mkdir -p $(USER_NOCT_BUILD_DIR)
	$(NOCT_CC) $(USER_NOCT_CPPFLAGS) $(USER_NOCT_CFLAGS) $(USER_NOCT_WARN) \
		-MMD -MP -c $< -o $@
$(USER_NOCT_BUILD_DIR)/jisx0208.o: $(NOCT_ROOT)/src/api/jisx0208.c
	@mkdir -p $(USER_NOCT_BUILD_DIR)
	$(NOCT_CC) $(USER_NOCT_CPPFLAGS) $(USER_NOCT_CFLAGS) $(USER_NOCT_WARN) \
		-MMD -MP -c $< -o $@
$(USER_NOCT_BUILD_DIR)/repl-%.o: $(NOCT_ROOT)/src/repl/%.c
	@mkdir -p $(USER_NOCT_BUILD_DIR)
	$(NOCT_CC) $(USER_NOCT_CPPFLAGS) $(USER_NOCT_CFLAGS) $(USER_NOCT_WARN) \
		-MMD -MP -c $< -o $@

-include $(USER_NOCT_OBJECTS:.o=.d)
