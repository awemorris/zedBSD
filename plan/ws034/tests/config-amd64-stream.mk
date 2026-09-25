# ws034-p054: the userland test image (serial console) for the pipe and
# device throughput measurements.
include plan/ws035/tests/config-amd64-userland.mk
CONFIG_PCAT_SERIAL_MIRROR := y
# The copy benchmark (plan/ws034/tests/copybench.c):
#   build/amd64/packages/toolchain/bin/zedbsd-clang -I . -O2 \
#       plan/ws034/tests/copybench.c -o build/p054-tests/copybench
ZEDBSD_EXTRA_INPUTS += build/p054-tests/copybench
ZEDBSD_EXTRA_FILES += --file /usr/bin/copybench=build/p054-tests/copybench
# The libc cancellation and stdio test (plan/ws034/tests/cancel-test.c).
ZEDBSD_EXTRA_INPUTS += build/p054-tests/cancel-test
ZEDBSD_EXTRA_FILES += --file /usr/bin/cancel-test=build/p054-tests/cancel-test
