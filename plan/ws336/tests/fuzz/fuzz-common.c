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
	int kind;

	if (size == 0)
		return 0;
	kind = data[0] % 4;
	data++;
	size--;
	if (kind == 0) {
		if (ssh_namelist_check((const char *)data, size) == 0)
			(void)ssh_namelist_contains((const char *)data, size, "ssh-ed25519");
	} else if (kind == 1) {
		(void)ssh_base64_decode((const char *)data, size, out, sizeof(out), &out_size);
		if (size <= 3000 && ssh_base64_encoded_size(size) < sizeof(text) * 8)
			(void)ssh_base64_encode(data, size < 300 ? size : 300, (char *)out, sizeof(out), (int)(size & 1));
	} else {
		/* A run of reads driven by the bytes themselves. */
		ssh_reader_init(&reader, data, size);
		while (ssh_reader_error(&reader) == 0 && ssh_reader_left(&reader) > 0) {
			switch (ssh_reader_get_u8(&reader) % 7) {
			case 0: (void)ssh_reader_get_u32(&reader); break;
			case 1: (void)ssh_reader_get_u64(&reader); break;
			case 2: ssh_reader_get_string(&reader, &field, &field_size); break;
			case 3: ssh_reader_get_cstring(&reader, text, sizeof(text)); break;
			case 4: ssh_reader_get_namelist(&reader, &list, &list_size); break;
			case 5: (void)ssh_reader_get_bool(&reader); break;
			default: (void)ssh_reader_get_bytes(&reader, 3); break;
			}
		}

		(void)ssh_reader_finish(&reader);
	}

	return 0;
}
