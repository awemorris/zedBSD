/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The encoded terminal modes of a "pty-req" (RFC 4254 8, and IUTF8 of
 * RFC 8160): written by the client from its terminal's settings, applied
 * by the server to the new pseudo-terminal.
 *
 * The encoding is a run of (byte opcode, uint32 argument) pairs ended by
 * TTY_OP_END (0).  Opcodes 1 to 159 take a uint32 argument; 160 and above
 * are not defined and stop the parsing (RFC 4254 8: "Opcodes 160 to 255
 * are not yet defined, and cause parsing to stop").  An opcode this side
 * does not know, or whose flag or character the local <termios.h> does not
 * have, is skipped.  The table of opcodes (RFC 4254 8, RFC 8160 3) is data
 * in the .c file, with the block's source given there.
 */

#ifndef SSH_SSH_TTYMODES_H
#define SSH_SSH_TTYMODES_H

#include <termios.h>

#include "ssh-buf.h"

/* The opcodes that are not flags or characters (RFC 4254 8). */
#define SSH_TTY_OP_END		0
#define SSH_TTY_OP_ISPEED	128
#define SSH_TTY_OP_OSPEED	129

/* The longest encoded modes string accepted. */
#define SSH_TTYMODES_MAX	1024U

/*
 * Appends the encoded modes of terminal (every opcode of the table whose
 * flag or character exists locally, the two speeds in bits per second,
 * then TTY_OP_END) to out, as a raw sequence (the caller wraps it in a
 * string).
 */
void ssh_ttymodes_put(const struct termios *terminal, struct ssh_buf *out);

/*
 * Applies size bytes of encoded modes to terminal (which holds the
 * pseudo-terminal's current settings): each known opcode sets or clears
 * its flag (argument 0 clears, any other sets) or sets its character (the
 * argument's low 8 bits; 255 is taken as _POSIX_VDISABLE where that is the
 * local value), the speeds go to cfsetispeed and cfsetospeed when they
 * name a speed the local system has (otherwise they are ignored).
 *
 * size above SSH_TTYMODES_MAX, or an opcode below 160 whose argument runs
 * past the end, is EINVAL (terminal is then not changed).  Running out of
 * bytes without TTY_OP_END is accepted (the end of the string ends the
 * modes).  Returns 0 or EINVAL.
 */
int ssh_ttymodes_apply(const uint8_t *modes, size_t size, struct termios *terminal);

#endif
