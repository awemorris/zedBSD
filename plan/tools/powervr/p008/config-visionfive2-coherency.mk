# ws269-p008 q1508: the VisionFive 2 test image of the gpu-selftest
# configuration (plan/tools/powervr/p010/config-visionfive2-selftest.mk) with
# the CPU and GPU coherency stress, vkcoherency (/bin/vkcoherency, run by
# hand).  Built as that configuration is, with SELFTEST_FIRMWARE for a
# runnable image.
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../..)
include $(TEST_TREE)/plan/tools/powervr/p010/config-visionfive2-selftest.mk

override CONFIG_USERLAND_PROGRAMS += vkcoherency
