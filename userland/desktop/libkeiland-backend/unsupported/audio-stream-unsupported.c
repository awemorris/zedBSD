/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sound's playback streams where the backend does not make them yet
 * (WS191: Linux and FreeBSD until their pump, ws191-p004).  The compositor
 * then does not offer kl_audio_v1, so nothing here is reached in use; an
 * open answers ENOTSUP all the same.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <stddef.h>

/* Marks a parameter a function does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/*
 * Tells that streams cannot be made here.
 */
int
kl_backend_audio_stream_supported(
	void)
{
	/* Succeeded: this backend makes no streams. */
	return 0;
}

/*
 * Refuses to make a stream.
 */
struct kl_backend_audio_stream *
kl_backend_audio_stream_open(
	const struct kl_backend_audio_stream_format *format)
{
	UNUSED_PARAMETER(format);

	/* No streams here. */
	errno = ENOTSUP;
	return NULL;
}

/*
 * Refuses a control: there is no stream.
 */
int
kl_backend_audio_stream_control(
	struct kl_backend_audio_stream *stream,
	unsigned what,
	uint32_t request)
{
	UNUSED_PARAMETER(stream);
	UNUSED_PARAMETER(what);
	UNUSED_PARAMETER(request);

	/* No stream is ready. */
	return ENOTCONN;
}

/*
 * Reports that nothing came of a stream.
 */
int
kl_backend_audio_stream_next(
	struct kl_backend_audio_stream *stream,
	struct kl_backend_audio_stream_report *report)
{
	UNUSED_PARAMETER(stream);
	UNUSED_PARAMETER(report);

	/* Succeeded: nothing came. */
	return 0;
}

/*
 * Ends a stream: there is none.
 */
void
kl_backend_audio_stream_close(
	struct kl_backend_audio_stream *stream)
{
	UNUSED_PARAMETER(stream);
}

/*
 * Lets ended pump threads go: there are none.
 */
void
kl_backend_audio_stream_reap(
	void)
{
	/* Succeeded: nothing to let go. */
	return;
}

/*
 * Waits for every pump thread to end: there are none.
 */
void
kl_backend_audio_stream_reap_all(
	void)
{
	/* Succeeded: nothing to wait for. */
	return;
}
