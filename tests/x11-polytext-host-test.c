/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit test for X11 PolyText8 / PolyText16 wire encoding in libX11
 * and transparent vs background-filled text rendering in xserver.
 *
 * Build and run from the top of the tree:
 *   mkdir -p build/tmp/x11-inc
 *   cp -r include/libc/X11 build/tmp/x11-inc/
 *   cc -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
 *      -fsanitize=address,undefined -fno-sanitize-recover=all \
 *      -I. -Ibuild/tmp/x11-inc -Iuserland/desktop/include \
 *      tests/x11-polytext-host-test.c \
 *      userland/desktop/xserver/font.c \
 *      -o build/tmp/x11-polytext-host-test
 *   build/tmp/x11-polytext-host-test
 */

#include "userland/x11/libX11/xlib.c"
#include "userland/desktop/xserver/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned test_checks;
static unsigned test_failures;

static struct x11_gc test_gc;
static struct x11_font *test_font;
static int test_draw_calls;
static int test_last_image;
static size_t test_last_count;
static struct x11_box test_last_bg;
static int test_last_glyph_x[16];

static void test_check(const char *what, int passed);
static int read_exact(int fd, uint8_t *buf, size_t n);
static void test_libx11_polytext8(void);
static void test_libx11_polytext8_long(void);
static void test_libx11_polytext16(void);
static void test_xserver_polytext_vs_imagetext(void);

/*
 * Runs all PolyText8/PolyText16 and ImageText8/ImageText16 checks.
 */
int
main(
	void)
{
	test_libx11_polytext8();
	test_libx11_polytext8_long();
	test_libx11_polytext16();
	test_xserver_polytext_vs_imagetext();

	if (test_failures != 0U) {
		printf("x11-polytext-host-test: FAIL (%u of %u checks)\n",
		       test_failures, test_checks);
		return 1;
	}

	printf("x11-polytext-host-test: PASS (%u checks)\n", test_checks);
	return 0;
}

/* Records one check outcome and reports failures immediately. */
static void
test_check(
	const char *what,
	int passed)
{
	test_checks++;
	if (!passed) {
		test_failures++;
		fprintf(stderr, "FAIL: %s\n", what);
	}
}

/* Reads exactly n bytes from fd into buf. */
static int
read_exact(
	int fd,
	uint8_t *buf,
	size_t n)
{
	size_t off;
	ssize_t got;

	off = 0U;
	while (off < n) {
		got = read(fd, buf + off, n - off);
		if (got <= 0)
			return -1;
		off += (size_t)got;
	}
	return 0;
}

/* Verifies XDrawString encodes a PolyText8 (opcode 74) request with one TEXTITEM8. */
static void
test_libx11_polytext8(
	void)
{
	int sv[2];
	Display d;
	struct _XGC gc;
	uint8_t buf[32];
	int rc;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		test_check("polytext8 socketpair", 0);
		return;
	}

	memset(&d, 0, sizeof(d));
	memset(&gc, 0, sizeof(gc));
	d.fd = sv[0];
	gc.xid = 0x00400002U;

	rc = XDrawString(&d, 0x00400001U, &gc, 12, 34, "Hello", 5);
	test_check("XDrawString returns 0", rc == 0);
	test_check("XDrawString reads 24-byte PolyText8 request",
		   read_exact(sv[1], buf, 24U) == 0);

	test_check("XDrawString opcode is 74 (PolyText8)", buf[0] == 74U);
	test_check("XDrawString unused byte is 0", buf[1] == 0U);
	test_check("XDrawString length is 6 words", r16(buf + 2) == 6U);
	test_check("XDrawString drawable matches", r32(buf + 4) == 0x00400001U);
	test_check("XDrawString gc matches", r32(buf + 8) == 0x00400002U);
	test_check("XDrawString x matches", (int16_t)r16(buf + 12) == 12);
	test_check("XDrawString y matches", (int16_t)r16(buf + 14) == 34);
	test_check("XDrawString item length is 5", buf[16] == 5U);
	test_check("XDrawString item delta is 0", buf[17] == 0U);
	test_check("XDrawString item text matches", memcmp(buf + 18, "Hello", 5U) == 0);
	test_check("XDrawString trailing pad is 0", buf[23] == 0U);

	close(sv[0]);
	close(sv[1]);
}

