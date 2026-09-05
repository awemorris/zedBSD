/* Userspace GPT/MBR parser and metadata editor. No kernel-private interfaces.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "table.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const uint8_t *p)
{ return get32(p) | (uint64_t)get32(p + 4) << 32; }
static void put32(uint8_t *p, uint32_t v)
{ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put64(uint8_t *p, uint64_t v)
{ put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static int zero(const uint8_t *p, size_t n)
{ while (n--) if (*p++) return 0; return 1; }

uint32_t dp_crc32(const void *data, size_t size)
{
	const uint8_t *p = data;
	uint32_t crc = UINT32_MAX;
	while (size--) {
		crc ^= *p++;
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1));
	}
	return ~crc;
}

static const unsigned guid_order[16] = {3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
static int unhex(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}
int dp_guid_parse(const char *s, uint8_t output[16])
{
	unsigned at = 0;
	if (!s || strlen(s) != 36) return EINVAL;
	for (unsigned i = 0; i < 16; i++) {
		int a, b;
		if (i == 4 || i == 6 || i == 8 || i == 10)
			if (s[at++] != '-') return EINVAL;
		a = unhex(s[at++]); b = unhex(s[at++]);
		if (a < 0 || b < 0) return EINVAL;
		output[guid_order[i]] = (uint8_t)(a * 16 + b);
	}
	return zero(output, 16) ? EINVAL : 0;
}
void dp_guid_text(const uint8_t input[16], char output[37])
{
	const char *hex = "0123456789abcdef";
	unsigned at = 0;
	for (unsigned i = 0; i < 16; i++) {
		unsigned v = input[guid_order[i]];
		if (i == 4 || i == 6 || i == 8 || i == 10) output[at++] = '-';
		output[at++] = hex[v >> 4]; output[at++] = hex[v & 15];
	}
	output[at] = 0;
}

static int name_decode(const uint8_t *raw, char *out)
{
	unsigned at = 0;
	for (unsigned i = 0; i < 36; i++) {
		uint32_t c = raw[i * 2] | (uint32_t)raw[i * 2 + 1] << 8;
		if (!c) break;
		if (c >= 0xd800 && c <= 0xdbff) {
			uint32_t low;
			if (++i == 36) return EINVAL;
			low = raw[i * 2] | (uint32_t)raw[i * 2 + 1] << 8;
			if (low < 0xdc00 || low > 0xdfff) return EINVAL;
			c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
		} else if (c >= 0xdc00 && c <= 0xdfff) return EINVAL;
		/* Never print terminal control sequences from untrusted disk names. */
		if (c < 32 || (c >= 127 && c <= 159)) c = '?';
		if (c < 0x80) out[at++] = (char)c;
		else {
			if (c >= 0x10000) out[at++] = (char)(0xf0 | (c >> 18));
			if (c >= 0x800) out[at++] = (char)((c < 0x10000 ? 0xe0 : 0x80) | ((c >> 12) & 63));
			out[at++] = (char)((c < 0x800 ? 0xc0 : 0x80) | ((c >> 6) & 63));
			out[at++] = (char)(0x80 | (c & 63));
		}
	}
	out[at] = 0;
	return 0;
}

static int name_encode(const char *s, uint8_t *out)
{
	unsigned units = 0;
	memset(out, 0, 72);
	if (!s) return 0;
	while (*s) {
		uint32_t c = (unsigned char)*s++, minimum = 0;
		unsigned more = 0;
		if (c >= 0xc2 && c <= 0xdf) { c &= 31; more = 1; minimum = 0x80; }
		else if (c >= 0xe0 && c <= 0xef) { c &= 15; more = 2; minimum = 0x800; }
		else if (c >= 0xf0 && c <= 0xf4) { c &= 7; more = 3; minimum = 0x10000; }
		else if (c >= 0x80) return EINVAL;
		while (more--) {
			unsigned v = (unsigned char)*s++;
			if ((v & 0xc0) != 0x80) return EINVAL;
			c = (c << 6) | (v & 63);
		}
		if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) ||
		    c < 32 || (c >= 127 && c <= 159)) return EINVAL;
		if (units + (c >= 0x10000 ? 2U : 1U) > 36) return ENAMETOOLONG;
		if (c >= 0x10000) {
			uint32_t hi = 0xd800 + ((c - 0x10000) >> 10);
			out[units * 2] = hi; out[units++ * 2 + 1] = hi >> 8;
			c = 0xdc00 + ((c - 0x10000) & 1023);
		}
		out[units * 2] = c; out[units++ * 2 + 1] = c >> 8;
	}
	return 0;
}

