# ws035-p009: the HDA test image with audiod and its test client.  Build the
# client first:
#   build/amd64/packages/toolchain/bin/zedbsd-clang -I . -O2 \
#       plan/ws035/tests/audiod-client.c -o build/p007-tests/audiod-client
include plan/ws035/tests/config-amd64-hda-image.mk
ZEDBSD_USER_PROGRAMS += audiod
ZEDBSD_EXTRA_INPUTS += build/p007-tests/audiod-client
ZEDBSD_EXTRA_FILES += --file /usr/bin/audiod-client=build/p007-tests/audiod-client
