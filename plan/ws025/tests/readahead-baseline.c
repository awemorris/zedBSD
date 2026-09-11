/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Link-only comparison: retain demand/cache/predictor paths, refuse optional I/O. */
#include <kern/readahead.h>
#include <errno.h>

/*
 * Refuses speculative admission for a controlled sequential baseline kernel.
 */
int
__wrap_readahead_submit(
	struct file *origin,
	struct inode *inode,
	const struct readahead_request *request)
{
	(void)origin;
	(void)inode;
	(void)request;
	return EAGAIN;
}