static int read_lba(const struct dp_io *io, uint64_t lba, void *p, size_t n)
{
	if (lba >= io->sectors || n > (io->sectors - lba) * io->sector_size)
		return EOVERFLOW;
	return io->read(io->context, lba * io->sector_size, p, n);
}

static int load_copy(const struct dp_io *io, uint64_t lba, struct dp_copy *c)
{
	uint32_t crc;
	uint64_t blocks, reserved;
	int error;
	c->header = malloc(io->sector_size);
	if (!c->header) return ENOMEM;
	error = read_lba(io, lba, c->header, io->sector_size);
	if (error) return error;
	if (memcmp(c->header, "EFI PART", 8) || get32(c->header + 8) != 0x10000 ||
	    get32(c->header + 20)) return EINVAL;
	c->header_size = get32(c->header + 12);
	if (c->header_size < 92 || c->header_size > io->sector_size) return EINVAL;
	crc = get32(c->header + 16); put32(c->header + 16, 0);
	error = dp_crc32(c->header, c->header_size) == crc ? 0 : EINVAL;
	put32(c->header + 16, crc);
	if (error) return error;
	c->lba = get64(c->header + 24); c->alternate = get64(c->header + 32);
	c->first = get64(c->header + 40); c->last = get64(c->header + 48);
	c->table_lba = get64(c->header + 72);
	c->slots = get32(c->header + 80); c->entry_size = get32(c->header + 84);
	if (c->lba != lba || c->alternate >= io->sectors || c->alternate == lba ||
	    c->alternate == 0 || (lba != 1 && c->alternate != 1) ||
	    c->first > c->last || c->last >= io->sectors ||
	    zero(c->header + 56, 16) || !c->slots || c->slots > DP_MAX_SLOTS ||
	    c->entry_size < 128 || c->entry_size > 4096 ||
	    (c->entry_size & (c->entry_size - 1))) return EINVAL;
	c->bytes = (size_t)c->slots * c->entry_size;
	if (c->bytes > 1024U * 1024U) return EOPNOTSUPP;
	blocks = (c->bytes + io->sector_size - 1) / io->sector_size;
	reserved = (16384U + io->sector_size - 1) / io->sector_size;
	if (c->first < 2 + reserved || c->table_lba < 2 ||
	    c->table_lba >= io->sectors || blocks > io->sectors - c->table_lba)
		return EINVAL;
	if (lba == 1) {
		if (c->table_lba + blocks > c->first || c->alternate <= c->last ||
		    c->alternate - c->last <= reserved) return EINVAL;
	} else if (c->table_lba <= c->last || c->table_lba + blocks > lba ||
	    lba - c->last <= reserved) return EINVAL;
	c->entries = malloc(c->bytes);
	if (!c->entries) return ENOMEM;
	error = read_lba(io, c->table_lba, c->entries, c->bytes);
	if (error) return error;
	return dp_crc32(c->entries, c->bytes) == get32(c->header + 88) ? 0 : EINVAL;
}

static int parse_parts(struct dp_table *t, const uint8_t *raw)
{
	struct dp_copy *c = &t->copy[t->selected];
	t->count = 0;
	for (unsigned i = 0; i < t->slots; i++) {
		const uint8_t *p = raw + (t->format == DP_GPT ? i * c->entry_size : 446 + i * 16);
		struct dp_part *part = &t->parts[t->count];
		uint64_t last;
		memset(part, 0, sizeof(*part));
		part->slot = i + 1;
		if (t->format == DP_GPT) {
			if (zero(p, 16)) continue;
			part->start = get64(p + 32); last = get64(p + 40);
			if (zero(p + 16, 16) || part->start < c->first || last > c->last ||
			    part->start > last) return EINVAL;
			part->count = last - part->start + 1;
			part->attributes = get64(p + 48);
			dp_guid_text(p, part->type); dp_guid_text(p + 16, part->uuid);
			if (name_decode(p + 56, part->name)) return EINVAL;
		} else {
			part->start = get32(p + 8); part->count = get32(p + 12);
			if (!p[4]) {
				if (part->start || part->count || p[0]) return EINVAL;
				continue;
			}
			if (!part->start || !part->count || part->start >= t->io.sectors ||
			    part->count > t->io.sectors - part->start ||
			    (p[0] != 0 && p[0] != 0x80)) return EINVAL;
			if (p[4] == 5 || p[4] == 15 || p[4] == 0x85)
				t->restrictions |= DP_UNSUPPORTED;
			snprintf(part->type, sizeof(part->type), "%02x", p[4]);
			snprintf(part->uuid, sizeof(part->uuid), "%08x-%02x", get32(raw + 440), i + 1);
			part->attributes = p[0];
		}
		for (unsigned j = 0; j < t->count; j++) {
			struct dp_part *other = &t->parts[j];
			if ((part->start < other->start + other->count &&
			    other->start < part->start + part->count) ||
			    (t->format == DP_GPT && !strcmp(part->uuid, other->uuid))) return EINVAL;
		}
		t->count++;
	}
	return 0;
}

