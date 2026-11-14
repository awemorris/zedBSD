# ws299-p004: the QEMU serial test kernel with a 128 MiB buffer cache, so that reads keep the cache at its cap and
# full-cache misses take lines over (qemu-cache-stress.sh).  Not smaller: at 32 MiB the cache equals the dirty limit
# (BUF_DIRTY_LIMIT) and a large file write fills it with dirty lines until new lines cannot be had.  A test kernel
# only; qemu-suite.sh puts it into a copy of the serial image (config-qemu-serial.mk).
#   make CONFIGMK=plan/ws299/tests/config-qemu-cache128.mk BUILD=<dir> <dir>/vmunix   (after llvm-toolchain-cached)
# The tree, found from this file: the nested makes read the configuration from their own directories.
WS299_TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(WS299_TEST_TREE)/plan/ws299/tests/config-qemu-serial.mk
override CONFIG_BUF_CACHE_KIB := 131072
