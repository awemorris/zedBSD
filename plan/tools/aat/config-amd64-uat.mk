# ws159-p005: the image of the Latitude 5330 UAT.  The UAT's own configuration (config/current-uat.mk) is gone since the
# build system was made again; this file now stands for it with the x86_64 platform's configuration and defaults (the
# full userland of a product image).  It stays for the AAT images and the older notes that name it.
#   plan/tools/guest/test-image.sh --no-harness plan/tools/aat/config-amd64-uat.mk build/uat-0505
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/config/config.x86_64.mk
include $(TEST_TREE)/mk/platform-x86_64/defaults.mk
