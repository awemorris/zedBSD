/*
 * Boots block device registry
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/block.h"

static struct boots_blkdev *registry[BOOTS_BLKDEV_MAX];
static unsigned registry_count;

static int
name_equal(const char *a, const char *b)
{
	unsigned i;

	for (i = 0; i < BOOTS_BLKDEV_NAME_MAX; i++) {
		if (a[i] != b[i])
			return 0;
		if (a[i] == '\0')
			return 1;
	}
	return 1;
}

int
boots_blkdev_register(struct boots_blkdev *dev)
{
	if (dev == NULL || dev->read == NULL || dev->sector_size == 0 ||
	    dev->sector_count == 0 || registry_count >= BOOTS_BLKDEV_MAX)
		return 0;
	/* The name must be NUL-terminated within the field. */
	if (dev->name[BOOTS_BLKDEV_NAME_MAX - 1U] != '\0')
		return 0;
	registry[registry_count++] = dev;
	return 1;
}

void
boots_blkdev_reset(void)
{
	unsigned i;

	for (i = 0; i < BOOTS_BLKDEV_MAX; i++)
		registry[i] = NULL;
	registry_count = 0;
}

unsigned
boots_blkdev_count(void)
{
	return registry_count;
}

struct boots_blkdev *
boots_blkdev_get(unsigned index)
{
	return index < registry_count ? registry[index] : NULL;
}

struct boots_blkdev *
boots_blkdev_find(const char *name)
{
	unsigned i;

	if (name == NULL)
		return NULL;
	for (i = 0; i < registry_count; i++)
		if (name_equal(registry[i]->name, name))
			return registry[i];
	return NULL;
}

static enum boots_blkdev_result
range_check(struct boots_blkdev *dev, uint64_t lba, uint32_t count)
{
	if (dev == NULL || count == 0)
		return BOOTS_BLKDEV_INVALID;
	if (lba >= dev->sector_count || count > dev->sector_count - lba)
		return BOOTS_BLKDEV_OUT_OF_RANGE;
	return BOOTS_BLKDEV_OK;
}

enum boots_blkdev_result
boots_blkdev_read(struct boots_blkdev *dev, uint64_t lba, uint32_t count,
		   void *buffer)
{
	enum boots_blkdev_result result = range_check(dev, lba, count);

	if (result != BOOTS_BLKDEV_OK)
		return result;
	if (buffer == NULL)
		return BOOTS_BLKDEV_INVALID;
	return dev->read(dev, lba, count, buffer);
}

enum boots_blkdev_result
boots_blkdev_write(struct boots_blkdev *dev, uint64_t lba, uint32_t count,
		    const void *buffer)
{
	enum boots_blkdev_result result = range_check(dev, lba, count);

	if (result != BOOTS_BLKDEV_OK)
		return result;
	if (buffer == NULL)
		return BOOTS_BLKDEV_INVALID;
	if (dev->write == NULL)
		return BOOTS_BLKDEV_READ_ONLY;
	return dev->write(dev, lba, count, buffer);
}
