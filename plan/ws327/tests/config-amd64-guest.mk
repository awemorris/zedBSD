# WS327's QEMU image: the menu image's programs (the compositor, Terminal and the probes) and the programs whose threads
# and sockets moved to libkeiland's layers (Mailer, Music, Video Player, Photos, Settings, the input method).
# Build:  plan/tools/guest/test-image.sh plan/ws327/tests/config-amd64-guest.mk build/ws327-img
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/tools/titlebar/config-amd64-menu.mk
ZEDBSD_USER_PROGRAMS += settings files textedit
ZEDBSD_USER_PROGRAMS += mailer music videoplayer photos keiland-ime