/* Verifies XDrawString splits strings longer than 254 chars without emitting 255. */
static void
test_libx11_polytext8_long(
	void)
{
	int sv[2];
	Display d;
	struct _XGC gc;
	char text[260];
	uint8_t buf[280];
	int rc;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		test_check("polytext8 long socketpair", 0);
		return;
	}

	memset(&d, 0, sizeof(d));
	memset(&gc, 0, sizeof(gc));
	memset(text, 'A', 254U);
	memset(text + 254U, 'B', 6U);
	d.fd = sv[0];
	gc.xid = 0x00400003U;

	rc = XDrawString(&d, 0x00400001U, &gc, 0, 16, text, 260);
	test_check("XDrawString(260) returns 0", rc == 0);
	test_check("XDrawString(260) reads 280-byte PolyText8 request",
		   read_exact(sv[1], buf, 280U) == 0);

	test_check("XDrawString(260) opcode is 74", buf[0] == 74U);
	test_check("XDrawString(260) length is 70 words", r16(buf + 2) == 70U);
	test_check("XDrawString(260) chunk 1 len is 254", buf[16] == 254U);
	test_check("XDrawString(260) chunk 1 delta is 0", buf[17] == 0U);
	test_check("XDrawString(260) chunk 1 char is A", buf[18] == 'A');
	test_check("XDrawString(260) chunk 2 len is 6", buf[272] == 6U);
	test_check("XDrawString(260) chunk 2 delta is 0", buf[273] == 0U);
	test_check("XDrawString(260) chunk 2 char is B", buf[274] == 'B');

	close(sv[0]);
	close(sv[1]);
}

/* Verifies XDrawString16 encodes a PolyText16 (opcode 75) request with XChar2b pairs. */
static void
test_libx11_polytext16(
	void)
{
	int sv[2];
	Display d;
	struct _XGC gc;
	XChar2b chars[3];
	uint8_t buf[24];
	int rc;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		test_check("polytext16 socketpair", 0);
		return;
	}

	memset(&d, 0, sizeof(d));
	memset(&gc, 0, sizeof(gc));
	d.fd = sv[0];
	gc.xid = 0x00400004U;
	chars[0].byte1 = 0x00U;
	chars[0].byte2 = 0x41U;
	chars[1].byte1 = 0x30U;
	chars[1].byte2 = 0x42U;
	chars[2].byte1 = 0x00U;
	chars[2].byte2 = 0x5AU;

	rc = XDrawString16(&d, 0x00400001U, &gc, -4, 20, chars, 3);
	test_check("XDrawString16 returns 0", rc == 0);
	test_check("XDrawString16 reads 24-byte PolyText16 request",
		   read_exact(sv[1], buf, 24U) == 0);

	test_check("XDrawString16 opcode is 75 (PolyText16)", buf[0] == 75U);
	test_check("XDrawString16 unused byte is 0", buf[1] == 0U);
	test_check("XDrawString16 length is 6 words", r16(buf + 2) == 6U);
	test_check("XDrawString16 item length is 3", buf[16] == 3U);
	test_check("XDrawString16 item delta is 0", buf[17] == 0U);
	test_check("XDrawString16 char 0 big-endian", buf[18] == 0x00U && buf[19] == 0x41U);
	test_check("XDrawString16 char 1 big-endian", buf[20] == 0x30U && buf[21] == 0x42U);
	test_check("XDrawString16 char 2 big-endian", buf[22] == 0x00U && buf[23] == 0x5AU);

	close(sv[0]);
	close(sv[1]);
}

