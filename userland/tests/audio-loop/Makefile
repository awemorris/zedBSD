include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws204-p003: plays a tone through audiod and records it at once, to check recording (test images only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
AUDIO_LOOP_SOURCES := userland/tests/audio-loop/main.c userland/tests/audio-loop/measure.c
$(eval $(call USERLAND_PACKAGE,audio-loop,audiod record and play loop test,*,n,basic,$(AUDIO_LOOP_SOURCES),y,tests,tests/audio-loop,,0755,program,,,bin))
