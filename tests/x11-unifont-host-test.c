/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit test for GNU Unifont core font support in xserver (font.c,
 * glyphs.c, and libtruetype): XLFD naming/aliases, ListFonts,
 * ListFontsWithInfo, QueryFont, 16px/32px integer scaling, 8px/16px
 * half/full-width metrics and glyph bitmaps, cursor separation, and
 * fallback when no fixed font is supplied.
 *
 * Build and run from the top of the tree:
 *   cc -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
 *      -fsanitize=address,undefined -fno-sanitize-recover=all \
 *      -I. -Iuserland/desktop/include \
 *      tests/x11-unifont-host-test.c \
 *      userland/desktop/xserver/font.c \
 *      userland/desktop/xserver/glyphs.c \
 *      userland/desktop/libtruetype/<all>.c \
 *      -lm -o build/tmp/x11-unifont-host-test
 *   build/tmp/x11-unifont-host-test
 */

#include "userland/desktop/xserver/internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_UNIFONT_PATH	"build/distfiles/unifont_jp-15.0.06.ttf"
#define TEST_MONO_PATH		"userland/desktop/fonts/Mahora-Mono.ttf"

static unsigned test_checks;
static unsigned test_failures;

static uint8_t *test_replies;
static size_t test_replies_size;
static size_t test_replies_cap;

struct test_res_slot {
	uint32_t id;
	unsigned kind;
	void *value;
};

static struct test_res_slot test_res_table[32];

static void test_check(const char *what, int passed);
static int read_file_bytes(const char *path, uint8_t **out, size_t *out_size);
static void reset_replies(void);
static uint16_t get16(const uint8_t *p);
static uint32_t get32(const uint8_t *p);
static void put16(uint8_t *p, uint16_t v);
static void put32(uint8_t *p, uint32_t v);
static unsigned send_open_font(struct x11server *s, uint32_t id, const char *name);
static unsigned send_query_font(struct x11server *s, uint32_t id);
static unsigned send_list_fonts(struct x11server *s, uint8_t opcode, uint16_t max_names, const char *pattern);
static int list_has_name(const uint8_t *reply, size_t reply_len, const char *target);
static int glyph_has_ink_in_cols(const struct x11_font_glyph *g, int c0, int c1);
static void test_with_unifont(void);
static void test_fallback_without_fixed(void);

int
main(
	void)
{
	test_with_unifont();
	test_fallback_without_fixed();
	free(test_replies);

	if (test_failures != 0U) {
		printf("x11-unifont-host-test: FAIL (%u of %u checks)\n",
		       test_failures, test_checks);
		return 1;
	}

	printf("x11-unifont-host-test: PASS (%u checks)\n", test_checks);
	return 0;
}

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

static int
read_file_bytes(
	const char *path,
	uint8_t **out,
	size_t *out_size)
{
	FILE *f;
	long len;
	uint8_t *buf;

	f = fopen(path, "rb");
	if (f == NULL)
		return errno;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return EIO;
	}
	len = ftell(f);
	if (len <= 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return EIO;
	}
	buf = malloc((size_t)len);
	if (buf == NULL) {
		fclose(f);
		return ENOMEM;
	}
	if (fread(buf, 1U, (size_t)len, f) != (size_t)len) {
		free(buf);
		fclose(f);
		return EIO;
	}
	fclose(f);
	*out = buf;
	*out_size = (size_t)len;
	return 0;
}

static void
reset_replies(
	void)
{
	test_replies_size = 0U;
}