/* Verifies xserver PolyText8/16 is transparent while ImageText8/16 fills background. */
static void
test_xserver_polytext_vs_imagetext(
	void)
{
	struct x11server server;
	struct x11_client client;
	uint8_t open_req[24];
	uint8_t img8_req[20];
	uint8_t poly8_req[24];
	uint8_t img16_req[20];
	uint8_t poly16_req[24];
	const char *font_name;
	unsigned code;

	memset(&server, 0, sizeof(server));
	memset(&client, 0, sizeof(client));
	memset(&test_gc, 0, sizeof(test_gc));
	server.clients = &client;
	server.clients_max = 1U;
	test_font = NULL;
	test_gc.id = 0x00200001U;
	test_gc.font = 0x00200002U;
	test_gc.foreground = 0x00ffffffU;
	test_gc.background = 0x00112233U;

	/* Opens the zed-unicode built-in font (opcode 45). */
	font_name = "zed-unicode";
	memset(open_req, 0, sizeof(open_req));
	open_req[0] = 45U;
	x11_write16(open_req + 2, 6U, 0);
	x11_write32(open_req + 4, test_gc.font, 0);
	x11_write16(open_req + 8, (uint16_t)strlen(font_name), 0);
	memcpy(open_req + 12, font_name, strlen(font_name));
	code = x11_font_request(&server, 0U, open_req, sizeof(open_req));
	test_check("xserver OpenFont(zed-unicode) succeeds", code == 0U && test_font != NULL);

	/* ImageText8 (opcode 76) must set text.image = 1 for zed-unicode. */
	memset(img8_req, 0, sizeof(img8_req));
	img8_req[0] = 76U;
	img8_req[1] = 2U;
	x11_write16(img8_req + 2, 5U, 0);
	x11_write32(img8_req + 4, 0x00200003U, 0);
	x11_write32(img8_req + 8, test_gc.id, 0);
	x11_write16(img8_req + 12, 10U, 0);
	x11_write16(img8_req + 14, 20U, 0);
	img8_req[16] = 'A';
	img8_req[17] = 'B';
	test_draw_calls = 0;
	code = x11_font_request(&server, 0U, img8_req, sizeof(img8_req));
	test_check("xserver ImageText8 succeeds", code == 0U);
	test_check("xserver ImageText8 fills background (image == 1)",
		   test_draw_calls == 1 && test_last_image == 1);
	test_check("xserver ImageText8 background box width == 16",
		   test_last_bg.x0 == 10 && test_last_bg.x1 == 26);

	/* PolyText8 (opcode 74) must draw transparently (text.image == 0) and apply delta. */
	memset(poly8_req, 0, sizeof(poly8_req));
	poly8_req[0] = 74U;
	x11_write16(poly8_req + 2, 6U, 0);
	x11_write32(poly8_req + 4, 0x00200003U, 0);
	x11_write32(poly8_req + 8, test_gc.id, 0);
	x11_write16(poly8_req + 12, 10U, 0);
	x11_write16(poly8_req + 14, 20U, 0);
	poly8_req[16] = 1U;
	poly8_req[17] = 0U;
	poly8_req[18] = 'A';
	poly8_req[19] = 1U;
	poly8_req[20] = 4U;
	poly8_req[21] = 'B';
	test_draw_calls = 0;
	code = x11_font_request(&server, 0U, poly8_req, sizeof(poly8_req));
	test_check("xserver PolyText8 succeeds", code == 0U);
	test_check("xserver PolyText8 is transparent (image == 0)",
		   test_draw_calls == 1 && test_last_image == 0);
	test_check("xserver PolyText8 draws 2 glyphs with delta",
		   test_last_count == 2U && test_last_glyph_x[0] == 10 &&
		   test_last_glyph_x[1] == 22);

	/* ImageText16 (opcode 77) must set text.image = 1. */
	memset(img16_req, 0, sizeof(img16_req));
	img16_req[0] = 77U;
	img16_req[1] = 2U;
	x11_write16(img16_req + 2, 5U, 0);
	x11_write32(img16_req + 4, 0x00200003U, 0);
	x11_write32(img16_req + 8, test_gc.id, 0);
	x11_write16(img16_req + 12, 10U, 0);
	x11_write16(img16_req + 14, 20U, 0);
	img16_req[16] = 0x00U;
	img16_req[17] = 'A';
	img16_req[18] = 0x00U;
	img16_req[19] = 'B';
	test_draw_calls = 0;
	code = x11_font_request(&server, 0U, img16_req, sizeof(img16_req));
	test_check("xserver ImageText16 succeeds", code == 0U);
	test_check("xserver ImageText16 fills background (image == 1)",
		   test_draw_calls == 1 && test_last_image == 1);

	/* PolyText16 (opcode 75) must draw transparently (text.image == 0). */
	memset(poly16_req, 0, sizeof(poly16_req));
	poly16_req[0] = 75U;
	x11_write16(poly16_req + 2, 6U, 0);
	x11_write32(poly16_req + 4, 0x00200003U, 0);
	x11_write32(poly16_req + 8, test_gc.id, 0);
	x11_write16(poly16_req + 12, 10U, 0);
	x11_write16(poly16_req + 14, 20U, 0);
	poly16_req[16] = 2U;
	poly16_req[17] = 0U;
	poly16_req[18] = 0x00U;
	poly16_req[19] = 'A';
	poly16_req[20] = 0x00U;
	poly16_req[21] = 'B';
	test_draw_calls = 0;
	code = x11_font_request(&server, 0U, poly16_req, sizeof(poly16_req));
	test_check("xserver PolyText16 succeeds", code == 0U);
	test_check("xserver PolyText16 is transparent (image == 0)",
		   test_draw_calls == 1 && test_last_image == 0);

	x11_font_free(&server, test_font);
}

