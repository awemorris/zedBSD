include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# A test client of the text input protocol (ws095-p004): a window whose text input logs what the input method
# sends, for the guest tests of the input method.  Not in any image by default.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,ime-probe,Input method test client,x86_64,n,basic,userland/tests/ime-probe/main.c,y,tests,tests/ime-probe,desktop/libwayland,0755,program,,,bin))
