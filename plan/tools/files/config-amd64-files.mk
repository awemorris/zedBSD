# ws071 (File Manager): a lean zdesktop guest image for the Venus tests, the ws070
# System Menu image (plan/tools/titlebar/config-amd64-menu.mk) with files.
# Build:
#   plan/tools/files/build-files-image.sh [BUILD]
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/titlebar/config-amd64-menu.mk
override CONFIG_USERLAND_PROGRAMS += libz-compat libpng-compat files
# ws079-p006: PDF Viewer (Files opens PDFs with it) and libpdf (with libjpeg-compat).
override CONFIG_USERLAND_PROGRAMS += libjpeg-compat libpdf pdfviewer
# ws127-p002: without the input method.  zdesktop starts keiland-ime itself (WS095) when it is installed, and
# it connects first, so the window under test would be client 2 where every guest test of files looks for
# client 1 (all of files-regress.sh failed so on 2026-10-03).  The tests do not type through an input method.
override CONFIG_USERLAND_PROGRAMS := $(filter-out ime dict,$(CONFIG_USERLAND_PROGRAMS))
