/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Storage page's analysis of a folder's use (storage-scan.c,
 * ws089-p023): how much each folder in it takes, counted by worker
 * threads while the page goes on drawing, and stopped on request.
 *
 * It knows nothing of the page: it walks the files and keeps totals that
 * the page copies when it draws (se_scan_view).  So the host tests run it
 * alone.
 */

#ifndef SE_STORAGE_SCAN_H
#define SE_STORAGE_SCAN_H

#include <keiland/keiland_thread.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* The workers, the folders of the root shown apart, and the longest path walked. */
#define SE_SCAN_THREADS		4U
#define SE_SCAN_GROUPS		48U
#define SE_SCAN_PATH		1024U
#define SE_SCAN_NAME		256U

/* Where a scan has got to. */
#define SE_SCAN_IDLE		0U
#define SE_SCAN_RUNNING		1U
#define SE_SCAN_DONE		2U
#define SE_SCAN_STOPPED		3U

/*
 * One folder (or "the files here", or "the other folders") of the root:
 * its name, the bytes its files take on the disk, how many files, and
 * whether it is a folder one may go into.
 */
struct se_scan_group {
	char name[SE_SCAN_NAME];
	uint64_t bytes;
	uint64_t files;
	unsigned folder;
};

/* A folder waiting to be walked: its path, and the group its bytes count for. */
struct se_scan_job {
	char *path;
	unsigned group;
};

/*
 * One analysis: the root and its file system (no other one is walked), the
 * folders waiting (jobs) and how many workers walk one now (busy), the
 * groups with their totals, the files of a few links counted once
 * (linked, a set of device and inode), the folders that could not be read,
 * the state, the stop asked, and a generation that grows with every
 * total added (the page draws again when it moves).  lock guards it all;
 * wake tells the workers a job came or the scan ends.
 */
struct se_scan {
	struct kl_mutex lock;
	struct kl_cond wake;
	struct kl_thread *threads[SE_SCAN_THREADS];
	unsigned thread_count;
	int initialized;
	char root[SE_SCAN_PATH];
	dev_t device;
	struct se_scan_job *jobs;
	size_t job_count;
	size_t job_capacity;
	unsigned busy;
	struct se_scan_group groups[SE_SCAN_GROUPS];
	unsigned group_count;
	uint64_t *linked;
	size_t linked_count;
	size_t linked_capacity;
	uint64_t unreadable;
	unsigned state;
	int stop;
	uint64_t generation;
	uint64_t started_ms;
	uint64_t finished_ms;
};

/* What the page shows: the groups the largest first, the totals and the state. */
struct se_scan_view {
	char root[SE_SCAN_PATH];
	struct se_scan_group groups[SE_SCAN_GROUPS];
	unsigned group_count;
	uint64_t bytes;
	uint64_t files;
	uint64_t unreadable;
	unsigned state;
	uint64_t generation;
};

int se_scan_start(struct se_scan *scan, const char *root);
void se_scan_stop(struct se_scan *scan);
void se_scan_finish(struct se_scan *scan);
void se_scan_view(struct se_scan *scan, struct se_scan_view *view);

#endif
