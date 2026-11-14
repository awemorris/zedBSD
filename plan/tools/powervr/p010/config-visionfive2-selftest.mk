# The VisionFive 2 test image of ws269-p010a and p010c (design-p010.md §8.2, D-P2): the
# CI board configuration with libvulkan, vkcompute and gpu-selftest.  Not for
# the ordinary image.
#
# GPU_SELFTEST_BOOT=y (the default) also selects gpu-selftest-boot, the
# oneshot service that runs gpu-selftest at boot with its lines on the
# console; GPU_SELFTEST_BOOT=n leaves it to be run by hand (/bin/gpu-selftest).
#
# The PowerVR firmware is left out because its fetch asks for agreement to
# LICENSE.powervr.  To make a runnable image, read the license and build with
#   make CONFIGMK=plan/tools/powervr/p010/config-visionfive2-selftest.mk \
#        SELFTEST_FIRMWARE=powervr-firmware BUILD=build/<dir> disk
# where license-agree.sh shows the license and asks yes/no on the first fetch.
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../..)
include $(TEST_TREE)/config/config.vf2.mk
include $(TEST_TREE)/mk/platform-vf2/defaults.mk

SELFTEST_FIRMWARE ?=
GPU_SELFTEST_BOOT ?= y
override CONFIG_USERLAND_PROGRAMS := $(filter-out powervr-firmware,$(CONFIG_USERLAND_PROGRAMS)) \
	libwayland libvulkan vkcompute vkimage vkstage vkdemo vkmatrix gpu-selftest \
	$(if $(filter y,$(GPU_SELFTEST_BOOT)),gpu-selftest-boot) $(SELFTEST_FIRMWARE)
# The kernel renders a clear and a triangle once at start and says each step (render check).
override CONFIG_POWERVR_RENDER_CHECK := y
# The GPU reset recovery test (ws269-p015, destructive; run by hand as vkreset --isolated).
override CONFIG_USERLAND_PROGRAMS += vkreset
