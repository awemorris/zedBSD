/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The callers' side of keiland-preview (WS168 p004,
 * plan/ws168/preview-sandbox-design.md section 5): compiled into the programs that
 * show previews (Files, Settings).  A preview is asked of a child that
 * gets the input file as fd 0 and the output file as fd 1 and nothing else
 * (zedbsd/spawn.c: sandbox_spawn; linux/spawn.c: a new process that
 * confines itself with seccomp).  On FreeBSD, until the program confines
 * itself with Capsicum, the preview is made in the caller's own process
 * (freebsd/spawn.c), as it was before.
 *
 * preview_start starts one and preview_poll follows it without waiting
 * (one too slow is ended); preview_picture makes one, waits for it and
 * reads the picture back; preview_picture_begin and preview_picture_follow
 * do the same without waiting, for a window that must keep answering.
 * What a child writes is not trusted: the reader takes only a binary PPM of
 * a sane size.  The children are the system's to start, follow and end
 * (spawn.h); a caller sees no process of theirs and no descriptor.
 */

#ifndef KEILAND_PREVIEW_CLIENT_H
#define KEILAND_PREVIEW_CLIENT_H

#include "preview.h"

#include <stdint.h>

/* How long a child may take before it is ended (ms), a picture's and a PDF's; a killed child's status. */
#define PREVIEW_LIMIT_MS	5000U
#define PREVIEW_LIMIT_PDF_MS	10000U
#define PREVIEW_KILLED		128

/* The limits of a child: its memory, its processor time (s), the most it writes. */
#define PREVIEW_MEMORY_MAX	(1024UL * 1024UL * 1024UL)
#define PREVIEW_CPU_SECONDS	10U
#define PREVIEW_WRITE_MAX	(48UL * 1024UL * 1024UL)

/* The system's record of a child (spawn.h), and the output a picture is written to (client.c). */
struct preview_child;
struct preview_output;

/*
 * A preview being made: the child while it runs (NULL once it ended, or
 * when the preview was made in this process), whether it was made by a
 * child of its own, when it started and how long it may take (ms),
 * whether it has finished and its exit status (PREVIEW_*, or
 * PREVIEW_KILLED plus the signal).
 */
struct preview_job {
	struct preview_child *child;
	int apart;
	uint64_t started_ms;
	unsigned limit_ms;
	int finished;
	int status;
};

/*
 * A picture being made without waiting (preview_picture_begin): the job
 * and the output it writes, a temporary file already removed from its
 * folder and known to this process only (NULL when none is being made).
 */
struct preview_pending {
	struct preview_job job;
	struct preview_output *output;
};

/* A picture read back: opaque 0xffRRGGBB pixels, width by height (allocated). */
struct preview_picture {
	uint32_t *pixels;
	int width;
	int height;
};

/*
 * What a media file is (preview_info_read, ws157-p013): its kind
 * (KL_MEDIA_* of picture/media-kind.h), a picture's size (0 when it is
 * not known), and whether its date is known and the date (seconds of the
 * camera's calendar, as photos.h's ph_time has them).
 */
struct preview_info {
	int kind;
	long width;
	long height;
	int dated;
	int64_t taken;
	int utc;
};

/* The callers' side (client.c). */
int preview_start(const char *input, const char *output, const struct preview_request *request, struct preview_job *job);
int preview_poll(struct preview_job *job);
int preview_wait(struct preview_job *job);
int preview_picture(const char *input, const struct preview_request *request, struct preview_picture *picture);
int preview_picture_begin(const char *input, const struct preview_request *request, struct preview_pending *pending);
int preview_picture_follow(struct preview_pending *pending, struct preview_picture *picture, int *error);
void preview_picture_cancel(struct preview_pending *pending);
int preview_status_error(int status);
void preview_picture_release(struct preview_picture *picture);
int preview_info_read(int input, struct preview_info *info);

#endif
