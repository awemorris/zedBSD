/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Block device identity ABI.
 */
#ifndef KERN_UAPI_BLKID_H
#define KERN_UAPI_BLKID_H

#include <stdint.h>
#include <sys/ioctl.h>

#define KERN_BLKID_IOC_GROUP	'B'
#define KERN_BLKID_TEXT_MAX	64U

#define KERN_BLKID_TYPE	0x0001U
#define KERN_BLKID_UUID	0x0002U
#define KERN_BLKID_LABEL	0x0004U
#define KERN_BLKID_PARTUUID	0x0008U
#define KERN_BLKID_PARTLABEL	0x0010U

struct block_identity {
	uint32_t flags;
	uint32_t reserved;
	char type[16];
	char uuid[KERN_BLKID_TEXT_MAX];
	char label[KERN_BLKID_TEXT_MAX];
	char partuuid[KERN_BLKID_TEXT_MAX];
	char partlabel[KERN_BLKID_TEXT_MAX];
};

#define BLKGETIDENTITY	_IOR(KERN_BLKID_IOC_GROUP, 1, struct block_identity)

#endif
