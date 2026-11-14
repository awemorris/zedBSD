# ws345: the guest image of shell-guest.sh and guest-latency.sh, the ws071 File Manager image (zdesktop, System
# Menu, fonts) with the browser, its engine, its second program and the roots for https.
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/files/config-amd64-files.mk
override CONFIG_USERLAND_PROGRAMS += browser libbrowser browser-probe ca-certificates libjpeg-compat libgif-compat
