/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UAPI_QUOTA_H
#define ZEDBSD_UAPI_QUOTA_H

#include <stdint.h>

#define ZEDBSD_QUOTA_VERSION 1U

#define ZEDBSD_QUOTA_GET     1U
#define ZEDBSD_QUOTA_SET     2U
#define ZEDBSD_QUOTA_ENABLE  3U
#define ZEDBSD_QUOTA_DISABLE 4U
#define ZEDBSD_QUOTA_SYNC    5U

#define ZEDBSD_QUOTA_USER  0U
#define ZEDBSD_QUOTA_GROUP 1U

#define ZEDBSD_QUOTA_F_ENABLED 0x00000001U

/* All quantities are filesystem blocks, inodes, or absolute UTC seconds. */
struct zedbsd_quota_ctl {
	uint32_t size;
	uint32_t version;
	uint32_t command;
	uint32_t type;
	uint32_t id;
	uint32_t flags;
	uint64_t block_soft;
	uint64_t block_hard;
	uint64_t inode_soft;
	uint64_t inode_hard;
	uint64_t blocks;
	uint64_t inodes;
	uint64_t block_deadline;
	uint64_t inode_deadline;
	uint64_t grace_seconds;
};

#endif
