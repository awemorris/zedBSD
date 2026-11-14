# ws299-p001: the 5330 kernel with a 128 MiB buffer cache instead of an eighth of memory, to see whether the read
# slowdown moves with the cache's cap (plan/ws299/phase001/phase.md).  A measurement kernel only.
#   make CONFIGMK=plan/ws299/tests/config-5330-cache128.mk BUILD=<dir> <dir>/vmunix   (after llvm-toolchain-cached)
# The tree, found from this file: the nested makes read the configuration from their own directories.
WS299_TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(WS299_TEST_TREE)/plan/tools/hw5330/config-5330.mk
override CONFIG_BUF_CACHE_KIB := 131072
