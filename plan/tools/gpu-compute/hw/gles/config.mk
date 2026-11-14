# ws101-p010: the image of OpenGL ES 3.1's compute on the i915 passthrough of the 5330 (plan/tools/gpu-compute/hw/gles-hw.sh).
# The zdesktop image of the 5330 (plan/ws031/tests/config-zdesktop-hw.mk: the i915 driver and its firmware, the Vulkan,
# Wayland, EGL and GLES libraries, the compositor, egltest) with /bin/glescompute, without the packages that take
# long to build and that the run does not use (App Home's Files and Browser, clang, libc++, noct).
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../../..)
include $(TEST_TREE)/plan/ws031/tests/config-zdesktop-hw.mk
override CONFIG_USERLAND_PROGRAMS := $(filter-out browser files libz-compat libpng-compat noct libcxx clang,$(CONFIG_USERLAND_PROGRAMS))
override CONFIG_USERLAND_PROGRAMS += glescompute
