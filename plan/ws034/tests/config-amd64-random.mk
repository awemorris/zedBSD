# ws034-p055: the stream test image plus the random test
# (plan/ws034/tests/random-test.c, built to build/p054-tests/random-test).
include plan/ws034/tests/config-amd64-stream.mk
ZEDBSD_EXTRA_INPUTS += build/p054-tests/random-test
ZEDBSD_EXTRA_FILES += --file /usr/bin/random-test=build/p054-tests/random-test
