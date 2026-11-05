include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws161-p003: tries a smart card slot, /dev/smartcardN (the test kernel's loopback card, or a reader's slot with
# -i: the information and the state only): the claim, the APDUs, GET RESPONSE, the card's events (test images only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,smartcard-probe,smart card slot probe,amd64,n,basic,userland/tests/smartcard-probe/main.c,y,tests,tests/smartcard-probe,,0755,program,,,bin))
