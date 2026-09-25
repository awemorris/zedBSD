# ws034-p046: the HDA/serial test image plus tcp-bulk, for TCP checks over
# USB CDC-ECM and loopback.  Build tcp-bulk first:
#   build/amd64/packages/toolchain/bin/zedbsd-clang -O2 \
#       plan/ws034/tests/tcp-bulk.c -o build/p007-tests/tcp-bulk
include plan/ws035/tests/config-amd64-hda-image.mk
ZEDBSD_EXTRA_INPUTS += build/p007-tests/tcp-bulk
ZEDBSD_EXTRA_FILES += --file /usr/bin/tcp-bulk=build/p007-tests/tcp-bulk
