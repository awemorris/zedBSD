/*
 * Image-loader interface
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_IMAGE_H
#define ZEDBSD_IMAGE_H

#include "kern/fs.h"

struct zedbsd_image_loader {
	const char *name;
	int (*probe)(struct zedbsd_file *file);
	int (*load)(struct zedbsd_file *file, const char *arguments);
};

int zedbsd_image_boot(const struct zedbsd_image_loader *loader,
		      struct zedbsd_filesystem *filesystem, const char *path,
		      const char *arguments);

#endif
