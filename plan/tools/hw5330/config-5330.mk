# ws245: the Latitude 5330 regression image (plan/tools/hw5330/5330-run.sh): the CI amd64 configuration with the
# compositor's test-only capture and its client keiland-shot (the screen's PNG over SSH), and backlight-probe on the
# image (the survey runs /bin/backlight-probe).  A test image only.
#   make CONFIGMK=plan/tools/hw5330/config-5330.mk BUILD=<dir> disk
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/config/config.x86_64.mk
include $(TEST_TREE)/mk/platform-x86_64/defaults.mk
ZEDBSD_TEST_SCREEN_CAPTURE := y
override CONFIG_USERLAND_PROGRAMS += keiland-shot backlight-probe
