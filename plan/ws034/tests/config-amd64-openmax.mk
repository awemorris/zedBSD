# ws034-p051: the CI amd64 image with the serial console and the descriptor
# limit test.  Build the test first:
#   build/amd64/packages/toolchain/bin/zedbsd-clang -I . -O2 \
#       plan/ws034/tests/openmax-test.c -o build/p051-tests/openmax-test
include config/ci/config-amd64.mk
CONFIG_PCAT_SERIAL_MIRROR := y
ZEDBSD_EXTRA_INPUTS += build/p051-tests/openmax-test
ZEDBSD_EXTRA_FILES += --file /usr/bin/openmax-test=build/p051-tests/openmax-test
