# ws041: the userland image with the serial console and wakebench, for
# measuring wake-to-run latency on amd64.
include plan/ws035/tests/config-amd64-userland.mk
CONFIG_PCAT_SERIAL_MIRROR := y
ZEDBSD_USER_PROGRAMS += wakebench
# rawecho: built first with
#   build/amd64/packages/toolchain/bin/zedbsd-clang -O2 plan/tools/latency/rawecho.c -o build/latency-tests/rawecho
ZEDBSD_EXTRA_INPUTS += build/latency-tests/rawecho
ZEDBSD_EXTRA_FILES += --file /usr/bin/rawecho=build/latency-tests/rawecho
