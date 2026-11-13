# How much of the build machine a nested build tool may use.
#
# make itself is told how many jobs to run by the caller's -j, but a build
# driven by cmake decides its own parallelism, so each of those is told this
# instead.  Keeping the answer in one place stops the number from being
# written differently in each of them.
#
# It is the number the caller gave make with -j, so that "make -j4" keeps
# every nested build tool to 4 jobs as well.  Without a number it is 16, or
# the machine's count when that is smaller (2026-10-03: several builds run
# at once on the 64-thread build machine, and 64 jobs each filled its disk's
# write-back).  The caller may also name it:
#
#   make BUILD_JOBS=8 toolchain
#
# Linking is separated because a link of a large C++ program needs far more
# memory than a compile, and a machine with many cores does not necessarily
# have memory for that many links at once.

ifndef BUILD_JOBS_MK
BUILD_JOBS_MK := 1

# The number given with -j, if any (make puts it in MAKEFLAGS before it
# reads the makefiles, and passes it on to the nested makes).
BUILD_JOBS_FROM_MAKE := $(lastword $(patsubst -j%,%,$(filter -j%,$(MAKEFLAGS))))

# The caller's -j decides the jobs of the nested build tools when it has a number.
ifneq ($(BUILD_JOBS_FROM_MAKE),)
BUILD_JOBS ?= $(BUILD_JOBS_FROM_MAKE)
endif

BUILD_JOBS ?= $(shell n=$$(nproc 2>/dev/null || echo 4); if test "$$n" -gt 16; then n=16; fi; echo "$$n")
BUILD_LINK_JOBS ?= $(shell \
	jobs=$(BUILD_JOBS); \
	links=$$((jobs / 8)); \
	if test "$$links" -lt 1; then links=1; fi; \
	echo "$$links")

endif
