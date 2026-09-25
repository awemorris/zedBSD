# ws035-p007: the userland test image with the HD Audio driver and audiotest.
# Build audiotest first:
#   build/amd64/packages/toolchain/bin/zedbsd-clang -I include -O2 \
#       plan/ws035/tests/audiotest.c -o build/p007-tests/audiotest
include plan/ws035/tests/config-amd64-userland.mk
CONFIG_DRIVER_PCI_HDA := y
CONFIG_PCAT_SERIAL_MIRROR := y
ZEDBSD_EXTRA_INPUTS += build/p007-tests/audiotest
ZEDBSD_EXTRA_FILES += --file /usr/bin/audiotest=build/p007-tests/audiotest
