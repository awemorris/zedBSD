# ws299: the QEMU serial test image of the buffer cache and NVMe tests (qemu-suite.sh): the serial-console image of
# plan/tools/posix/config-amd64-serial.mk without the ISA NE2000 driver, whose shared DP8390 core the x86_64 kernel's
# rules do not link (the tests use no ISA network card).  A test image only:
#   plan/tools/guest/test-image.sh --no-harness plan/ws299/tests/config-qemu-serial.mk <dir> --file /root/... disk
# The tree, found from this file: the nested makes read the configuration from their own directories.
WS299_TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(WS299_TEST_TREE)/plan/tools/posix/config-amd64-serial.mk
override CONFIG_DRIVER_NE2000 := n
