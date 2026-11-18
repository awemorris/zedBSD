/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Extracts plain text from MAP's MIME MMS body; attachments remain unexposed. */
#ifndef BLUETOOTHD_MMS_H
#define BLUETOOTHD_MMS_H

#include <stddef.h>
#include <stdint.h>

int btd_mms_text(const uint8_t *input, size_t length, char *output, size_t size, size_t *used, int *truncated);

#endif
