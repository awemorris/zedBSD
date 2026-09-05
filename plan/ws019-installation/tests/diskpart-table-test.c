/* Sparse memory-backed production parser/writer fault matrix.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/diskpart/table.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW 65536U
struct image {
	uint8_t first[WINDOW], last[WINDOW];
	uint64_t sectors;
	uint32_t sector;
	unsigned calls, fault, writes, flushes;
	uint64_t write_at[8];
};
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); \
} } while (0)
static void put32(uint8_t *p, uint32_t v)
{ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put64(uint8_t *p, uint64_t v)
{ put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint8_t *address(struct image *m, uint64_t off, size_t n)
{
	uint64_t end = m->sectors * m->sector;
	CHECK(off <= end && n <= end - off);
	if (off < WINDOW) { CHECK(n <= WINDOW - off); return m->first + off; }
	CHECK(off >= end - WINDOW); return m->last + off - (end - WINDOW);
}
static int read_image(void *ctx, uint64_t off, void *p, size_t n)
{
	struct image *m = ctx;
	if (++m->calls == m->fault) return EIO;
	memcpy(p, address(m, off, n), n); return 0;
}
static int write_image(void *ctx, uint64_t off, const void *p, size_t n)
{
	struct image *m = ctx;
	CHECK(m->writes < 8); m->write_at[m->writes++] = off;
	if (++m->calls == m->fault) {
		/* Model a short write: some bytes reached disk before EIO. */
		memcpy(address(m, off, n), p, n / 2); return EIO;
	}
	memcpy(address(m, off, n), p, n); return 0;
}
static int flush_image(void *ctx)
{
	struct image *m = ctx;
	m->flushes++;
	return ++m->calls == m->fault ? EIO : 0;
}
static struct dp_io image_io(struct image *m)
{
	return (struct dp_io){ m, m->sectors, m->sector, read_image, write_image, flush_image };
}
static const char *esp = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b";
static const char *uuid1 = "12345678-1234-4567-890a-bcdef0123456";
static const char *uuid2 = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";

static void header_crc(uint8_t *p)
{ put32(p + 16, 0); put32(p + 16, dp_crc32(p, 92)); }

static void make_image(struct image *m, unsigned sector, int gpt)
{
	uint8_t *p;
	memset(m, 0, sizeof(*m));
	m->sector = sector; m->sectors = 0x200000000ULL / sector + 128;
	m->first[510] = 0x55; m->first[511] = 0xaa;
	memset(m->first, 0x71, 440); /* Boot-code preservation sentinel. */
	put32(m->first + 440, 0x12345678);
	if (!gpt) {
		p = m->first + 446; p[4] = 0x83;
		put32(p + 8, 2048); put32(p + 12, 100);
		return;
	}
	p = m->first + 446; p[4] = 0xee;
	put32(p + 8, 1); put32(p + 12, (uint32_t)(m->sectors - 1));
	for (unsigned copy = 0; copy < 2; copy++) {
		uint64_t lba = copy ? m->sectors - 1 : 1;
		uint64_t table_lba = copy ? lba - 16384 / sector : 2;
		uint8_t *entries = address(m, table_lba * sector, 16384);
		p = entries + 3 * 128; /* Sparse slot 4. */
		CHECK(dp_guid_parse(esp, p) == 0); CHECK(dp_guid_parse(uuid1, p + 16) == 0);
		put64(p + 32, 2048); put64(p + 40, 2147); p[56] = 'E';
		p = address(m, lba * sector, sector);
		memcpy(p, "EFI PART", 8); put32(p + 8, 0x10000); put32(p + 12, 92);
		put64(p + 24, lba); put64(p + 32, copy ? 1 : m->sectors - 1);
		put64(p + 40, 2 + 16384 / sector);
		put64(p + 48, m->sectors - 2 - 16384 / sector);
		CHECK(dp_guid_parse(uuid2, p + 56) == 0);
		put64(p + 72, table_lba); put32(p + 80, 128); put32(p + 84, 128);
		put32(p + 88, dp_crc32(entries, 16384)); header_crc(p);
	}
}

