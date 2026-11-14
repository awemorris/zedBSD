# ws041: the userland image with the serial console and wakebench, for
# measuring wake-to-run latency on amd64.
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/ws035/tests/config-amd64-userland.mk
override CONFIG_PCAT_SERIAL_MIRROR := y
override CONFIG_USERLAND_PROGRAMS += wakebench
# rawecho: built first with
#   build/amd64/packages/toolchain/bin/zedbsd-clang -O2 plan/tools/latency/rawecho.c -o build/latency-tests/rawecho
ROOTFS_EXTRA_FILES += --file /usr/bin/rawecho=build/latency-tests/rawecho