/* Stubs for xserver/font.c dependencies. */

uint16_t
x11_read16(
	const uint8_t *bytes,
	int msb)
{
	if (msb)
		return ((uint16_t)bytes[0] << 8) | (uint16_t)bytes[1];
	return ((uint16_t)bytes[1] << 8) | (uint16_t)bytes[0];
}

uint32_t
x11_read32(
	const uint8_t *bytes,
	int msb)
{
	if (msb) {
		return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
		       ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
	}
	return ((uint32_t)bytes[3] << 24) | ((uint32_t)bytes[2] << 16) |
	       ((uint32_t)bytes[1] << 8) | (uint32_t)bytes[0];
}

void
x11_write16(
	uint8_t *bytes,
	uint16_t value,
	int msb)
{
	if (msb) {
		bytes[0] = (uint8_t)(value >> 8);
		bytes[1] = (uint8_t)value;
	} else {
		bytes[0] = (uint8_t)value;
		bytes[1] = (uint8_t)(value >> 8);
	}
}

void
x11_write32(
	uint8_t *bytes,
	uint32_t value,
	int msb)
{
	if (msb) {
		bytes[0] = (uint8_t)(value >> 24);
		bytes[1] = (uint8_t)(value >> 16);
		bytes[2] = (uint8_t)(value >> 8);
		bytes[3] = (uint8_t)value;
	} else {
		bytes[0] = (uint8_t)value;
		bytes[1] = (uint8_t)(value >> 8);
		bytes[2] = (uint8_t)(value >> 16);
		bytes[3] = (uint8_t)(value >> 24);
	}
}

void
x11_reply(
	struct x11server *server,
	struct x11_client *client,
	uint8_t *reply,
	size_t length)
{
	(void)server;
	(void)client;
	(void)reply;
	(void)length;
}

int
x11_id_chosen(
	const struct x11_client *client,
	uint32_t id)
{
	(void)client;
	return id != 0U;
}

int
x11_resource_add(
	struct x11server *server,
	uint32_t id,
	unsigned type,
	void *object)
{
	(void)server;
	(void)id;
	if (type == X11_RESOURCE_FONT)
		test_font = (struct x11_font *)object;
	return 0;
}

