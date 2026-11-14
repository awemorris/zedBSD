# ws101-p015: the demonstration image (plan/ws075/demo/config-demo-hdmi.mk, with gpudemo and the accelerator-enabled
# noct) for scene S13 on the 5330's passthrough, built by plan/tools/gpu-compute/demo/build-s13-image.sh.  noct is left
# out of the build and its /bin/noct given as a file (build-s13-image.sh's NOCT, made with ZEDBSD_NOCT_ACCEL := y by
# the main checkout), so WS101 does not build Noct (the toolchain rule).
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../..)
include $(TEST_TREE)/plan/ws075/demo/config-demo-hdmi.mk
override CONFIG_USERLAND_PROGRAMS := $(filter-out noct,$(CONFIG_USERLAND_PROGRAMS))
