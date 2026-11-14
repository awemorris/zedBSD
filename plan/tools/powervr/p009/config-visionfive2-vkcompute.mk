# The VisionFive 2 test image of ws269-p009: the CI board configuration with
# libvulkan and the vkcompute test (one compute dispatch through the powervr
# executor, one line on the screen).  Not for the ordinary image.
#
# The PowerVR firmware is left out because its fetch asks for agreement to
# LICENSE.powervr.  To make a runnable image, read the license and build with
#   make CONFIGMK=plan/tools/powervr/p009/config-visionfive2-vkcompute.mk \
#        VKCOMPUTE_FIRMWARE=powervr-firmware BUILD=build/<dir> disk
# where license-agree.sh shows the license and asks yes/no on the first fetch
# (the answer is kept in the firmware cache, userland/firmware/zedbsd-firmware.mk).
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../..)
include $(TEST_TREE)/config/config.vf2.mk
include $(TEST_TREE)/mk/platform-vf2/defaults.mk

VKCOMPUTE_FIRMWARE ?=
override CONFIG_USERLAND_PROGRAMS := $(filter-out powervr-firmware,$(CONFIG_USERLAND_PROGRAMS)) \
	libwayland libvulkan vkcompute $(VKCOMPUTE_FIRMWARE)
