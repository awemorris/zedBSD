/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * keiland-shot: writes what the display shows, as the compositor composed
 * it, to a PNG file (ws173-p002, the Agent Acceptance Test).  Test images
 * only: the compositor answers only when the image was built with
 * ZEDBSD_TEST_SCREEN_CAPTURE=y (userland/desktop/wayland/shot.c).
 *
 *   keiland-shot [--socket PATH] [--timeout MS] [--partial] OUT.png
 *   keiland-shot [--socket PATH] [--timeout MS] --request LINE
 *
 * --request sends another request of the capture channel (DISPLAYS, MODE,
 * PLACE of ws113-p004b; POLICY, ROTATE of WS217) and prints its answer.
 * --partial takes the next frame as its damage draws it ("SHOT partial",
 * WS217) instead of asking for a whole one.
 *
 * Without --socket it asks every compositor's socket it finds
 * (/run/user/<uid>/keiland-shot.sock, /tmp/keiland-shot.<uid>.sock) and
 * takes the one that shows the display now (the session's, or the login
 * screen's).  Root can ask any of them; another user only its own.  Prints
 * "KEILAND-SHOT OUT.png WxH" (status 0) or "KEILAND-SHOT error WHY"
 * (status 1).
 *
 * The PNG is 8-bit RGB, its image data stored uncompressed in the deflate
 * stream (zedBSD's libz-compat only inflates); a host tool may recompress.
 */

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* The longest socket path, the most sockets looked at, and the default wait. */
#define SHOT_PATH_MAX		108U
#define SHOT_CANDIDATES		32U
#define SHOT_TIMEOUT_MS		10000

/* The swapchain formats the pixels may come in (VkFormat numbers). */
#define SHOT_R8G8B8A8_UNORM	37
#define SHOT_R8G8B8A8_SRGB	43
#define SHOT_B8G8R8A8_UNORM	44
#define SHOT_B8G8R8A8_SRGB	50

/* The largest stored deflate block. */
#define SHOT_STORED_MAX		65535U

static int shot_find(char paths[][SHOT_PATH_MAX], unsigned capacity);
static int shot_connect(const char *path, int timeout_ms);
static int shot_ask(const char *path, const char *request, char *answer, size_t size, int timeout_ms);
static int shot_read_all(int connection, uint8_t *data, size_t size, int timeout_ms);
static int shot_write_png(const char *out, const uint8_t *pixels, unsigned width, unsigned height, int bgr);
static uint32_t shot_crc(uint32_t crc, const uint8_t *data, size_t size);
static void shot_put32(uint8_t *bytes, uint32_t value);
static int shot_chunk(FILE *file, const char *type, const uint8_t *data, size_t size);

/* Captures the display into a PNG. */
int
main(
	int argc,
	char **argv)
{
	char paths[SHOT_CANDIDATES][SHOT_PATH_MAX];
	char answer[64];
	char line[96];
	char request[160];
	char reply[2048];
	const char *socket_path;
	const char *asked;
	const char *out;
	uint8_t *pixels;
	unsigned width;
	unsigned height;
	unsigned count;
	unsigned index;
	size_t used;
	int timeout;
	int format;
	int connection;
	int error;
	int argument;
	int partial;

	/* The options and the file. */
	socket_path = NULL;
	asked = NULL;
	out = NULL;
	timeout = SHOT_TIMEOUT_MS;
	partial = 0;
	for (argument = 1; argument < argc; argument++) {
		if (strcmp(argv[argument], "--partial") == 0)
			partial = 1;
		else if (strcmp(argv[argument], "--socket") == 0 && argument + 1 < argc)
			socket_path = argv[++argument];
		else if (strcmp(argv[argument], "--request") == 0 && argument + 1 < argc)
			asked = argv[++argument];
		else if (strcmp(argv[argument], "--timeout") == 0 && argument + 1 < argc)
			timeout = atoi(argv[++argument]);
		else
			out = argv[argument];
	}
	if (out == NULL && asked == NULL) {
		fprintf(stderr, "usage: keiland-shot [--socket PATH] [--timeout MS] [--partial] OUT.png | --request LINE\n");
		return 2;
	}

	/* The compositor: the one named, or the one that shows the display. */
	if (socket_path == NULL) {
		count = (unsigned)shot_find(paths, SHOT_CANDIDATES);
		for (index = 0U; index < count; index++) {
			error = shot_ask(paths[index], "PING\n", answer, sizeof(answer), 1000);
			if (error == 0 && strncmp(answer, "ACTIVE", 6U) == 0) {
				socket_path = paths[index];
				break;
			}
		}
		if (socket_path == NULL) {
			printf("KEILAND-SHOT error no-active-compositor (looked at %u socket(s))\n", count);
			return 1;
		}
	}

	/* Another request (the displays' of ws113-p004b): the answer printed as it comes. */
	if (asked != NULL) {
		(void)snprintf(request, sizeof(request), "%s\n", asked);
		error = shot_ask(socket_path, request, reply, sizeof(reply), timeout);
		if (error != 0) {
			printf("KEILAND-SHOT error request errno=%d\n", error);
			return 1;
		}
		printf("%s", reply);
		return 0;
	}

	/* SHOT, and the header. */
	connection = shot_connect(socket_path, timeout);
	if (connection < 0) {
		printf("KEILAND-SHOT error connect %s errno=%d\n", socket_path, errno);
		return 1;
	}
	if (partial)
		(void)write(connection, "SHOT partial\n", 13U);
	else
		(void)write(connection, "SHOT\n", 5U);
	used = 0U;
	while (used + 1U < sizeof(line)) {
		error = shot_read_all(connection, (uint8_t *)line + used, 1U, timeout);
		if (error != 0)
			break;
		used++;
		if (line[used - 1U] == '\n')
			break;
	}
	line[used] = '\0';
	if (sscanf(line, "OK %u %u %d", &width, &height, &format) != 3) {
		printf("KEILAND-SHOT error %s", used != 0U ? line : "no-answer\n");
		(void)close(connection);
		return 1;
	}
	if (width == 0U || height == 0U || width > 16384U || height > 16384U) {
		printf("KEILAND-SHOT error bad-size %ux%u\n", width, height);
		(void)close(connection);
		return 1;
	}

	/* The pixels. */
	pixels = malloc((size_t)width * height * 4U);
	if (pixels == NULL) {
		printf("KEILAND-SHOT error memory\n");
		(void)close(connection);
		return 1;
	}
	error = shot_read_all(connection, pixels, (size_t)width * height * 4U, timeout);
	(void)close(connection);
	if (error != 0) {
		printf("KEILAND-SHOT error pixels errno=%d\n", error);
		free(pixels);
		return 1;
	}

	/* The PNG. */
	if (format != SHOT_R8G8B8A8_UNORM && format != SHOT_R8G8B8A8_SRGB && format != SHOT_B8G8R8A8_UNORM &&
	    format != SHOT_B8G8R8A8_SRGB) {
		printf("KEILAND-SHOT error format %d\n", format);
		free(pixels);
		return 1;
	}
	error = shot_write_png(out, pixels, width, height, format == SHOT_B8G8R8A8_UNORM || format == SHOT_B8G8R8A8_SRGB);
	free(pixels);
	if (error != 0) {
		printf("KEILAND-SHOT error write errno=%d\n", error);
		return 1;
	}
	printf("KEILAND-SHOT %s %ux%u\n", out, width, height);
	return 0;
}

/* Lists the capture sockets there are: /run/user/<uid>/ and /tmp/. */
static int
shot_find(
	char paths[][SHOT_PATH_MAX],
	unsigned capacity)
{
	struct dirent *entry;
	DIR *directory;
	unsigned count;

	/* Each user's runtime directory. */
	count = 0U;
	directory = opendir("/run/user");
	if (directory != NULL) {
		while (count < capacity && (entry = readdir(directory)) != NULL) {
			if (entry->d_name[0] == '.' || strlen(entry->d_name) > 32U)
				continue;
			snprintf(paths[count], SHOT_PATH_MAX, "/run/user/%.32s/keiland-shot.sock", entry->d_name);
			if (access(paths[count], F_OK) == 0)
				count++;
		}
		(void)closedir(directory);
	}

	/* /tmp (the login screen, and a desktop without a runtime directory). */
	directory = opendir("/tmp");
	if (directory != NULL) {
		while (count < capacity && (entry = readdir(directory)) != NULL) {
			if (strncmp(entry->d_name, "keiland-shot.", 13U) != 0 || strlen(entry->d_name) > 64U)
				continue;
			snprintf(paths[count], SHOT_PATH_MAX, "/tmp/%.64s", entry->d_name);
			count++;
		}
		(void)closedir(directory);
	}
	return (int)count;
}

/* Connects to a socket; returns it, or -1. */
static int
shot_connect(
	const char *path,
	int timeout_ms)
{
	struct sockaddr_un address;
	int connection;

	(void)timeout_ms;
	connection = socket(AF_UNIX, SOCK_STREAM, 0);
	if (connection < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
	if (connect(connection, (struct sockaddr *)&address, sizeof(address)) != 0) {
		(void)close(connection);
		return -1;
	}
	return connection;
}

/* Sends a request and reads a one-line answer. */
static int
shot_ask(
	const char *path,
	const char *request,
	char *answer,
	size_t size,
	int timeout_ms)
{
	struct pollfd wait;
	ssize_t count;
	int connection;

	connection = shot_connect(path, timeout_ms);
	if (connection < 0)
		return errno;
	(void)write(connection, request, strlen(request));
	wait.fd = connection;
	wait.events = POLLIN;
	wait.revents = 0;
	if (poll(&wait, 1, timeout_ms) <= 0) {
		(void)close(connection);
		return ETIMEDOUT;
	}
	count = read(connection, answer, size - 1U);
	(void)close(connection);
	if (count <= 0)
		return EIO;
	answer[count] = '\0';
	return 0;
}

/* Reads exactly size bytes, waiting at most timeout_ms between parts. */
static int
shot_read_all(
	int connection,
	uint8_t *data,
	size_t size,
	int timeout_ms)
{
	struct pollfd wait;
	ssize_t count;
	size_t done;

	done = 0U;
	while (done < size) {
		wait.fd = connection;
		wait.events = POLLIN;
		wait.revents = 0;
		if (poll(&wait, 1, timeout_ms) <= 0)
			return ETIMEDOUT;
		count = read(connection, data + done, size - done);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return EIO;
		done += (size_t)count;
	}
	return 0;
}

/*
 * Writes an 8-bit RGB PNG: IHDR, one IDAT holding a zlib stream of stored
 * deflate blocks (each scanline with filter 0), IEND.
 */
static int
shot_write_png(
	const char *out,
	const uint8_t *pixels,
	unsigned width,
	unsigned height,
	int bgr)
{
	static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	uint8_t header[13];
	uint8_t *raw;
	uint8_t *stream;
	size_t raw_size;
	size_t blocks;
	size_t stream_size;
	size_t offset;
	size_t at;
	size_t part;
	size_t row;
	size_t column;
	uint32_t a;
	uint32_t b;
	FILE *file;
	int error;

	/* The scanlines: a filter byte, then RGB. */
	raw_size = (size_t)height * (1U + (size_t)width * 3U);
	raw = malloc(raw_size);
	if (raw == NULL)
		return ENOMEM;
	at = 0U;
	for (row = 0U; row < height; row++) {
		raw[at++] = 0U;
		for (column = 0U; column < width; column++) {
			offset = (row * width + column) * 4U;
			raw[at++] = pixels[offset + (bgr ? 2U : 0U)];
			raw[at++] = pixels[offset + 1U];
			raw[at++] = pixels[offset + (bgr ? 0U : 2U)];
		}
	}

	/* The zlib stream: its header, stored blocks, and the Adler-32 of the scanlines. */
	blocks = (raw_size + SHOT_STORED_MAX - 1U) / SHOT_STORED_MAX;
	stream_size = 2U + blocks * 5U + raw_size + 4U;
	stream = malloc(stream_size);
	if (stream == NULL) {
		free(raw);
		return ENOMEM;
	}
	stream[0] = 0x78U;
	stream[1] = 0x01U;
	at = 2U;
	for (offset = 0U; offset < raw_size; offset += part) {
		part = raw_size - offset;
		if (part > SHOT_STORED_MAX)
			part = SHOT_STORED_MAX;
		stream[at++] = (uint8_t)(offset + part == raw_size ? 1U : 0U);
		stream[at++] = (uint8_t)part;
		stream[at++] = (uint8_t)(part >> 8U);
		stream[at++] = (uint8_t)~part;
		stream[at++] = (uint8_t)(~part >> 8U);
		memcpy(stream + at, raw + offset, part);
		at += part;
	}
	a = 1U;
	b = 0U;
	for (offset = 0U; offset < raw_size; offset++) {
		a = (a + raw[offset]) % 65521U;
		b = (b + a) % 65521U;
	}
	shot_put32(stream + at, (b << 16U) | a);
	at += 4U;
	free(raw);

	/* The file. */
	file = fopen(out, "wb");
	if (file == NULL) {
		error = errno;
		free(stream);
		return error;
	}
	shot_put32(header, width);
	shot_put32(header + 4U, height);
	header[8] = 8U;
	header[9] = 2U;
	header[10] = 0U;
	header[11] = 0U;
	header[12] = 0U;
	error = fwrite(signature, 1U, sizeof(signature), file) == sizeof(signature) ? 0 : EIO;
	if (error == 0)
		error = shot_chunk(file, "IHDR", header, sizeof(header));
	if (error == 0)
		error = shot_chunk(file, "IDAT", stream, at);
	if (error == 0)
		error = shot_chunk(file, "IEND", NULL, 0U);
	free(stream);
	if (fclose(file) != 0 && error == 0)
		error = EIO;
	return error;
}

/* Writes one chunk: its length, type, data and CRC. */
static int
shot_chunk(
	FILE *file,
	const char *type,
	const uint8_t *data,
	size_t size)
{
	uint8_t word[4];
	uint32_t crc;

	shot_put32(word, (uint32_t)size);
	if (fwrite(word, 1U, 4U, file) != 4U)
		return EIO;
	if (fwrite(type, 1U, 4U, file) != 4U)
		return EIO;
	if (size != 0U && fwrite(data, 1U, size, file) != size)
		return EIO;
	crc = shot_crc(0xffffffffU, (const uint8_t *)type, 4U);
	if (size != 0U)
		crc = shot_crc(crc, data, size);
	shot_put32(word, crc ^ 0xffffffffU);
	if (fwrite(word, 1U, 4U, file) != 4U)
		return EIO;
	return 0;
}

/* Continues a CRC-32 (the PNG polynomial) over bytes. */
static uint32_t
shot_crc(
	uint32_t crc,
	const uint8_t *data,
	size_t size)
{
	size_t index;
	unsigned bit;

	for (index = 0U; index < size; index++) {
		crc ^= data[index];
		for (bit = 0U; bit < 8U; bit++)
			crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
	}
	return crc;
}

/* Writes a 32-bit big-endian number. */
static void
shot_put32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0] = (uint8_t)(value >> 24U);
	bytes[1] = (uint8_t)(value >> 16U);
	bytes[2] = (uint8_t)(value >> 8U);
	bytes[3] = (uint8_t)value;
}
