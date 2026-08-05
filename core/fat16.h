/*
 * Boots FAT12/FAT16 driver
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_FAT16_H
#define BOOTS_FAT16_H

#include "core/fs.h"

extern const struct boots_filesystem_driver boots_fat12_driver;
extern const struct boots_filesystem_driver boots_fat16_driver;

#endif
