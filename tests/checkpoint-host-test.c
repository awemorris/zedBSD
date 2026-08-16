/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/buf.h>
#include <kern/disk.h>
#include <kern/test-checkpoint.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>

static mtx_t lock;
static cnd_t condition;
static int reached, release_checkpoint, gone_started;
static struct disk *target, *lookup_result;

static int submit(struct disk *disk, struct bio *bio)
{
	(void)disk;
	bio_complete(bio, 0, (size_t)bio->b_block_count * 512U);
	return 0;
}
static const struct disk_ops ops = { .submit = submit };

static void checkpoint(enum kern_test_checkpoint_id id, void *object,
	void *argument)
{
	(void)argument;
	if (id != KERN_TEST_DISK_LOOKUP_BEFORE_REF || object != target)
		return;
	assert(mtx_lock(&lock) == thrd_success);
	reached = 1;
	assert(cnd_broadcast(&condition) == thrd_success);
	while (!release_checkpoint)
		assert(cnd_wait(&condition, &lock) == thrd_success);
	assert(mtx_unlock(&lock) == thrd_success);
}

static int lookup_thread(void *argument)
{
	(void)argument;
	lookup_result = disk_find("race0");
	return 0;
}

static int gone_thread(void *argument)
{
	(void)argument;
	assert(mtx_lock(&lock) == thrd_success);
	gone_started = 1;
	assert(cnd_broadcast(&condition) == thrd_success);
	assert(mtx_unlock(&lock) == thrd_success);
	disk_gone(target);
	return 0;
}

int main(void)
{
	thrd_t lookup, gone;
	assert(mtx_init(&lock, mtx_plain) == thrd_success);
	assert(cnd_init(&condition) == thrd_success);
	assert(buf_init() == 0);
	disk_registry_reset();
	target = disk_alloc();
	assert(target != NULL);
	strcpy(target->d_name, "race0");
	target->d_block_size = 512;
	target->d_block_count = 32;
	target->d_max_transfer_blocks = 8;
	target->d_ops = &ops;
	assert(disk_create(target) == 0);
	kern_test_checkpoint_set(checkpoint, NULL);
	assert(thrd_create(&lookup, lookup_thread, NULL) == thrd_success);
	assert(mtx_lock(&lock) == thrd_success);
	while (!reached)
		assert(cnd_wait(&condition, &lock) == thrd_success);
	assert(mtx_unlock(&lock) == thrd_success);
	assert(thrd_create(&gone, gone_thread, NULL) == thrd_success);
	assert(mtx_lock(&lock) == thrd_success);
	while (!gone_started)
		assert(cnd_wait(&condition, &lock) == thrd_success);
	release_checkpoint = 1;
	assert(cnd_broadcast(&condition) == thrd_success);
	assert(mtx_unlock(&lock) == thrd_success);
	assert(thrd_join(lookup, NULL) == thrd_success);
	assert(thrd_join(gone, NULL) == thrd_success);
	kern_test_checkpoint_set(NULL, NULL);
	assert(lookup_result == target);
	assert(disk_count() == 0);
	disk_release(lookup_result);
	assert(disk_destroy(target) == 0);
	puts("zedBSD deterministic registry checkpoint test: PASS");
	return 0;
}
