/*
 * zedBSD image-loader dispatch
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/image.h"

int zedbsd_image_boot(const struct zedbsd_image_loader *loader,
		      struct zedbsd_filesystem *filesystem, const char *path,
		      const char *arguments)
{
	struct zedbsd_file file;

	if (!loader || !loader->probe || !loader->load ||
	    !zedbsd_fs_open(filesystem, path, &file) || !loader->probe(&file))
		return 0;
	return loader->load(&file, arguments ? arguments : "");
}
