# zedBSD userland package: hand-hershey, the handwriting templates made from the Hershey fonts (ws165-p002).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The handwriting face's recognizer (userland/desktop/wayland/hand-cloud.c) compares what was written with a
# template of each character: the simplex Roman letters, digits and signs, and the hiragana and katakana of
# Dr. A. V. Hershey's fonts (plan/ws165/phase001/phase.md section 4, the user's decision H3).  The data is the
# Usenet distribution (comp.sources.unix volume 4, "hershey", parts 2 to 5: hersh.oc1-4 and hersh.or1-4), fetched
# and checked here; convert.py takes the font files out of the shell archives as text (it never runs them) and
# writes the glyphs hershey-unicode.txt names as /usr/share/keiland/hand/hershey.txt.  Its terms ask that the
# acknowledgements go with the data: NOTICE, installed as /usr/share/licenses/hand-hershey/NOTICE.
#
# To update: there is no newer data; a change of the glyphs taken is hershey-unicode.txt's.
HAND_HERSHEY_MAKEFILE := $(lastword $(MAKEFILE_LIST))
HAND_HERSHEY_DIR := $(patsubst %/,%,$(dir $(HAND_HERSHEY_MAKEFILE)))
include $(HAND_HERSHEY_DIR)/../../../base/package.mk
include $(HAND_HERSHEY_DIR)/../../external.mk

# The four parts of the Usenet distribution, as compress(1) files.
HAND_HERSHEY_URL := https://ftp.isc.org/pub/usenet/comp.sources.unix/volume4/hershey
EXT_hand-hershey-part2_VERSION := 1986
EXT_hand-hershey-part2_ARCHIVE := hershey-1986-part2.Z
EXT_hand-hershey-part2_URL := $(HAND_HERSHEY_URL)/part2.Z
EXT_hand-hershey-part2_SIZE := 32469
EXT_hand-hershey-part2_SHA256 := 0e227df0675d577f865ff383752c69c4484a80223d3bf98b0c8bb9947761e723
EXT_hand-hershey-part3_VERSION := 1986
EXT_hand-hershey-part3_ARCHIVE := hershey-1986-part3.Z
EXT_hand-hershey-part3_URL := $(HAND_HERSHEY_URL)/part3.Z
EXT_hand-hershey-part3_SIZE := 33478
EXT_hand-hershey-part3_SHA256 := feb8c2fb565e94dfe615ae0c83c87004b569200490a8b20a1c92ec6cf5029a0e
EXT_hand-hershey-part4_VERSION := 1986
EXT_hand-hershey-part4_ARCHIVE := hershey-1986-part4.Z
EXT_hand-hershey-part4_URL := $(HAND_HERSHEY_URL)/part4.Z
EXT_hand-hershey-part4_SIZE := 31423
EXT_hand-hershey-part4_SHA256 := 8e58672b6e3c81378b8742e28ca6c0f269163c0fc79ad65f5a85eb75798a426a
EXT_hand-hershey-part5_VERSION := 1986
EXT_hand-hershey-part5_ARCHIVE := hershey-1986-part5.Z
EXT_hand-hershey-part5_URL := $(HAND_HERSHEY_URL)/part5.Z
EXT_hand-hershey-part5_SIZE := 31260
EXT_hand-hershey-part5_SHA256 := ecfe957bf8969f33d0b8e883cd0845e10615853d68c9cf382df9847901f6d8ce

$(eval $(call EXTERNAL_FILE,hand-hershey-part2))
$(eval $(call EXTERNAL_FILE,hand-hershey-part3))
$(eval $(call EXTERNAL_FILE,hand-hershey-part4))
$(eval $(call EXTERNAL_FILE,hand-hershey-part5))

HAND_HERSHEY_PARTS := part2 part3 part4 part5
HAND_HERSHEY_WORK := $(EXTERNAL_WORKROOT)/hand-hershey
HAND_HERSHEY_TEMPLATES := $(HAND_HERSHEY_WORK)/hershey.txt
HAND_HERSHEY_NOTICE := $(HAND_HERSHEY_DIR)/NOTICE

# The parts uncompressed (as text, never run), then the templates.
$(HAND_HERSHEY_TEMPLATES): $(foreach part,$(HAND_HERSHEY_PARTS),$(EXT_hand-hershey-$(part)_ARCHIVE_VERIFIED)) \
	$(HAND_HERSHEY_DIR)/convert.py $(HAND_HERSHEY_DIR)/hershey-unicode.txt
	@mkdir -p '$(HAND_HERSHEY_WORK)'
	$(foreach part,$(HAND_HERSHEY_PARTS),gzip -dc < '$(EXT_hand-hershey-$(part)_DISTFILE)' > '$(HAND_HERSHEY_WORK)/$(part).txt' &&) true
	$(PYTHON) -I '$(HAND_HERSHEY_DIR)/convert.py' '$(HAND_HERSHEY_DIR)/hershey-unicode.txt' '$@.tmp' \
		$(foreach part,$(HAND_HERSHEY_PARTS),'$(HAND_HERSHEY_WORK)/$(part).txt')
	mv -f '$@.tmp' '$@'

.PHONY: hand-hershey
hand-hershey: $(HAND_HERSHEY_TEMPLATES)
	@:

# ---------------------------------------------------------------- packaging

$(eval $(call USERLAND_PACKAGE,hand-hershey,Handwriting templates,*,n,package,,y,\
	packages/fonts,fonts/hand-hershey,))

PACKAGE_INPUTS += $(if $(filter hand-hershey,$(USER_PROGRAMS)),\
	$(HAND_HERSHEY_TEMPLATES) $(HAND_HERSHEY_NOTICE))
PACKAGE_FILES += $(if $(filter hand-hershey,$(USER_PROGRAMS)),\
	--file /usr/share/keiland/hand/hershey.txt=$(HAND_HERSHEY_TEMPLATES) \
	--file /usr/share/licenses/hand-hershey/NOTICE=$(HAND_HERSHEY_NOTICE) \
	--mode /usr/share/keiland/hand/hershey.txt=0644 \
	--mode /usr/share/licenses/hand-hershey/NOTICE=0644)
