/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Runs metadata requests on a worker-owned compositor connection. */
#ifndef PH_MEDIA_WORKER_H
#define PH_MEDIA_WORKER_H

/* A completed request owns its snapshot until the UI takes or the worker releases it. */
struct ph_media_result {
	int descriptor;
	int error;
	int imported;
	int notice;
};

int ph_media_worker_start(void);
void ph_media_worker_stop(void);
int ph_media_worker_queue(const char *path, int notice);
int ph_media_worker_take(struct ph_media_result *result);
int ph_media_worker_busy(void);

#endif
