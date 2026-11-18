/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The removable media daemon (ws132-p004, plan/ws132/phase004/phase.md).
 *
 * volumed finds the removable disks whose filesystem zedBSD mounts (FAT,
 * UFS), lists them on /run/volumed.sock, and mounts one under /media when
 * the seat's user asks (never by itself), with nosuid and noexec and that
 * user as the owner a FAT volume shows; it unmounts one on eject, and
 * tidies a mount whose disk was pulled out.  The parts without system calls
 * (names, lines, the permission rule) are in names.c, so the host tests can
 * build them.
 */

#ifndef VOLUMED_VOLUMED_H
#define VOLUMED_VOLUMED_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* The socket, the folder volumes are mounted under, and the longest line either side sends. */
#define VOLUMED_SOCKET		"/run/volumed.sock"
#define VOLUMED_MEDIA		"/media"
#define VOLUMED_LINE_MAX	256U

/* The most volumes and clients kept, and the lengths of a volume's texts. */
#define VOLUMED_VOLUMES_MAX	16U
#define VOLUMED_CLIENTS_MAX	8U
#define VOLUMED_NAME_MAX	32U
#define VOLUMED_LABEL_MAX	64U
#define VOLUMED_PATH_MAX	128U

/* What a client asks. */
#define VOLUMED_ASK_HELLO	1
#define VOLUMED_ASK_MOUNT	2
#define VOLUMED_ASK_EJECT	3

/*
 * One volume: the disk's name (its ID), the filesystem (the mount type),
 * the label, the size, where it is mounted ("" when it is not), and whether
 * it was never mounted since it was inserted (the notification blinks for
 * it).  seen marks the volumes a scan found again.
 */
struct volumed_volume {
	char id[VOLUMED_NAME_MAX];
	char fs[8];
	char label[VOLUMED_LABEL_MAX];
	uint64_t bytes;
	char path[VOLUMED_PATH_MAX];
	unsigned fresh;
	unsigned seen;
};

int volumed_escape(const char *text, char *output, size_t size);
int volumed_mount_name(const char *label, const char *id, char *output, size_t size);
int volumed_parse(const char *line, int *ask, unsigned *request, char *id, size_t id_size);
int volumed_permitted(uid_t peer, uid_t seat, uid_t greeter);
int volumed_format_volume(const struct volumed_volume *volume, char *output, size_t size);

#endif
