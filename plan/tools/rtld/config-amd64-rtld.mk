# ws140: the SSH guest image for the dynamic loader's tests (rtld-many.sh, and run-tls-check.sh of ws073-p038), the
# guest harness's image with nothing added: the tests copy their programs and libraries in over SSH.  Build (the
# result is BUILD/hdd-image.img):
#   plan/tools/guest/test-image.sh plan/tools/rtld/config-amd64-rtld.mk BUILD
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/guest/config-amd64-ssh.mk