void *
x11_resource_find(
	struct x11server *server,
	uint32_t id,
	unsigned type)
{
	(void)server;
	if (type == X11_RESOURCE_FONT && test_font != NULL && test_font->id == id)
		return test_font;
	return NULL;
}

unsigned
x11_resource_type(
	struct x11server *server,
	uint32_t id)
{
	(void)server;
	if (test_font != NULL && test_font->id == id)
		return X11_RESOURCE_FONT;
	return 0U;
}

void
x11_resource_remove(
	struct x11server *server,
	uint32_t id)
{
	(void)server;
	if (test_font != NULL && test_font->id == id)
		test_font = NULL;
}

int
x11_memory_charge(
	struct x11server *server,
	unsigned owner,
	size_t bytes)
{
	(void)server;
	(void)owner;
	(void)bytes;
	return 0;
}

void
x11_memory_release(
	struct x11server *server,
	unsigned owner,
	size_t bytes)
{
	(void)server;
	(void)owner;
	(void)bytes;
}

struct x11_gc *
x11_gc_find(
	struct x11server *server,
	uint32_t id)
{
	(void)server;
	if (id == test_gc.id)
		return &test_gc;
	return NULL;
}

uint32_t
x11_atom_intern(
	struct x11server *server,
	const char *name,
	size_t length,
	int only_if_exists)
{
	(void)server;
	(void)name;
	(void)length;
	(void)only_if_exists;
	return 1U;
}

int
x11_face_present(
	const struct x11_faces *faces,
	unsigned role)
{
	(void)faces;
	(void)role;
	return 1;
}

int
x11_face_has_own(
	const struct x11_faces *faces,
	unsigned role)
{
	(void)faces;
	(void)role;
	return 0;
}

int
x11_face_char_advance(
	struct x11_faces *faces,
	unsigned role,
	unsigned pixels,
	uint32_t codepoint,
	int *advance)
{
	(void)faces;
	(void)role;
	(void)pixels;
	(void)codepoint;
	*advance = 8;
	return 0;
}

int
x11_face_line(
	struct x11_faces *faces,
	unsigned role,
	unsigned pixels,
	int *ascent,
	int *descent,
	int *advance)
{
	(void)faces;
	(void)role;
	(void)pixels;
	*ascent = 12;
	*descent = 4;
	*advance = 8;
	return 0;
}

int
x11_face_char(
	struct x11_faces *faces,
	unsigned role,
	unsigned pixels,
	uint32_t codepoint,
	struct x11_char_metrics *metrics)
{
	(void)faces;
	(void)role;
	(void)pixels;
	(void)codepoint;
	memset(metrics, 0, sizeof(*metrics));
	metrics->width = 8;
	metrics->right = 8;
	metrics->ascent = 12;
	metrics->descent = 4;
	return 0;
}

int
x11_face_draw(
	struct x11_faces *faces,
	unsigned role,
	unsigned pixels,
	uint32_t codepoint,
	struct x11_face_glyph *glyph)
{
	static const uint8_t dummy_coverage[128] = { 0 };

	(void)faces;
	(void)role;
	(void)pixels;
	(void)codepoint;
	memset(glyph, 0, sizeof(*glyph));
	glyph->width = 8;
	glyph->height = 16;
	glyph->stride = 8U;
	glyph->top = 12;
	glyph->advance = 8;
	glyph->coverage = dummy_coverage;
	return 0;
}

unsigned
x11_draw_text(
	struct x11server *server,
	const struct x11_text *text)
{
	size_t i;

	(void)server;
	if (text->count == 0U && !text->image)
		return 0U;
	test_draw_calls++;
	test_last_image = text->image;
	test_last_count = text->count;
	test_last_bg = text->background;
	for (i = 0U; i < text->count && i < 16U; i++)
		test_last_glyph_x[i] = text->glyphs[i].x;
	return 0U;
}
