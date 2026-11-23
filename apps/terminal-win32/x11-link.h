/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The link between mikuterm and its X server process: the frames on the
 * loopback socket between them, and the checks each side makes of what
 * the other sends.
 *
 * mikuterm runs the X server of one SSH connection in a child process
 * (mikuterm.exe --x11-server).  Each X client the remote sshd forwards is
 * an "x11" channel of the connection, which the parent numbers with a
 * link id that only grows (never reused on one link) and carries over
 * the socket in frames: a type byte, the link id and the body's length
 * (both 32-bit little endian), then the body.  The receiver of a channel's
 * DATA has given its sender credit (bytes it may send), and gives more
 * with CREDIT frames as it takes them, so one stuck X client stops only
 * its own channel.
 *
 * Each side checks every frame (x11_link_check): an unknown type, a body
 * too long, DATA past the credit, an id never opened or a credit that
 * overflows is a violation, and the receiver ends the link (the parent
 * ends the child).  DATA and EOF of a channel already closed are dropped
 * (they crossed the close) and counted.
 *
 * Pure: no Windows header, no system call; the tests run on the host.
 */

#ifndef MIKUTERM_X11_LINK_H
#define MIKUTERM_X11_LINK_H

#include <stddef.h>
#include <stdint.h>

/* The frames' types. */
#define X11_LINK_HELLO		1U
#define X11_LINK_OPEN		2U
#define X11_LINK_DATA		3U
#define X11_LINK_EOF		4U
#define X11_LINK_CLOSE		5U
#define X11_LINK_CREDIT		6U
#define X11_LINK_PING		7U
#define X11_LINK_PONG		8U
#define X11_LINK_SHUTDOWN	9U

/* The header's size, the most bytes one frame carries, and the link's version. */
#define X11_LINK_HEADER		9U
#define X11_LINK_BODY_MAX	65536U
#define X11_LINK_VERSION	1U

/* The X clients one link carries at a time. */
#define X11_LINK_CHANNELS	32U

/* The credit each side starts a channel with: the parent may send 192 KiB, the child 256 KiB. */
#define X11_LINK_CREDIT_TO_CHILD	(192U * 1024U)
#define X11_LINK_CREDIT_TO_PARENT	(256U * 1024U)

/* The most credit a channel may ever be given (more is a violation). */
#define X11_LINK_CREDIT_MAX	(16U * 1024U * 1024U)

/* Which side a table is kept by. */
#define X11_LINK_PARENT		0
#define X11_LINK_CHILD		1

/* What x11_link_check says of a frame: take it, drop it (it crossed a close), or end the link. */
#define X11_LINK_TAKE		0
#define X11_LINK_DROP		1
#define X11_LINK_VIOLATION	2

/*
 * One frame as read: its type, link id, body and the body's length.  The
 * body points into the bytes read.
 */
struct x11_link_frame {
	unsigned type;
	uint32_t id;
	const uint8_t *body;
	uint32_t size;
};

/*
 * One open channel of a link: its id, the credit this side may still
 * send on it and the credit it gave the other side, and whether each
 * direction has ended (EOF).
 */
struct x11_link_channel {
	uint32_t id;
	int used;
	uint32_t credit_out;
	uint32_t credit_in;
	int eof_in;
	int eof_out;
};

/*
 * The channels of a link as one side sees them, the next id the parent
 * gives (every id below it was opened once), and the frames dropped.
 */
struct x11_link_table {
	int side;
	struct x11_link_channel channels[X11_LINK_CHANNELS];
	uint32_t next_id;
	unsigned dropped;
};

int x11_link_parse(const uint8_t *bytes, size_t size, struct x11_link_frame *frame, size_t *used);
void x11_link_header(uint8_t *header, unsigned type, uint32_t id, uint32_t size);
void x11_link_table_init(struct x11_link_table *table, int side);
struct x11_link_channel *x11_link_find(struct x11_link_table *table, uint32_t id);
int x11_link_open(struct x11_link_table *table, uint32_t id, struct x11_link_channel **channel);
void x11_link_close(struct x11_link_table *table, uint32_t id);
int x11_link_check(struct x11_link_table *table, const struct x11_link_frame *frame);
uint32_t x11_link_room(const struct x11_link_table *table, uint32_t id);
void x11_link_sent(struct x11_link_table *table, uint32_t id, uint32_t size);
void x11_link_gave(struct x11_link_table *table, uint32_t id, uint32_t size);
void x11_link_put32(uint8_t *bytes, uint32_t value);
uint32_t x11_link_get32(const uint8_t *bytes);

#endif
