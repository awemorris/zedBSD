/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mikuterm's side of the X server of one SSH connection (x11-host.c):
 * the child process that runs it, and the X clients' channels carried
 * between the connection's engine and the child over the link
 * (x11-link.h).
 *
 * The connection's network thread owns it.  The first X client the
 * remote sshd forwards starts the child (mikuterm.exe --x11-server, in a
 * Job object that ends it with mikuterm, at a lower integrity level where
 * Windows allows); the engine's events of the X11 channels are handed in,
 * and x11_host_pump moves the bytes both ways each round, as fast as the
 * credit of the link and the SSH window let it.  A child that fails, stops
 * answering or breaks the link's rules is ended and its channels closed;
 * the next X client starts another, three times at most.
 */

#ifndef MIKUTERM_X11_HOST_H
#define MIKUTERM_X11_HOST_H

#include "x11-link.h"

#include <stddef.h>
#include <stdint.h>

struct kl_socket;
struct ssh_engine;

/* The fake MIT-MAGIC-COOKIE-1's bytes. */
#define X11_HOST_COOKIE		16U

/* The settings' flags of a server (in the HELLO to the child). */
#define X11_HOST_TRUSTED	(1U << 0)
#define X11_HOST_PRIMARY	(1U << 1)
#define X11_HOST_RENDER_CPU	(1U << 2)

/* The longest host name given to the child (for its titles). */
#define X11_HOST_NAME_MAX	256U

/* The longest line x11_host_pump gives for the tab. */
#define X11_HOST_MESSAGE_MAX	256U

/*
 * What the X server of a connection is started with: the remote host's
 * name (the windows' titles), the fake cookie its clients must present,
 * the scale (0: the monitor's), the flags, and the size of the root
 * window shown as one window (a width of 0: each X window a window of its
 * own).
 */
struct x11_host_settings {
	char host[X11_HOST_NAME_MAX];
	uint8_t cookie[X11_HOST_COOKIE];
	unsigned scale;
	unsigned flags;
	unsigned root_width;
	unsigned root_height;
};

/*
 * One X client's channel: the engine's channel number and the link id,
 * the bytes from the remote client not yet sent to the child and those
 * from the child not yet written to the connection, and how each end
 * has ended.
 */
struct x11_host_channel {
	int used;
	uint32_t channel;
	uint32_t id;
	uint8_t *to_child;
	size_t to_child_size;
	uint8_t *to_remote;
	size_t to_remote_size;
	int remote_eof;
	int eof_sent;
	int child_closed;
};

/*
 * The X server of one connection as its network thread sees it.
 */
struct x11_host {
	struct x11_host_settings settings;
	void *process;
	void *job;
	unsigned long pid;
	struct kl_socket *socket;
	struct kl_socket *child_end;
	struct x11_link_table table;
	struct x11_host_channel channels[X11_LINK_CHANNELS];
	uint8_t *out;
	size_t out_size;
	size_t out_capacity;
	uint8_t *in;
	size_t in_size;
	int hello;
	uint64_t started_ms;
	uint64_t heard_ms;
	uint64_t ping_ms;
	int ping_waiting;
	unsigned starts;
	char message[X11_HOST_MESSAGE_MAX];
};

void x11_host_init(struct x11_host *host, const struct x11_host_settings *settings);
void x11_host_end(struct x11_host *host, struct ssh_engine *engine);
int x11_host_accept(struct x11_host *host, uint32_t channel, uint64_t now);
void x11_host_data(struct x11_host *host, struct ssh_engine *engine, uint32_t channel, const uint8_t *bytes, size_t size);
void x11_host_eof(struct x11_host *host, uint32_t channel);
void x11_host_closed(struct x11_host *host, uint32_t channel);
int x11_host_pump(struct x11_host *host, struct ssh_engine *engine, uint64_t now);
struct kl_socket *x11_host_socket(const struct x11_host *host, int *writable);
int x11_host_open_count(const struct x11_host *host);
void x11_host_foreground(const struct x11_host *host);
int mikuterm_x11_child(void);

#endif
