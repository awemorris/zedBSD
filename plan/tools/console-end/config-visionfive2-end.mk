# The VisionFive 2 kernel of the console's end over the loader's screen: the
# port's configuration with a kernel that ends the console over the screen's
# range 60 seconds after the screen is mapped, as a display driver's first flip
# does (KERN_TEST_TEXT_END_SECONDS in src/drivers/generic/boot-fb.c), and logs
# what the end returned and where the mapping it removed was.
#   make CONFIGMK=plan/tools/console-end/config-visionfive2-end.mk BUILD=build/<dir> build/<dir>/vmunix
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/config/config.vf2.mk
CONFIG_CPPFLAGS += -DKERN_TEST_TEXT_END_SECONDS=60
