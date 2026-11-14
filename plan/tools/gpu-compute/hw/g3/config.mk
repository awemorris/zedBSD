# ws101-p011: the image of G3 (Noct's automatic parallelization) on the i915 passthrough of the 5330
# (plan/tools/gpu-compute/hw/g3-hw.sh): the image of plan/tools/gpu-compute/hw/gles/config.mk (the zdesktop image of the 5330
# with glescompute) with the demonstration's program and script (gpudemo).  /bin/noct comes from a build made with
# ZEDBSD_NOCT_ACCEL := y (g3-hw.sh's NOCT), not from this one, which keeps noct out.
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../../..)
include $(TEST_TREE)/plan/tools/gpu-compute/hw/gles/config.mk
override CONFIG_USERLAND_PROGRAMS += gpudemo
