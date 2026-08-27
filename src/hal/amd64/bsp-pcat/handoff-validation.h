/* Pure ZBL6 version/size/flag classification shared with host fixtures. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_HAL_AMD64_PCAT_HANDOFF_VALIDATION_H
#define ZEDBSD_HAL_AMD64_PCAT_HANDOFF_VALIDATION_H

#include <stdint.h>

enum zbl6_handoff_form {
	ZBL6_HANDOFF_FORM_INVALID = 0,
	ZBL6_HANDOFF_FORM_LEGACY_BIOS,
	ZBL6_HANDOFF_FORM_LEGACY_UEFI,
	ZBL6_HANDOFF_FORM_V5_BIOS,
	ZBL6_HANDOFF_FORM_V5_UEFI
};

enum zbl6_handoff_form zbl6_handoff_classify(uint16_t version,
	uint16_t size, uint32_t flags);
enum zbl6_handoff_form zbl6_handoff_classify_raw(const void *raw_handoff);

#endif
