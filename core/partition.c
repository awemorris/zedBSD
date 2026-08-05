/*
 * Boots partition-table dispatch
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "core/partition.h"

static const struct boots_partition_scheme *active_scheme;

void
boots_partition_set_scheme(const struct boots_partition_scheme *scheme)
{
	active_scheme = scheme;
}

const struct boots_partition_scheme *
boots_partition_get_scheme(void)
{
	return active_scheme;
}

int
boots_partition_scan(struct boots_blkdev *dev,
		      struct boots_partition *entries, unsigned max_entries)
{
	if (active_scheme == NULL || active_scheme->scan == NULL ||
	    dev == NULL || entries == NULL || max_entries == 0)
		return -1;
	return active_scheme->scan(active_scheme, dev, entries, max_entries);
}
