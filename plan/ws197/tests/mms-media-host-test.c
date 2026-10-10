/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Exercises binary MMS round trips, captions and media-only bMessages. */
#include "userland/base/bluetoothd/bmsg.h"
#include "userland/desktop/libmms/mms.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * Runs the production MIME writer, reader and MAP bMessage reader together.
 */
int
main(
	void)
{
	struct mms_document document;
	struct btd_bmsg message;
	FILE *stream;
	uint8_t *data;
	uint8_t *mime;
	uint8_t *body;
	size_t length;
	size_t count;
	size_t used;
	size_t index;
	long end;
	int error;
	int same;
	int closed;
	const char *type;
	const char *caption;
	char framed[512];
	int written;
	static const char *const outer[] = { "text/plain", "image/jpeg", "application/vnd.wap.multipart.related" };

	/* MAP preserves a leaf or WAP type while still framing its parts with a boundary. */
	for (index = 0U; index < 3U; index++) {
		type = "image/jpeg";
		caption = "Content-Transfer-Encoding: base64\r\n\r\nAQIDBA==";
		if (index == 0U) {
			type = "text/plain";
			caption = "Content-Transfer-Encoding: 8BIT\r\n\r\nHello";
		}

		/* Builds a sanitized phone-shaped body rather than using our standard MIME writer. */
		written = snprintf(framed, sizeof(framed), "Content-Type: %s; boundary=b\r\n\r\n--b\r\nContent-Type: %s\r\n%s\r\n--b--\r\n", outer[index], type, caption);
		assert(written > 0 && (size_t)written < sizeof(framed));
		error = mms_parse((const uint8_t *)framed, (size_t)written, &document);
		assert(error == 0);

		/* Part headers never become a caption or the original binary payload. */
		if (index == 0U) {
			same = strcmp(document.text, "Hello");
			assert(same == 0 && document.count == 0U);
		} else {
			assert(document.text_length == 0U && document.count == 1U);
			assert(document.media[0].length == 4U && document.media[0].data[0] == 1U && document.media[0].data[3] == 4U);
		}

		/* Releases the decoded ownership before testing another framing type. */
		mms_release(&document);
	}

	/* Covers payloads larger than the old 64 KiB limit, including every byte value. */
	length = 262147U;
	data = malloc(length);
	assert(data != NULL);
	for (index = 0U; index < length; index++)
		data[index] = (uint8_t)index;

	/* Exercises both media classes with and without a Japanese caption. */
	for (index = 0U; index < 2U; index++) {
		type = "image/jpeg";
		caption = "日本語の写真";
		if (index != 0U) {
			type = "video/mp4";
			caption = "";
		}

		/* Writes the original bytes without image/video transcoding. */
		stream = tmpfile();
		assert(stream != NULL);
		error = mms_write(stream, type, "test.bin", data, length, caption);
		assert(error == 0);
		error = fflush(stream);
		assert(error == 0);
		end = ftell(stream);
		assert(end > 0);
		mime = malloc((size_t)end);
		assert(mime != NULL);
		rewind(stream);
		count = fread(mime, 1U, (size_t)end, stream);
		assert(count == (size_t)end);
		closed = fclose(stream);
		assert(closed == 0);

		/* Checks caption and byte-for-byte media preservation. */
		error = mms_parse(mime, count, &document);
		assert(error == 0);
		assert(document.count == 1U && document.bytes == length);
		same = strcmp(document.text, caption);
		assert(same == 0);
		same = strcmp(document.media[0].type, type);
		assert(same == 0);
		same = memcmp(document.media[0].data, data, length);
		assert(same == 0);
		/* Releases the decoded ownership before testing another framing type. */
		mms_release(&document);

		/* Checks that Get and Push carry the entire MIME span, including media-only messages. */
		body = malloc(2U * count + 1024U);
		assert(body != NULL);
		error = btd_bmsg_build("+123", mime, count, BTD_MAP_TYPE_MMS, body, 2U * count + 1024U, &used);
		assert(error == 0);
		error = btd_bmsg_parse(body, used, &message);
		assert(error == 0);
		assert(message.mime != NULL && message.mime_length >= count);
		error = mms_parse(message.mime, message.mime_length, &document);
		assert(error == 0);
		same = memcmp(document.media[0].data, data, length);
		assert(same == 0);
		/* Releases the decoded ownership before testing another framing type. */
		mms_release(&document);
		free(body);
		free(mime);
	}

	/* Refuses an advertised oversize input without reading beyond its pointer. */
	error = mms_parse(data, MMS_INPUT_MAX + 1U, &document);
	assert(error == EINVAL);
	free(data);

	/* Succeeded: both media classes survive the actual MAP envelope. */
	puts("mms-media-host-test: PASS");
	return 0;
}
