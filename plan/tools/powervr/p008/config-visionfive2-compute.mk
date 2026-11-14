# The VisionFive 2 test kernel of ws269-p008: the CI board configuration
# plus the PowerVR driver's check of the core's compute path (a compute
# kernel's writes, the data master coherency rounds, the temporaries
# field).  Not for the ordinary image.
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../..)
include $(TEST_TREE)/config/config.vf2.mk
include $(TEST_TREE)/mk/platform-vf2/defaults.mk

override CONFIG_POWERVR_COMPUTE_CHECK := y