static void test_format(unsigned sector, int gpt)
{
	struct image *m = malloc(sizeof(*m)), *original = malloc(sizeof(*m));
	struct dp_table t, verify;
	struct dp_io io;
	int started;
	unsigned operation_count;
	CHECK(m && original);
	make_image(m, sector, gpt); *original = *m; io = image_io(m);
	CHECK(dp_load(&t, &io) == 0);
	CHECK(t.count == 1 && t.parts[0].slot == (gpt ? 4U : 1U));
	CHECK(t.parts[0].start == 2048 && t.parts[0].count == 100);
	CHECK(t.restrictions == 0);
	CHECK(dp_add(&t, 2, 2100, 100, gpt ? esp : "83", gpt ? uuid2 : NULL, NULL) == EINVAL);
	CHECK(dp_add(&t, 2, 3000, UINT64_MAX, gpt ? esp : "83", gpt ? uuid2 : NULL, NULL) == EINVAL);
	if (gpt) {
		CHECK(!strcmp(t.parts[0].type, esp) && !strcmp(t.parts[0].uuid, uuid1));
		CHECK(dp_add(&t, 2, 3000, 100, esp, uuid1, NULL) == EINVAL);
		CHECK(dp_add(&t, 2, 3000, 100, esp, uuid2, "\xf0\x80\x80\x80") == EINVAL);
		CHECK(dp_add(&t, 2, 3000, 100, esp, uuid2, "\xf0") == EINVAL);
	}
	CHECK(dp_add(&t, 2, 3000, 100, gpt ? esp : "83", gpt ? uuid2 : NULL,
	    gpt ? "日本語\xf0\x9f\x8c\x99" : NULL) == 0);
	CHECK(dp_add(&t, 2, 3300, 100, gpt ? esp : "83", gpt ? uuid2 : NULL, NULL) == EEXIST);
	m->calls = m->writes = m->flushes = 0;
	CHECK(dp_write(&t, &started) == 0 && started == 1);
	operation_count = m->calls;
	CHECK(m->writes == (gpt ? 4U : 1U) && m->flushes == (gpt ? 2U : 1U));
	if (gpt) {
		CHECK(m->write_at[0] == (m->sectors - 1 - 16384 / sector) * sector);
		CHECK(m->write_at[1] == (m->sectors - 1) * sector);
		CHECK(m->write_at[2] == 2U * sector && m->write_at[3] == sector);
		CHECK(!memcmp(m->first, original->first, sector));
	}
	CHECK(!memcmp(m->first, original->first, 446));
	CHECK(dp_load(&verify, &io) == 0 && verify.count == 2);
	if (gpt) CHECK(!strcmp(verify.parts[0].name, "日本語\xf0\x9f\x8c\x99"));
	CHECK(dp_delete(&verify, 2) == 0);
	CHECK(dp_delete(&verify, 2) == ENOENT);
	m->writes = 0;
	CHECK(dp_write(&verify, &started) == 0);
	CHECK(!memcmp(m->first, original->first, WINDOW));
	CHECK(!memcmp(m->last, original->last, WINDOW));
	dp_free(&verify);
	for (unsigned fault = 1; fault <= operation_count; fault++) {
		*m = *original; m->calls = m->writes = m->flushes = 0; m->fault = fault;
		CHECK(dp_write(&t, &started) == EIO);
		if (!started) {
			CHECK(!m->writes);
			CHECK(!memcmp(m->first, original->first, WINDOW));
			CHECK(!memcmp(m->last, original->last, WINDOW));
		}
	}
	*m = *original; m->first[50] ^= 1;
	CHECK(dp_write(&t, &started) == EBUSY && !started && !m->writes);
	dp_free(&t);
	/* Every reader call has an explicit short/error path. */
	*m = *original; CHECK(dp_load(&t, &io) == 0); operation_count = m->calls; dp_free(&t);
	for (unsigned fault = 1; fault <= operation_count; fault++) {
		*m = *original; m->fault = fault;
		int error = dp_load(&t, &io);
		if (!error) { CHECK(t.restrictions & DP_DEGRADED); dp_free(&t); }
		CHECK(!m->writes);
	}
	*m = *original;
	if (gpt) {
		m->first[sector + 16] ^= 1;
		CHECK(dp_load(&t, &io) == 0 && (t.restrictions & DP_DEGRADED));
		CHECK(dp_delete(&t, 4) == EOPNOTSUPP); dp_free(&t);
		m->last[WINDOW - sector + 16] ^= 1;
		CHECK(dp_load(&t, &io) != 0);
		*m = *original; memset(m->first + 446, 0, 16);
		CHECK(dp_load(&t, &io) == EINVAL); /* Missing protective MBR. */
	} else {
		m->first[446 + 4] = 5;
		CHECK(dp_load(&t, &io) == 0 && (t.restrictions & DP_UNSUPPORTED));
		CHECK(dp_delete(&t, 1) == EOPNOTSUPP); dp_free(&t);
	}
	free(m); free(original);
}

static void test_corrupt(unsigned sector)
{
	struct image *m = malloc(sizeof(*m));
	struct dp_table t;
	struct dp_io io;
	CHECK(m != NULL);
	for (unsigned fault = 0; fault < 12; fault++) {
		make_image(m, sector, 1); io = image_io(m);
		for (unsigned copy = 0; copy < 2; copy++) {
			uint64_t lba = copy ? m->sectors - 1 : 1;
			uint64_t table = copy ? lba - 16384 / sector : 2;
			uint8_t *h = address(m, lba * sector, sector);
			uint8_t *e = address(m, table * sector, 16384);
			switch (fault) {
			case 0: put64(h + 24, 0); break; /* wrong self LBA */
			case 1: put64(h + 32, lba); break; /* self-referential */
			case 2: put64(h + 40, 1); break; /* no metadata reservation */
			case 3: put64(h + 48, UINT64_MAX); break;
			case 4: put32(h + 80, UINT32_MAX); break;
			case 5: put32(h + 84, 129); break;
			case 6: put64(h + 72, UINT64_MAX); break;
			case 7: memset(h + 56, 0, 16); break;
			case 8: /* matching copies with overlapping entries */
				memcpy(e, e + 3 * 128, 128);
				CHECK(dp_guid_parse(uuid2, e + 16) == 0); break;
			case 9: /* matching copies with duplicate GUID */
				memcpy(e, e + 3 * 128, 128);
				put64(e + 32, 3000); put64(e + 40, 3099); break;
			case 10: put64(e + 3 * 128 + 40, UINT64_MAX); break;
			case 11: /* Individually CRC-valid contradictory copies. */
				if (copy) h[56] ^= 1;
				break;
			}
			put32(h + 88, dp_crc32(e, 16384)); header_crc(h);
		}
		CHECK(dp_load(&t, &io) != 0 && !m->writes);
	}
	free(m);
}

int main(void)
{
	uint8_t guid[16]; char text[37];
	CHECK(dp_crc32("123456789", 9) == 0xcbf43926U);
	CHECK(dp_guid_parse(esp, guid) == 0); dp_guid_text(guid, text); CHECK(!strcmp(esp, text));
	CHECK(dp_guid_parse("00000000-0000-0000-0000-000000000000", guid) == EINVAL);
	test_format(512, 0); test_format(512, 1); test_format(4096, 1);
	test_corrupt(512); test_corrupt(4096);
	printf("userspace partition parser/writer: %u checks PASS\n", checks);
	return 0;
}
