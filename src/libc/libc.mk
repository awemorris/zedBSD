# zedBSD libc
# Copyright (C) 2026 Awe Morris

ZEDBSD_LIBC_CC ?= $(CC)
ZEDBSD_LIBC_NM ?= $(if $(NM),$(NM),nm)
ZEDBSD_LIBC_OBJDUMP ?= $(if $(OBJDUMP),$(OBJDUMP),objdump)

ZEDBSD_REGEX_SOURCES := src/libc/regex/regcomp.c src/libc/regex/regexec.c \
	src/libc/regex/regerror.c src/libc/regex/tre-mem.c

ZEDBSD_LIBC_USER_EXTRA_SOURCES := \
	userland/base/libc/atomic-runtime.c \
	src/libc/string-extra.c src/libc/fenv.c src/libc/wide-extra.c src/libc/inttypes.c \
	src/libc/stdlib-extra.c src/libc/time-extra.c src/libc/stdio-extra.c \
	src/libc/setjmp.c src/libc/err.c src/libc/libgen.c src/libc/search.c src/libc/iconv.c \
	src/libc/random48.c src/libc/random.c src/libc/xsi-crypto.c \
	src/libc/ftw.c src/libc/ndbm.c src/libc/realpath.c src/libc/tempnam.c \
	src/libc/fallocate.c \
	src/libc/xsi-process.c src/libc/fmtmsg.c src/libc/syslog.c src/libc/sysv-ipc.c \
	src/libc/catalog.c src/libc/locale-db.c src/libc/fnmatch.c src/libc/openbsd.c src/libc/openbsd-vis.c \
	src/libc/openbsd-base64.c src/libc/readpassphrase.c \
	src/libc/openbsd-opts.c src/libc/openbsd-glob.c \
	src/libc/openbsd-digest.c src/libc/openbsd-sha2.c \
	userland/base/libc/resolv.c \
	$(ZEDBSD_REGEX_SOURCES)

ZEDBSD_LIBC_SOURCES := \
	src/libc/heap.c \
	src/libc/string.c \
	src/libc/string-extra.c \
	src/libc/ctype.c \
	src/libc/fenv.c \
	src/libc/locale.c \
	src/libc/wide.c \
	src/libc/wide-extra.c \
	src/libc/int64.c \
	src/libc/inttypes.c \
	src/libc/strto.c \
	src/libc/stdlib-extra.c \
	src/libc/time-extra.c \
	src/libc/format.c \
	src/libc/stdio.c \
	src/libc/stdio-extra.c \
	src/libc/setjmp.c \
	src/libc/err.c \
	src/libc/libgen.c \
	src/libc/search.c \
	src/libc/random48.c \
	src/libc/random.c \
	src/libc/xsi-crypto.c \
	src/libc/ftw.c \
	src/libc/ndbm.c \
	src/libc/realpath.c \
	src/libc/tempnam.c \
	src/libc/fallocate.c \
	$(ZEDBSD_REGEX_SOURCES)

ZEDBSD_LIBC_OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(ZEDBSD_LIBC_SOURCES))

ZEDBSD_LIBC_CPPFLAGS := -nostdinc -Iinclude -Isrc -I. \
	-I$(BUILD) -Iinclude/libc
ZEDBSD_LIBC_CFLAGS := \
	-m32 -march=i386 -Os -ffreestanding -fno-builtin \
	-fno-pic -fno-pie -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-strict-aliasing \
	-msoft-float -mno-80387 -mno-fp-ret-in-387 \
	-mno-mmx -mno-sse -mno-sse2 \
	-ffunction-sections -fdata-sections -Wall -Wextra -Werror

libc-objects: $(ZEDBSD_LIBC_OBJECTS)

libc-opcode-check: libc-objects
	@if $(ZEDBSD_LIBC_OBJDUMP) -d --no-show-raw-insn $(ZEDBSD_LIBC_OBJECTS) | \
		grep -E '(^[[:space:]]*[0-9a-f]+:[[:space:]]+f[a-z0-9]+[[:space:]])|\b(bswap|cmpxchg|xadd|cmov[a-z]*|rdtsc|ud2|cpuid|fx[a-z]+|movaps|movups|xmm[0-9]|ymm[0-9]|zmm[0-9])\b'; then \
		echo "ERROR: zedBSD libc objects contain a post-i386 opcode" >&2; \
		exit 1; \
	fi
	@echo "zedBSD libc i386 opcode check: PASS"

.PHONY: libc-objects libc-opcode-check
