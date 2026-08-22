# Noct userland build for zedBSD.

NOCT_ROOT ?= userland/noct
NOCT_SOURCE_SENTINEL ?= $(NOCT_ROOT)/CMakeLists.txt
HOLORIS_NOCT := $(NOCT_ROOT)/apps/holoris/holoris.noct
NOCT_CC ?= $(CC)
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
$(NOCT_SOURCES): | $(NOCT_SOURCE_SENTINEL)
	@test -e $@ || { echo "Noct submodule source is missing: $@" >&2; exit 1; }

NOCT_CORE_SOURCES := $(filter $(NOCT_ROOT)/src/core/%,$(NOCT_SOURCES))
NOCT_REPL_SOURCES := $(filter $(NOCT_ROOT)/src/repl/%,$(NOCT_SOURCES))
NOCT_API_SOURCES := $(filter $(NOCT_ROOT)/src/api/%,$(NOCT_SOURCES))
# Static ring-3 Noct build.
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
USER_NOCT_CFLAGS := -m32 -march=i386 -Os -ffreestanding -fno-builtin \
	-ffunction-sections -fdata-sections -fno-pic -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-isolate-erroneous-paths-dereference -fno-strict-aliasing \
	-msoft-float -mno-80387 -mno-fp-ret-in-387 \
	-mno-mmx -mno-sse -mno-sse2 -Wall -Wextra -Werror

# The original zedBSD port was i386-only.  amd64 uses the same POSIX target
# integration, but must produce a native LP64 executable because the amd64
# kernel intentionally has no ELF32 compatibility process ABI.
ifeq ($(ARCH),amd64)
USER_NOCT_CPPFLAGS := -nostdinc -Iuserland/include -Iinclude/uapi -I. \
	-I$(BUILD) -Ilibc/include -I$(NOCT_ROOT)/include \
	-I$(NOCT_ROOT)/src/core -I$(NOCT_ROOT)/src/api \
	-DNOCT_TARGET_POSIX -DNOCT_TARGET_ZEDBSD -DNOCT_MEMORY_SMALL \
	-DNOCT_USE_JIT -DNOCT_FORCE_SOFT_FMAF \
	-DHAVE_STDINT_H=1 -DHAVE_INTTYPES_H=1 \
	-DHAVE_SYS_TYPES_H=1 -DHAVE_STDBOOL_H=1 \
	-DZEDBSD_USER_ABI_LP64 -U__linux__ -Ulinux
USER_NOCT_CFLAGS := -m64 -march=x86-64 -mno-red-zone -Os -ffreestanding \
	-fno-builtin -ffunction-sections -fdata-sections -fno-pic -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-isolate-erroneous-paths-dereference -fno-strict-aliasing \
	-Wall -Wextra -Werror
endif

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
