# ws101-p009: the guest image of the OpenGL ES 3.1 compute test on Venus.  The lean GLSL image
# (plan/ws068/tests/config-amd64-glsl.mk: the zdesktop image without clang, libc++ and noct) with
# /bin/glescompute.  Build with
#   plan/tools/gpu-compute/gles/build-image.sh [BUILD]
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../..)
include $(TEST_TREE)/plan/ws068/tests/config-amd64-glsl.mk
override CONFIG_USERLAND_PROGRAMS += glescompute
