# ws191-p003: the AAT image (plan/tools/aat/config-amd64-aat.mk: the UAT image -- the desktop, audiod, the HD Audio
# driver, Music, Video Player and libavcodec -- with aat-input, the screen's capture and keiland-settings, so that the
# test can pause and seek) and a 20 s video of AAC sound (plan/tools/media/sample.mp4) for Music and Video Player to
# play.  The test copies it to ~/Music/sample.m4a and ~/Videos/sample.mp4 in the guest.
#   plan/tools/guest/test-image.sh plan/ws191/tests/config-amd64-sound.mk build/ws191-sound
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/tools/aat/config-amd64-aat.mk
ZEDBSD_EXTRA_INPUTS += plan/tools/media/sample.mp4
ZEDBSD_EXTRA_FILES += --file /usr/share/ws191/sample.mp4=plan/tools/media/sample.mp4
