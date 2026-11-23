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

/*
 * The frames of the X windows shown in mikuterm's tabs (a Host's
 * X11WindowTabify), each with the X window's id as its link id.  The
 * child sends a tab's opening (its size in X's pixels, the X server's
 * scale in percent and its title), a new title, a new size, a part of its
 * pixels (the part's place and size, then its pixels of 0xAARRGGBB, rows
 * of the part's width) and its end.  The parent sends the user's input
 * in the tab (X11_LINK_INPUT_*), the size the tab gives the window (X's
 * pixels), the user's closing of the tab, and its refusal of a tab it has
 * no room for (the child then shows the window as a window of its own).
 * A frame of a window the receiver no longer has (it crossed the window's
 * end, a closing or a refusal) is dropped.
 */
#define X11_LINK_TAB_OPEN	10U
#define X11_LINK_TAB_TITLE	11U
#define X11_LINK_TAB_SIZE	12U
#define X11_LINK_TAB_PIXELS	13U
#define X11_LINK_TAB_GONE	14U
#define X11_LINK_TAB_INPUT	15U
#define X11_LINK_TAB_CONFIGURE	16U
#define X11_LINK_TAB_CLOSE	17U
#define X11_LINK_TAB_REFUSE	18U

/*
 * The fixed parts of the tabs' frames: an opening's size and scale, a
 * size, a part's place and size, an input's kind and its three numbers
 * (an input of text has its UTF-8 after the kind instead), and the
 * longest title.
 */
#define X11_LINK_TAB_OPEN_FIXED		12U
#define X11_LINK_TAB_SIZE_BODY		8U
#define X11_LINK_TAB_PIXELS_FIXED	16U
#define X11_LINK_TAB_INPUT_BODY		16U
#define X11_LINK_TAB_TITLE_MAX		1024U

/*
 * The inputs of a tab, each its kind and three numbers: a key (its evdev
 * code, and 1 pressed or 0 released), text an input method typed (its
 * UTF-8 in place of the numbers), the pointer (X's button 1 to 9, 0 for
 * a move, with 0x100 when pressed; its place on the window in X's pixels,
 * as signed numbers), the keyboard's focus coming (1) or going (0), and
 * the pointer leaving the window.
 */
#define X11_LINK_INPUT_KEY	1U
#define X11_LINK_INPUT_TEXT	2U
#define X11_LINK_INPUT_POINTER	3U
#define X11_LINK_INPUT_FOCUS	4U
#define X11_LINK_INPUT_LEAVE	5U

/* The bit of a pointer's input that says its button went down. */
#define X11_LINK_INPUT_PRESSED	0x100U

/*
 * The most tabs of X windows one link has at a time, the longest side of
 * one and its most pixels (16 M, 64 MiB at four bytes a pixel); a larger
 * window stays a window of its own.
 */
#define X11_LINK_TABS		32U
#define X11_LINK_TAB_SIDE_MAX	8192U
#define X11_LINK_TAB_AREA_MAX	(4096U * 4096U)

/* The header's size, the most bytes one frame carries, and the link's version. */
#define X11_LINK_HEADER		9U
#define X11_LINK_BODY_MAX	65536U
#define X11_LINK_VERSION	3U

/* The X clients one link carries at a time. */
#define X11_LINK_CHANNELS	32U

/*
 * The credit each side starts a channel with: the parent may send 192 KiB,
 * the child 256 KiB.  The child keeps the parent's credit and what its
 * core holds of the client at 192 KiB, or at the whole of a longer
 * request the core holds a part of (up to X11_LINK_CREDIT_MAX), since the
 * core hands back nothing of a request until it is whole.
 */
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
