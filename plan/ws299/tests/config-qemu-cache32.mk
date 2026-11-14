# ws299: the QEMU serial test kernel with a 32 MiB buffer cache, the size at which the cache equals the old fixed
# dirty bound (qemu-cache32-write.sh).  A test kernel only; qemu-suite.sh puts it into a copy of the serial image
# (config-qemu-serial.mk).
#   make CONFIGMK=plan/ws299/tests/config-qemu-cache32.mk BUILD=<dir> <dir>/vmunix   (after llvm-toolchain-cached)
# The tree, found from this file: the nested makes read the configuration from their own directories.
WS299_TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(WS299_TEST_TREE)/plan/ws299/tests/config-qemu-serial.mk
override CONFIG_BUF_CACHE_KIB := 32768
