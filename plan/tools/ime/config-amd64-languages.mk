# ws154 (Languages and SKK): the lean zdesktop guest image for the Venus tests (the Files image,
# plan/tools/files/config-amd64-files.mk) with the input method, both dictionaries, the test client, Settings and the
# settings tool.  Build (the result is BUILD/hdd-image.img):
#   plan/tools/guest/test-image.sh plan/tools/ime/config-amd64-languages.mk BUILD \
#     --file /usr/share/keiland/wallpaper.png=userland/desktop/wallpapers/Birch-Lake.png
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/files/config-amd64-files.mk
override CONFIG_USERLAND_PROGRAMS += ime dict skk-dict ime-probe settings keiland-settings
