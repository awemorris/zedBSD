/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The encoded terminal modes of a pty-req.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "ssh-ttymodes.h"

/*
 * Appends the encoded modes of terminal (every opcode of the table whose
 * flag or character exists locally, the two speeds in bits per second,
 * then TTY_OP_END) to out, as a raw sequence (the caller wraps it in a
 * string).
 */
void
ssh_ttymodes_put(
	const struct termios *terminal,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)terminal;
	(void)out;
}

/*
 * Applies size bytes of encoded modes to terminal (which holds the
 * pseudo-terminal's current settings): each known opcode sets or clears
 * its flag (argument 0 clears, any other sets) or sets its character
 * (the argument's low 8 bits; 255 is taken as _POSIX_VDISABLE where that
 * is the local value), the speeds go to cfsetispeed and cfsetospeed when
 * they name a speed the local system has (otherwise they are ignored).
 */
int
ssh_ttymodes_apply(
	const uint8_t *modes,
	size_t size,
	struct termios *terminal)
{
	/* Marks the arguments used until the function is implemented. */
	(void)modes;
	(void)size;
	(void)terminal;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