static uint16_t
get16(
	const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t
get32(
	const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
put16(
	uint8_t *p,
	uint16_t v)
{
	p[0] = (uint8_t)(v & 0xffU);
	p[1] = (uint8_t)((v >> 8) & 0xffU);
}

static void
put32(
	uint8_t *p,
	uint32_t v)
{
	p[0] = (uint8_t)(v & 0xffU);
	p[1] = (uint8_t)((v >> 8) & 0xffU);
	p[2] = (uint8_t)((v >> 16) & 0xffU);
	p[3] = (uint8_t)((v >> 24) & 0xffU);
}

static unsigned
send_open_font(
	struct x11server *s,
	uint32_t id,
	const char *name)
{
	uint8_t req[300];
	size_t nlen;
	size_t padded;

	nlen = strlen(name);
	padded = (nlen + 3U) & ~(size_t)3U;
	memset(req, 0, 12U + padded);
	req[0] = 45U;
	put16(req + 2, (uint16_t)(3U + padded / 4U));
	put32(req + 4, id);
	put16(req + 8, (uint16_t)nlen);
	memcpy(req + 12, name, nlen);
	return x11_font_request(s, 0U, req, 12U + padded);
}

static unsigned
send_query_font(
	struct x11server *s,
	uint32_t id)
{
	uint8_t req[8];

	reset_replies();
	memset(req, 0, sizeof(req));
	req[0] = 47U;
	put16(req + 2, 2U);
	put32(req + 4, id);
	return x11_font_request(s, 0U, req, sizeof(req));
}

static unsigned
send_list_fonts(
	struct x11server *s,
	uint8_t opcode,
	uint16_t max_names,
	const char *pattern)
{
	uint8_t req[300];
	size_t plen;
	size_t padded;

	reset_replies();
	plen = strlen(pattern);
	padded = (plen + 3U) & ~(size_t)3U;
	memset(req, 0, 8U + padded);
	req[0] = opcode;
	put16(req + 2, (uint16_t)(2U + padded / 4U));
	put16(req + 4, max_names);
	put16(req + 6, (uint16_t)plen);
	memcpy(req + 8, pattern, plen);
	return x11_font_request(s, 0U, req, 8U + padded);
}

static int
list_has_name(
	const uint8_t *reply,
	size_t reply_len,
	const char *target)
{
	uint16_t count;
	uint16_t i;
	size_t off;
	size_t tlen;

	if (reply_len < 32U)
		return 0;
	count = get16(reply + 8);
	off = 32U;
	tlen = strlen(target);
	for (i = 0U; i < count && off < reply_len; i++) {
		uint8_t nlen = reply[off];
		if (off + 1U + nlen > reply_len)
			break;
		if (nlen == tlen && memcmp(reply + off + 1U, target, tlen) == 0)
			return 1;
		off += 1U + nlen;
	}
	return 0;
}

static int
glyph_has_ink_in_cols(
	const struct x11_font_glyph *g,
	int c0,
	int c1)
{
	int r;
	int c;

	if (g == NULL || g->bits == NULL)
		return 0;
	for (r = 0; r < g->height; r++) {
		for (c = c0; c < c1 && c < g->width; c++) {
			uint8_t byte = g->bits[(size_t)r * g->stride + (size_t)c / 8U];
			if ((byte & (uint8_t)(0x80U >> (c % 8))) != 0U)
				return 1;
		}
	}
	return 0;
}

static void
test_with_unifont(
	void)
{
	struct x11server *s;
	struct x11server_font_file files[2];
	uint8_t *unifont_bytes;
	uint8_t *mono_bytes;
	size_t unifont_size;
	size_t mono_size;
	struct x11_font *f_fixed;
	struct x11_font *f_16x16;
	struct x11_font *f_zed;
	struct x11_font *f_32;
	struct x11_font *f_cursor;
	const struct x11_font_glyph *g;
	uint32_t n_infos;
	size_t info_off;
	int err;

	unifont_bytes = NULL;
	mono_bytes = NULL;
	err = read_file_bytes(TEST_UNIFONT_PATH, &unifont_bytes, &unifont_size);
	test_check("read unifont_jp-15.0.06.ttf", err == 0);
	err = read_file_bytes(TEST_MONO_PATH, &mono_bytes, &mono_size);
	test_check("read Mahora-Mono.ttf", err == 0);
	if (unifont_bytes == NULL || mono_bytes == NULL) {
		free(unifont_bytes);
		free(mono_bytes);
		return;
	}

	s = calloc(1U, sizeof(*s));
	test_check("alloc server", s != NULL);
	s->clients = calloc(1U, sizeof(*s->clients));
	s->clients[0].order = 0;
	s->client_memory_max = 256U * 1024U * 1024U;
	s->server_memory_max = 512U * 1024U * 1024U;

	files[0].bytes = mono_bytes;
	files[0].size = mono_size;
	files[0].role = X11SERVER_FONT_MONO;
	files[1].bytes = unifont_bytes;
	files[1].size = unifont_size;
	files[1].role = X11SERVER_FONT_FIXED;
	err = x11_faces_open(&s->faces, files, 2U, NULL);
	test_check("x11_faces_open with MONO + FIXED", err == 0);
	test_check("x11_face_has_own(FIXED) is 1", x11_face_has_own(&s->faces, X11_FACE_FIXED) == 1);

	test_check("ListFonts(*) succeeds", send_list_fonts(s, 49U, 500U, "*") == 0U);
	test_check("ListFonts(*) includes fixed", list_has_name(test_replies, test_replies_size, "fixed"));
	test_check("ListFonts(*) includes 8x16", list_has_name(test_replies, test_replies_size, "8x16"));
	test_check("ListFonts(*) includes 16x16", list_has_name(test_replies, test_replies_size, "16x16"));
	test_check("ListFonts(*) includes unifont", list_has_name(test_replies, test_replies_size, "unifont"));
	test_check("ListFonts(*) includes zed-unicode", list_has_name(test_replies, test_replies_size, "zed-unicode"));
	test_check("ListFonts(*) includes cursor", list_has_name(test_replies, test_replies_size, "cursor"));
	test_check("ListFonts(*) includes gnu-unifont 32px",
		   list_has_name(test_replies, test_replies_size, "-gnu-unifont-medium-r-normal--32-320-75-75-c-160-iso10646-1"));

	test_check("ListFontsWithInfo(fixed) succeeds", send_list_fonts(s, 50U, 10U, "fixed") == 0U);
	test_check("ListFontsWithInfo(fixed) has font + terminator", test_replies_size > 60U);
	test_check("ListFontsWithInfo(fixed) ascent is 14", (int16_t)get16(test_replies + 52) == 14);
	test_check("ListFontsWithInfo(fixed) descent is 2", (int16_t)get16(test_replies + 54) == 2);

	test_check("OpenFont(fixed) succeeds", send_open_font(s, 0x101U, "fixed") == 0U);
	test_check("OpenFont(16x16) succeeds", send_open_font(s, 0x102U, "16x16") == 0U);
	test_check("OpenFont(zed-unicode) succeeds", send_open_font(s, 0x103U, "zed-unicode") == 0U);
	test_check("OpenFont(unifont 32px) succeeds",
		   send_open_font(s, 0x104U, "-gnu-unifont-medium-r-normal--32-320-75-75-c-160-iso10646-1") == 0U);
	test_check("OpenFont(cursor) succeeds", send_open_font(s, 0x105U, "cursor") == 0U);

	f_fixed = x11_font_find(s, 0x101U);
	f_16x16 = x11_font_find(s, 0x102U);
	f_zed = x11_font_find(s, 0x103U);
	f_32 = x11_font_find(s, 0x104U);
	f_cursor = x11_font_find(s, 0x105U);
	test_check("all 5 fonts found", f_fixed != NULL && f_16x16 != NULL && f_zed != NULL && f_32 != NULL && f_cursor != NULL);

	test_check("QueryFont(fixed) succeeds", send_query_font(s, 0x101U) == 0U);
	n_infos = get32(test_replies + 56);
	test_check("QueryFont(fixed) has 65536 char_infos", n_infos == 65536U);
	test_check("QueryFont(fixed) min_bounds.width == 8", (int16_t)get16(test_replies + 12) == 8);
	test_check("QueryFont(fixed) max_bounds.width == 16", (int16_t)get16(test_replies + 28) == 16);
	info_off = test_replies_size - (size_t)n_infos * 12U;
	test_check("QueryFont(fixed) 'A' width == 8", (int16_t)get16(test_replies + info_off + (size_t)'A' * 12U + 4U) == 8);
	test_check("QueryFont(fixed) U+3042 width == 16", (int16_t)get16(test_replies + info_off + (size_t)0x3042U * 12U + 4U) == 16);
	test_check("QueryFont(fixed) U+9F8D width == 16", (int16_t)get16(test_replies + info_off + (size_t)0x9F8DU * 12U + 4U) == 16);

	test_check("QueryFont(16x16) succeeds", send_query_font(s, 0x102U) == 0U);
	test_check("QueryFont(16x16) char_infos == 0", get32(test_replies + 56) == 0U);
	test_check("QueryFont(16x16) min_bounds.width == 16", (int16_t)get16(test_replies + 12) == 16);

	test_check("zed-unicode ascent=14 descent=2 pixels=16",
		   f_zed->ascent == 14 && f_zed->descent == 2 && f_zed->pixels == 16U);
	test_check("unifont 32px kept pixels=32 ascent=28 descent=4",
		   f_32->pixels == 32U && f_32->ascent == 28 && f_32->descent == 4);
	test_check("cursor font isolated",
		   f_cursor->kind == X11_FONT_CURSOR && x11_font_glyph(s, f_cursor, 'A') == NULL);

	g = x11_font_glyph(s, f_fixed, 'A');
	test_check("fixed 'A' glyph 8x16 with ink", g != NULL && g->width == 8 && g->height == 16 && glyph_has_ink_in_cols(g, 0, 8));
	g = x11_font_glyph(s, f_fixed, 0x9F8DU);
	test_check("fixed U+9F8D glyph 16x16 with ink in left and right halves",
		   g != NULL && g->width == 16 && g->height == 16 &&
		   glyph_has_ink_in_cols(g, 0, 8) && glyph_has_ink_in_cols(g, 8, 16));

	g = x11_font_glyph(s, f_32, 0x9F8DU);
	test_check("unifont 32px U+9F8D glyph 32x32 with ink in right half",
		   g != NULL && g->width == 32 && g->height == 32 &&
		   glyph_has_ink_in_cols(g, 16, 32));

	x11_fonts_fini(s);
	x11_faces_close(&s->faces);
	free(s->font_names);
	free(s->clients);
	free(s);
	free(unifont_bytes);
	free(mono_bytes);
}

static void
test_fallback_without_fixed(
	void)
{
	struct x11server *s;
	struct x11server_font_file file;
	uint8_t *mono_bytes;
	size_t mono_size;
	int err;

	mono_bytes = NULL;
	err = read_file_bytes(TEST_MONO_PATH, &mono_bytes, &mono_size);
	test_check("fallback: read Mahora-Mono.ttf", err == 0);
	if (mono_bytes == NULL)
		return;

	s = calloc(1U, sizeof(*s));
	s->clients = calloc(1U, sizeof(*s->clients));
	s->client_memory_max = 256U * 1024U * 1024U;
	s->server_memory_max = 512U * 1024U * 1024U;

	file.bytes = mono_bytes;
	file.size = mono_size;
	file.role = X11SERVER_FONT_MONO;
	err = x11_faces_open(&s->faces, &file, 1U, NULL);
	test_check("fallback: x11_faces_open(MONO only) succeeds", err == 0);
	test_check("fallback: x11_face_has_own(FIXED) is 0", x11_face_has_own(&s->faces, X11_FACE_FIXED) == 0);
	test_check("fallback: x11_face_present(FIXED) is 1", x11_face_present(&s->faces, X11_FACE_FIXED) == 1);
	test_check("fallback: OpenFont(fixed) succeeds", send_open_font(s, 0x201U, "fixed") == 0U);
	test_check("fallback: OpenFont(zed-unicode) succeeds", send_open_font(s, 0x202U, "zed-unicode") == 0U);

	x11_fonts_fini(s);
	x11_faces_close(&s->faces);
	free(s->font_names);
	free(s->clients);
	free(s);
	free(mono_bytes);
}

void *
x11_resource_find(
	struct x11server *server,
	uint32_t id,
	unsigned type)
{
	size_t i;

	(void)server;
	for (i = 0U; i < 32U; i++) {
		if (test_res_table[i].id == id && test_res_table[i].kind == type)
			return test_res_table[i].value;
	}
	return NULL;
}

unsigned
x11_resource_type(
	struct x11server *server,
	uint32_t id)
{
	size_t i;

	(void)server;
	for (i = 0U; i < 32U; i++) {
		if (test_res_table[i].id == id)
			return test_res_table[i].kind;
	}
	return 0U;
}

int
x11_id_chosen(
	const struct x11_client *client,
	uint32_t id)
{
	(void)client;
	(void)id;
	return 1;
}

struct x11_gc *
x11_gc_find(
	struct x11server *server,
	uint32_t id)
{
	(void)server;
	(void)id;
	return NULL;
}

int
x11_resource_add(
	struct x11server *server,
	uint32_t id,
	unsigned kind,
	void *value)
{
	size_t i;

	(void)server;
	for (i = 0U; i < 32U; i++) {
		if (test_res_table[i].id == 0U) {
			test_res_table[i].id = id;
			test_res_table[i].kind = kind;
			test_res_table[i].value = value;
			return 0;
		}
	}
	return ENOMEM;
}

void
x11_resource_remove(
	struct x11server *server,
	uint32_t id)
{
	size_t i;

	(void)server;
	for (i = 0U; i < 32U; i++) {
		if (test_res_table[i].id == id) {
			test_res_table[i].id = 0U;
			test_res_table[i].kind = 0U;
			test_res_table[i].value = NULL;
			return;
		}
	}
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
x11_memory_charge(
	struct x11server *server,
	unsigned client,
	size_t bytes)
{
	(void)server;
	(void)client;
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

void
x11_reply(
	struct x11server *server,
	struct x11_client *client,
	uint8_t *bytes,
	size_t length)
{
	(void)server;
	(void)client;
	if (test_replies_size + length > test_replies_cap) {
		size_t want = test_replies_cap ? test_replies_cap * 2U : 4096U;
		while (want < test_replies_size + length)
			want *= 2U;
		test_replies = realloc(test_replies, want);
		test_replies_cap = want;
	}
	memcpy(test_replies + test_replies_size, bytes, length);
	test_replies_size += length;
}

uint16_t
x11_read16(
	const uint8_t *bytes,
	int msb)
{
	(void)msb;
	return get16(bytes);
}

uint32_t
x11_read32(
	const uint8_t *bytes,
	int msb)
{
	(void)msb;
	return get32(bytes);
}

void
x11_write16(
	uint8_t *bytes,
	uint16_t value,
	int msb)
{
	(void)msb;
	put16(bytes, value);
}

void
x11_write32(
	uint8_t *bytes,
	uint32_t value,
	int msb)
{
	(void)msb;
	put32(bytes, value);
}

unsigned
x11_draw_text(
	struct x11server *server,
	const struct x11_text *text)
{
	(void)server;
	(void)text;
	return 0U;
}