void dp_free(struct dp_table *t)
{
	free(t->mbr); free(t->edited); free(t->parts);
	for (unsigned i = 0; i < 2; i++) { free(t->copy[i].header); free(t->copy[i].entries); }
	memset(t, 0, sizeof(*t));
}

int dp_load(struct dp_table *t, const struct dp_io *io)
{
	uint8_t probe[4096];
	unsigned protective = 0, hybrid = 0;
	int error, a, b;
	uint64_t backup;
	const uint8_t *raw;
	size_t bytes;
	memset(t, 0, sizeof(*t));
	if (!io || !io->read || (io->sector_size != 512 && io->sector_size != 4096) ||
	    io->sectors < 3 || io->sectors > INT64_MAX / io->sector_size) return EINVAL;
	t->io = *io; t->mbr = malloc(io->sector_size);
	if (!t->mbr) return ENOMEM;
	error = read_lba(io, 0, t->mbr, io->sector_size);
	if (error) goto fail;
	if (t->mbr[510] != 0x55 || t->mbr[511] != 0xaa) { error = EINVAL; goto fail; }
	for (unsigned i = 0; i < 4; i++) {
		const uint8_t *p = t->mbr + 446 + i * 16;
		if (p[4] == 0xee) {
			protective++;
			if (p[0] || get32(p + 8) != 1 || !get32(p + 12)) { error = EINVAL; goto fail; }
			if (get32(p + 12) != (io->sectors - 1 > UINT32_MAX ?
			    UINT32_MAX : (uint32_t)(io->sectors - 1))) t->restrictions |= DP_UNSUPPORTED;
		} else if (!zero(p, 16)) hybrid++;
	}
	if (!protective) {
		error = read_lba(io, 1, probe, io->sector_size);
		if (error) goto fail;
		if (!memcmp(probe, "EFI PART", 8)) { error = EINVAL; goto fail; }
		t->format = DP_MBR; t->slots = 4;
		if (io->sector_size != 512) t->restrictions |= DP_UNSUPPORTED;
		raw = t->mbr; bytes = io->sector_size;
	} else {
		if (protective != 1) { error = EINVAL; goto fail; }
		t->format = DP_GPT;
		if (hybrid) t->restrictions |= DP_UNSUPPORTED;
		a = load_copy(io, 1, &t->copy[0]);
		backup = a == 0 ? t->copy[0].alternate : io->sectors - 1;
		b = load_copy(io, backup, &t->copy[1]);
		if (a && b) { error = a; goto fail; }
		if (a || b) { t->restrictions |= DP_DEGRADED; t->selected = a ? 1 : 0; }
		else if (t->copy[1].alternate != 1 ||
		    t->copy[0].header_size != t->copy[1].header_size ||
		    t->copy[0].first != t->copy[1].first || t->copy[0].last != t->copy[1].last ||
		    t->copy[0].slots != t->copy[1].slots || t->copy[0].entry_size != t->copy[1].entry_size ||
		    memcmp(t->copy[0].header + 56, t->copy[1].header + 56, 16) ||
		    memcmp(t->copy[0].entries, t->copy[1].entries, t->copy[0].bytes)) {
			error = EINVAL; goto fail;
		}
		struct dp_copy *c = &t->copy[t->selected];
		if (backup != io->sectors - 1 || c->entry_size != 128 || c->header_size != 92)
			t->restrictions |= DP_UNSUPPORTED;
		t->slots = c->slots; raw = c->entries; bytes = c->bytes;
	}
	t->edited = malloc(bytes); t->parts = calloc(t->slots, sizeof(*t->parts));
	if (!t->edited || !t->parts) { error = ENOMEM; goto fail; }
	memcpy(t->edited, raw, bytes);
	error = parse_parts(t, t->edited);
	if (!error) return 0;
fail:
	dp_free(t); return error;
}

