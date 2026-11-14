/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libFuzzer target: name-lists, base64 and the RFC 4251 reader on any
 * bytes.  The first byte picks what is read.
 */

#include <stdlib.h>
#include <string.h>

#include "ssh-buf.h"
#include "ssh-common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct ssh_reader reader;
	char text[512];
	uint8_t out[4096];
	size_t out_size;
	const uint8_t *field;
	size_t field_size;
	const char *list;
	size_t list_size;
	size_t encode_size;
	size_t left;
	int kind;
	int error;
	int operation;
	int encoded_ok;

	/* An empty input selects nothing to exercise. */
	if (size == 0)
		return 0;

	/* The first byte picks which parser the rest of the input drives. */
	kind = data[0] % 4;
	data++;
	size--;

	/* Runs the parser the first byte picked. */
	if (kind == 0) {
		/* A name list is checked, and searched only when it is well formed. */
		error = ssh_namelist_check((const char *)data, size);
		if (error == 0)
			(void)ssh_namelist_contains((const char *)data, size, "ssh-ed25519");
	} else if (kind == 1) {
		/* Base64 is decoded as given, then the same bytes are encoded when the result fits. */
		(void)ssh_base64_decode((const char *)data, size, out, sizeof(out), &out_size);
		encoded_ok = 0;
		if (size <= 3000) {
			encode_size = ssh_base64_encoded_size(size);
			if (encode_size < sizeof(text) * 8)
				encoded_ok = 1;
		}

		/* Encodes at most the first 300 bytes into the output buffer. */
		if (encoded_ok != 0) {
			encode_size = size;
			if (encode_size > 300)
				encode_size = 300;
			(void)ssh_base64_encode(data, encode_size, (char *)out, sizeof(out), (int)(size & 1));
		}
	} else {
		/* A run of reads driven by the bytes themselves. */
		ssh_reader_init(&reader, data, size);

		/* Reads one field per input byte until the reader fails or runs dry. */
		while (1) {
			error = ssh_reader_error(&reader);
			if (error != 0)
				break;
			left = ssh_reader_left(&reader);
			if (left == 0)
				break;

			/* The byte read decides which kind of field comes next. */
			operation = (int)(ssh_reader_get_u8(&reader) % 7);

			/* Reads the field of that kind. */
			switch (operation) {
			case 0:
				(void)ssh_reader_get_u32(&reader);
				break;
			case 1:
				(void)ssh_reader_get_u64(&reader);
				break;
			case 2:
				ssh_reader_get_string(&reader, &field, &field_size);
				break;
			case 3:
				ssh_reader_get_cstring(&reader, text, sizeof(text));
				break;
			case 4:
				ssh_reader_get_namelist(&reader, &list, &list_size);
				break;
			case 5:
				(void)ssh_reader_get_bool(&reader);
				break;
			default:
				(void)ssh_reader_get_bytes(&reader, 3);
				break;
			}
		}

		/* Finishes the reader to exercise its end check. */
		(void)ssh_reader_finish(&reader);
	}

	/* Succeeded: the input was handled without a crash. */
	return 0;
}
