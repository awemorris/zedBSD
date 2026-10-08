/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The two Linux seats of libkeiland-backend (ws131-p006): logind's
 * (seat-logind-linux.c) and the direct root seat (seat-direct-linux.c).
 * seat-linux.c chooses one at open and forwards the seat's calls to it.
 */
#ifndef KL_BACKEND_SEAT_LINUX_H
#define KL_BACKEND_SEAT_LINUX_H

#include "userland/desktop/libkeiland-backend/backend-private.h"

/* The leases: slot zero is the primary node's, the others the input devices' (the compositor reads at most 32, KWL_INPUT_MAX). */
#define LINUX_SEAT_DEVICES 33U

/* The direct root seat: the primary node and the input devices opened by path. */
int linux_direct_seat_open(void);
void linux_direct_seat_close(void);
int linux_direct_device_open(const char *path);
void linux_direct_device_close(int descriptor);
int linux_direct_drm_fd(void);
const char *linux_direct_drm_path(void);

/* logind's seat: TakeControl, TakeDevice and the PauseDevice and ResumeDevice signals. */
int linux_logind_seat_open(struct kl_backend *backend);
void linux_logind_seat_close(void);
int linux_logind_device_open(const char *path);
void linux_logind_device_close(int descriptor);
int linux_logind_drm_fd(void);
const char *linux_logind_drm_path(void);
int linux_logind_seat_paused(void);
int linux_logind_poll_fd(void);
int linux_logind_dispatch(struct kl_backend *backend);
int linux_logind_device_revoked(int descriptor);

#endif
