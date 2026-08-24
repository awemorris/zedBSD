# Freestanding libc subset for zedBSD.
#
# Historically this code was built and tested independently (M3) before
# being linked into vmunix at a later integration milestone.

ZEDBSD_LIBC_CC ?= $(CC)
ZEDBSD_LIBC_NM ?= nm
ZEDBSD_LIBC_OBJDUMP ?= objdump

ZEDBSD_REGEX_SOURCES := libc/regex/regcomp.c libc/regex/regexec.c \
	libc/regex/regerror.c libc/regex/tre-mem.c

ZEDBSD_LIBC_USER_EXTRA_SOURCES := \
	userland/base/libc/atomic-runtime.c \
	libc/string-extra.c libc/fenv.c libc/wide-extra.c libc/inttypes.c \
	libc/stdlib-extra.c libc/time-extra.c libc/stdio-extra.c \
	libc/setjmp.c libc/err.c libc/libgen.c libc/search.c \
	libc/random48.c libc/random.c libc/xsi-crypto.c \
	libc/ftw.c libc/ndbm.c libc/realpath.c libc/tempnam.c \
	libc/xsi-process.c libc/fmtmsg.c libc/syslog.c libc/sysv-ipc.c \
	libc/catalog.c libc/locale-db.c libc/fnmatch.c \
	$(ZEDBSD_REGEX_SOURCES)

ZEDBSD_LIBC_SOURCES := \
	libc/heap.c \
	libc/string.c \
	libc/string-extra.c \
	libc/ctype.c \
	libc/fenv.c \
	libc/locale.c \
	libc/wide.c \
	libc/wide-extra.c \
	libc/int64.c \
	libc/inttypes.c \
	libc/strto.c \
	libc/stdlib-extra.c \
	libc/time-extra.c \
	libc/format.c \
	libc/stdio.c \
	libc/stdio-extra.c \
	libc/setjmp.c \
	libc/err.c \
	libc/libgen.c \
	libc/search.c \
	libc/random48.c \
	libc/random.c \
	libc/xsi-crypto.c \
	libc/ftw.c \
	libc/ndbm.c \
	libc/realpath.c \
	libc/tempnam.c \
	$(ZEDBSD_REGEX_SOURCES)

ZEDBSD_LIBC_OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(ZEDBSD_LIBC_SOURCES))

ZEDBSD_LIBC_CPPFLAGS := -nostdinc -Iinclude -Iinclude/uapi -Isrc -I. \
	-I$(BUILD) -Ilibc/include
ZEDBSD_LIBC_CFLAGS := \
	-m32 -march=i386 -Os -ffreestanding -fno-builtin \
	-fno-pic -fno-pie -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-isolate-erroneous-paths-dereference -fno-strict-aliasing \
	-msoft-float -mno-80387 -mno-fp-ret-in-387 \
	-mno-mmx -mno-sse -mno-sse2 \
	-ffunction-sections -fdata-sections -Wall -Wextra -Werror

ZEDBSD_HOST_TEST_CFLAGS := \
	-m32 -O2 -fno-builtin -fno-stack-protector \
	-Wall -Wextra -Werror \
	-I. -Iinclude -Iinclude/uapi -Isrc -Ilibc/include

$(BUILD)/tests/libc-host-test: tests/libc-host-test.c \
	$(ZEDBSD_LIBC_SOURCES) src/kern/fs.c src/kern/namespace.c src/kern/env.c \
	src/kern/disk.c src/kern/buf.c \
	tests/vfs-host-stubs.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(ZEDBSD_HOST_TEST_CFLAGS) \
		src/kern/fs.c src/kern/namespace.c src/kern/env.c \
		src/kern/disk.c src/kern/buf.c src/kern/inode.c src/kern/file.c \
		src/kern/namecache.c src/kern/namei.c src/kern/mount.c \
		src/kern/rootfs.c \
		tests/vfs-host-stubs.c \
		$(ZEDBSD_LIBC_SOURCES) $< -o $@

libc-objects: $(ZEDBSD_LIBC_OBJECTS)

libc-host-test:
	@if printf 'int main(void){return 0;}\n' | \
		$(HOSTCC) -m32 -x c - -o /tmp/zedbsd-libc-m32-probe >/dev/null 2>&1; then \
		rm -f /tmp/zedbsd-libc-m32-probe; \
		$(MAKE) --no-print-directory $(BUILD)/tests/libc-host-test; \
		$(BUILD)/tests/libc-host-test; \
		echo "zedBSD libc host tests: PASS"; \
	else \
		echo "zedBSD libc host tests: SKIP (32-bit host libc unavailable)"; \
	fi

libc-opcode-check: libc-objects
	@if $(ZEDBSD_LIBC_OBJDUMP) -d --no-show-raw-insn $(ZEDBSD_LIBC_OBJECTS) | \
		grep -E '(^[[:space:]]*[0-9a-f]+:[[:space:]]+f[a-z0-9]+[[:space:]])|\b(bswap|cmpxchg|xadd|cmov[a-z]*|rdtsc|ud2|cpuid|fx[a-z]+|movaps|movups|xmm[0-9]|ymm[0-9]|zmm[0-9])\b'; then \
		echo "ERROR: zedBSD libc objects contain a post-i386 opcode" >&2; \
		exit 1; \
	fi
	@echo "zedBSD libc i386 opcode check: PASS"

.PHONY: libc-objects libc-host-test libc-opcode-check
