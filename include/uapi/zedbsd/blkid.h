/* Block device identity ABI.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UAPI_BLKID_H
#define ZEDBSD_UAPI_BLKID_H

#include <stdint.h>
#include <sys/ioctl.h>

#define ZEDBSD_BLKID_IOC_GROUP 'B'
#define ZEDBSD_BLKID_TEXT_MAX 64U

#define ZEDBSD_BLKID_TYPE      0x0001U
#define ZEDBSD_BLKID_UUID      0x0002U
#define ZEDBSD_BLKID_LABEL     0x0004U
#define ZEDBSD_BLKID_PARTUUID  0x0008U
#define ZEDBSD_BLKID_PARTLABEL 0x0010U

struct zedbsd_block_identity {
	uint32_t flags;
	uint32_t reserved;
	char type[16];
	char uuid[ZEDBSD_BLKID_TEXT_MAX];
	char label[ZEDBSD_BLKID_TEXT_MAX];
	char partuuid[ZEDBSD_BLKID_TEXT_MAX];
	char partlabel[ZEDBSD_BLKID_TEXT_MAX];
};

#define BLKGETIDENTITY _IOR(ZEDBSD_BLKID_IOC_GROUP, 1, \
	struct zedbsd_block_identity)

#endif
