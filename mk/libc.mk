# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# libc Base
#

LIBC_SOURCES := \
	src/libc/heap.c \
	src/libc/string.c \
	src/libc/ctype.c \
	src/libc/locale.c \
	src/libc/wide.c \
	src/libc/int64.c \
	src/libc/strto.c \
	src/libc/softfloat.c \
	src/libc/float-parse.c \
	src/libc/format.c \
	src/libc/stdio.c \
	src/libc/string-extra.c \
	src/libc/fenv.c \
	src/libc/wide-extra.c \
	src/libc/inttypes.c \
	src/libc/stdlib-extra.c \
	src/libc/sort.c \
	src/libc/time-extra.c \
	src/libc/stdio-extra.c \
	src/libc/scan.c \
	src/libc/setjmp.c \
	src/libc/err.c \
	src/libc/libgen.c \
	src/libc/search.c \
	src/libc/iconv.c \
	src/libc/random48.c \
	src/libc/random.c \
	src/libc/xsi-crypto.c \
	src/libc/ftw.c \
	src/libc/ndbm.c \
	src/libc/realpath.c \
	src/libc/tempnam.c \
	src/libc/fallocate.c \
	src/libc/xsi-process.c \
	src/libc/fmtmsg.c \
	src/libc/syslog.c \
	src/libc/sysv-ipc.c \
	src/libc/catalog.c \
	src/libc/locale-db.c \
	src/libc/fnmatch.c \
	src/libc/openbsd.c \
	src/libc/openbsd-vis.c \
	src/libc/openbsd-base64.c \
	src/libc/readpassphrase.c \
	src/libc/openbsd-opts.c \
	src/libc/openbsd-glob.c \
	src/libc/openbsd-digest.c \
	src/libc/openbsd-sha2.c \
	src/libc/posix.c \
	src/libc/dlfcn.c \
	src/libc/static-tls.c \
	src/libc/poll.c \
	src/libc/termios.c \
	src/libc/pthread.c \
	src/libc/timer.c \
	src/libc/shm.c \
	src/libc/semaphore.c \
	src/libc/mqueue.c \
	src/libc/socket.c \
	src/libc/resolver.c \
	src/libc/resolver-dns.c \
	src/libc/signal.c \
	src/libc/account.c \
	src/libc/crypt.c \
	src/libc/utmpx.c \
	src/libc/atomic-runtime.c \
	src/libc/resolv.c \
	src/libc/regex/regcomp.c \
	src/libc/regex/regexec.c \
	src/libc/regex/regerror.c \
	src/libc/regex/tre-mem.c

CRT0_ASM := \
	src/libc/crt/crt0-$(ZEDBSD_ARCHITECTURE).S

CRT1_ASM := \
	src/libc/crt/crt1-$(ZEDBSD_ARCHITECTURE).S

LIBM_SOURCES := \
	src/libc/math/support.c \
	src/libc/math/classify.c \
	src/libc/math/rounding.c \
	src/libc/math/remainder.c \
	src/libc/math/fma.c \
	src/libc/math/sqrt.c \
	src/libc/math/float.c \
	src/libc/math/long-double.c \
	src/libc/math/tables.c \
	src/libc/math/exp.c \
	src/libc/math/log.c \
	src/libc/math/pow.c \
	src/libc/math/trig.c \
	src/libc/math/atrig.c \
	src/libc/math/hyperbolic.c \
	src/libc/math/cbrt-hypot.c \
	src/libc/math/erf.c \
	src/libc/math/gamma.c \
	src/libc/math/bessel.c

LIBM_HEADERS := \
	src/libc/math/math-internal.h \
	src/libc/math/math-constants.h

# The compiler runtime is compiled from the LLVM source tree, which a build
# that installed the cached toolchain has not extracted: a missing source
# waits for the extraction (an existing one is used as it is).
$(LLVM_SOURCE)/compiler-rt/lib/builtins/%: | $(LLVM_SOURCE)/.patched
	@:
