# Freestanding libc subset for Boots.
#
# Historically this code was built and tested independently (M3) before
# being linked into BOOT.SYS at a later integration milestone.

BOOTS_LIBC_CC ?= $(CC)
BOOTS_LIBC_NM ?= nm
BOOTS_LIBC_OBJDUMP ?= objdump

BOOTS_LIBC_SOURCES := \
	libc/heap.c \
	libc/string.c \
	libc/ctype.c \
	libc/int64.c \
	libc/strto.c \
	libc/format.c \
	libc/stdio.c \
	libc/stdio-fs.c

BOOTS_LIBC_OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(BOOTS_LIBC_SOURCES))

BOOTS_LIBC_CPPFLAGS := -nostdinc -Iinclude -Isrc -I. -I$(BUILD) -Ilibc/include
BOOTS_LIBC_CFLAGS := \
	-m32 -march=i386 -Os -ffreestanding -fno-builtin \
	-fno-pic -fno-pie -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-isolate-erroneous-paths-dereference -fno-strict-aliasing \
	-msoft-float -mno-80387 -mno-fp-ret-in-387 \
	-mno-mmx -mno-sse -mno-sse2 \
	-Wall -Wextra -Werror

BOOTS_HOST_TEST_CFLAGS := \
	-m32 -O2 -fno-builtin -fno-stack-protector \
	-Wall -Wextra -Werror \
	-I. -Iinclude -Isrc -Ilibc/include

$(BUILD)/tests/libc-host-test: tests/libc-host-test.c \
	$(BOOTS_LIBC_SOURCES) src/kern/fs.c src/kern/namespace.c src/kern/env.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(BOOTS_HOST_TEST_CFLAGS) \
		src/kern/fs.c src/kern/namespace.c src/kern/env.c \
		src/kern/disk.c src/kern/inode.c src/kern/file.c \
		src/kern/namecache.c src/kern/namei.c src/kern/mount.c \
		src/kern/rootfs.c \
		$(BOOTS_LIBC_SOURCES) $< -o $@

libc-objects: $(BOOTS_LIBC_OBJECTS)

libc-host-test: $(BUILD)/tests/libc-host-test
	$(BUILD)/tests/libc-host-test
	@echo "Boots libc host tests: PASS"

libc-opcode-check: libc-objects
	@if $(BOOTS_LIBC_OBJDUMP) -d --no-show-raw-insn $(BOOTS_LIBC_OBJECTS) | \
		grep -E '(^[[:space:]]*[0-9a-f]+:[[:space:]]+f[a-z0-9]+[[:space:]])|\b(bswap|cmpxchg|xadd|cmov[a-z]*|rdtsc|ud2|cpuid|fx[a-z]+|movaps|movups|xmm[0-9]|ymm[0-9]|zmm[0-9])\b'; then \
		echo "ERROR: Boots libc objects contain a post-i386 opcode" >&2; \
		exit 1; \
	fi
	@echo "Boots libc i386 opcode check: PASS"

.PHONY: libc-objects libc-host-test libc-opcode-check