int dp_add(struct dp_table *t, unsigned slot, uint64_t start, uint64_t count,
	const char *type, const char *uuid, const char *name)
{
	uint8_t entry[128];
	uint8_t *destination;
	unsigned size = t->format == DP_GPT ? 128 : 16;
	int error;
	if (t->restrictions) return EOPNOTSUPP;
	if (!slot || slot > t->slots || !count || start >= t->io.sectors ||
	    count > t->io.sectors - start) return EINVAL;
	destination = t->edited + (t->format == DP_GPT ? (slot - 1) * 128 : 446 + (slot - 1) * 16);
	/* Avoid silently overwriting nonzero unused/reserved records. */
	if (!zero(destination, size)) return EEXIST;
	memset(entry, 0, sizeof(entry));
	if (t->format == DP_GPT) {
		if (dp_guid_parse(type, entry) || dp_guid_parse(uuid, entry + 16)) return EINVAL;
		put64(entry + 32, start); put64(entry + 40, start + count - 1);
		error = name_encode(name, entry + 56); if (error) return error;
	} else {
		char *end;
		unsigned long v;
		if (!type || !*type || *type == '-' || *type == '+') return EINVAL;
		errno = 0; v = strtoul(type, &end, 16);
		if (errno || *end || !v || v > 255 || v == 5 || v == 15 || v == 0x85 || v == 0xee ||
		    start > UINT32_MAX || count > UINT32_MAX || uuid || name) return EINVAL;
		entry[4] = (uint8_t)v;
		memset(entry + 1, 0xff, 3); memset(entry + 5, 0xff, 3);
		put32(entry + 8, (uint32_t)start); put32(entry + 12, (uint32_t)count);
	}
	memcpy(destination, entry, size);
	error = parse_parts(t, t->edited);
	if (error) { memset(destination, 0, size); (void)parse_parts(t, t->edited); return error; }
	t->changed = 1;
	return 0;
}

int dp_delete(struct dp_table *t, unsigned slot)
{
	uint8_t *p;
	unsigned size = t->format == DP_GPT ? 128 : 16;
	if (t->restrictions) return EOPNOTSUPP;
	if (!slot || slot > t->slots) return EINVAL;
	p = t->edited + (t->format == DP_GPT ? (slot - 1) * 128 : 446 + (slot - 1) * 16);
	if (t->format == DP_GPT ? zero(p, 16) : !p[4]) return ENOENT;
	memset(p, 0, size); t->changed = 1;
	return parse_parts(t, t->edited);
}

static int unchanged(struct dp_table *t, uint64_t lba, const uint8_t *expected, size_t size)
{
	uint8_t *actual = malloc(size);
	int error;
	if (!actual) return ENOMEM;
	error = read_lba(&t->io, lba, actual, size);
	if (!error && memcmp(actual, expected, size)) error = EBUSY;
	free(actual); return error;
}

int dp_write(struct dp_table *t, int *started)
{
	int error;
	uint8_t header[4096];
	if (!started) return EINVAL;
	*started = 0;
	if (!t->changed || t->restrictions || !t->io.write || !t->io.flush) return EINVAL;
	error = unchanged(t, 0, t->mbr, t->io.sector_size);
	if (error) return error;
	if (t->format == DP_GPT) {
		for (unsigned i = 0; i < 2; i++) {
			struct dp_copy *c = &t->copy[i];
			error = unchanged(t, c->lba, c->header, t->io.sector_size);
			if (!error) error = unchanged(t, c->table_lba, c->entries, c->bytes);
			if (error) return error;
		}
	}
	*started = 1;
	if (t->format == DP_MBR) {
		error = t->io.write(t->io.context, 0, t->edited, t->io.sector_size);
		if (!error) error = t->io.flush(t->io.context);
		if (!error) error = unchanged(t, 0, t->edited, t->io.sector_size);
		return error;
	}
	for (unsigned step = 0; step < 2; step++) {
		struct dp_copy *c = &t->copy[1 - step];
		memcpy(header, c->header, t->io.sector_size);
		put32(header + 88, dp_crc32(t->edited, c->bytes));
		put32(header + 16, 0); put32(header + 16, dp_crc32(header, c->header_size));
		error = t->io.write(t->io.context, c->table_lba * t->io.sector_size, t->edited, c->bytes);
		if (!error) error = t->io.write(t->io.context, c->lba * t->io.sector_size, header, t->io.sector_size);
		if (!error) error = t->io.flush(t->io.context);
		if (!error) error = unchanged(t, c->lba, header, t->io.sector_size);
		if (!error) error = unchanged(t, c->table_lba, t->edited, c->bytes);
		if (error) return error;
	}
	return 0;
}
