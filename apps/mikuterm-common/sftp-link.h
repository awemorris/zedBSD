/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The relay between a connection's network thread (ssh-thread.c) and an
 * SFTP tab's worker thread (sftp-worker.c).
 *
 * The network thread owns the SSH engine and moves bytes between the
 * subsystem channel and the two rings here; it never parses SFTP
 * packets.  The worker thread owns sftp_client, the transfer queue, and
 * local file I/O.  Four holders keep a reference: the terminal session,
 * the network thread, the worker thread, and the SFTP tab; the last to
 * release frees the relay.
 */

#ifndef MIKUTERM_SFTP_LINK_H
#define MIKUTERM_SFTP_LINK_H

#include "userland/base/ssh/ssh-client-engine.h"

#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>

/*
 * The incoming ring's capacity: one and a half subsystem windows so a
 * server that obeys the channel window never overflows it.
 */
#define SFTP_LINK_IN_MAX		(SSH_ENGINE_SUBSYSTEM_WINDOW + SSH_ENGINE_SUBSYSTEM_WINDOW / 2U)

/* The outgoing ring's capacity, in bytes. */
#define SFTP_LINK_OUT_MAX		(512U * 1024U)

/* How many incoming bytes the worker reads before waking the network thread to return window. */
#define SFTP_LINK_CONSUMED_STEP		65536U

/* Engine output backlog threshold above which SFTP writes pause so terminal keystrokes stay responsive. */
#define SFTP_LINK_ENGINE_BACKPRESSURE	(256U * 1024U)

/* The longest failure reason kept from SSH_ENGINE_SUBSYSTEM_REPLY. */
#define SFTP_LINK_REASON_MAX		256U

struct sftp_link;

int sftp_link_create(struct kl_socket_waker *network_waker, struct sftp_link **link_out);
void sftp_link_retain(struct sftp_link *link);
void sftp_link_release(struct sftp_link *link);

/* Worker and UI side. */
void sftp_link_request_open(struct sftp_link *link);
void sftp_link_request_close(struct sftp_link *link);
int sftp_link_write(struct sftp_link *link, const uint8_t *bytes, size_t size, size_t *written);
int sftp_link_read(struct sftp_link *link, uint8_t *bytes, size_t capacity, size_t *read_size, int *eof, int *closed);
void sftp_link_wait(struct sftp_link *link, uint32_t timeout_ms);
void sftp_link_wake_worker(struct sftp_link *link);
int sftp_link_reply(struct sftp_link *link, int *ready, int *accepted, uint32_t *code, char *reason, size_t reason_cap);
int sftp_link_is_ended(struct sftp_link *link);
int sftp_link_is_open(struct sftp_link *link);
size_t sftp_link_out_pending(struct sftp_link *link);

/* Network thread side (called with link->lock released). */
void sftp_link_pump(struct sftp_link *link, struct ssh_engine *engine);
void sftp_link_event(struct sftp_link *link, struct ssh_engine *engine, const struct ssh_engine_event *event, int *taken);
void sftp_link_end(struct sftp_link *link);
int sftp_link_channel_open(struct sftp_link *link);

/* Direct relay helpers for pipe-backed host tests. */
int sftp_link_take_open(struct sftp_link *link);
int sftp_link_take_close(struct sftp_link *link);
void sftp_link_push_reply(struct sftp_link *link, uint32_t channel, int accepted, uint32_t code, const char *reason);
size_t sftp_link_take_out(struct sftp_link *link, uint8_t *bytes, size_t capacity);
void sftp_link_push_in(struct sftp_link *link, const uint8_t *bytes, size_t size);

#endif
