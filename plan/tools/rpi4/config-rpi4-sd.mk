# The Pi 4 image for the SD regression of the generic SDHCI core (WS289 p002,
# QEMU raspi4b boot-test).  It is the platform's configuration without the
# vendor firmware packages, whose licenses are accepted per cache by the
# user; the SD path needs none of them.
#   make CONFIGMK=plan/tools/rpi4/config-rpi4-sd.mk BUILD=build/<dir> disk
# The platform's defaults are read here first, so that their program list can
# be filtered (the top Makefile reads them again after this file; the override
# keeps the filtered list).
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/config/config.rpi4.mk
include $(TEST_TREE)/mk/platform-rpi4/defaults.mk
override CONFIG_DRIVER_RPI4_WIFI := n
override CONFIG_USERLAND_PROGRAMS := $(filter-out amd-navi-firmware bcm43455-firmware \
	brcmbt-firmware i915-firmware intelax211-firmware intelbt-firmware \
	nvidia-gsp-firmware rtl8822b-firmware,$(CONFIG_USERLAND_PROGRAMS))
