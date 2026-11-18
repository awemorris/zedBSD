/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The session's requests and the plan of an installation, read and
 * checked (plan.h).  The reading is done from the bytes received so far:
 * a request not yet whole is EAGAIN, so the session reads more and asks
 * again, and nothing of a request is believed before all of it is read.
 */

#include "plan.h"

#include "userland/base/common/account.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest token other than a typed field. */
#define PLAN_TOKEN_MAX		128U

/* The most digits a typed field's length has. */
#define PLAN_FIELD_DIGITS	5U

/* Where the reading of one request has got to. */
struct plan_reader {
	const char *text;
	size_t length;
	size_t offset;
	int ended;
};

static int plan_token(struct plan_reader *reader, char *token, size_t capacity);
static int plan_word(struct plan_reader *reader, const char *word);
static int plan_field(struct plan_reader *reader, char *field, size_t capacity);
static int plan_decimal(const char *text, uint64_t *value);
static int plan_number(struct plan_reader *reader, uint64_t *value);
static int plan_read_disk(struct plan_reader *reader, struct plan_disk *disk);
static int plan_read_tuple(struct plan_reader *reader, struct plan_tuple *tuple);
static int plan_read_extent(struct plan_reader *reader, struct plan_extent *extent);
static int plan_read_edit(struct plan_reader *reader, struct plan_request *request);
static int plan_read_layout(struct plan_reader *reader, struct install_plan *plan);
static int plan_read_units(struct plan_reader *reader, struct install_plan *plan);
static int plan_read_people(struct plan_reader *reader, struct install_plan *plan);
static int plan_read_install(struct plan_reader *reader, struct plan_request *request);
static int plan_end(const struct plan_reader *reader);
static int plan_token_valid(const char *token);
static int plan_extent_inside(const struct plan_disk *disk, uint64_t start, uint64_t sectors);
static int plan_extent_aligned(const struct plan_disk *disk, const struct plan_extent *extent);
static int plan_overlap(uint64_t start, uint64_t sectors, uint64_t other_start, uint64_t other_sectors);
static int plan_tuple_check(const struct plan_disk *disk, const struct plan_tuple *tuple);
static int plan_whole_check(const struct plan_disk *disk, const struct install_plan *plan);
static int plan_chosen_check(const struct plan_disk *disk, const struct install_plan *plan);
static int plan_append(char *text, size_t capacity, size_t *used, const char *line, const char *value);

/*
 * Reads the first request of the bytes received.  Returns 0 with the
 * request and the bytes it took (consumed), EAGAIN when the bytes end
 * before the request does, EMSGSIZE when a request runs past
 * PLAN_REQUEST_MAX, and EINVAL for one that does not follow plan.h.  The
 * request's typed fields are checked by plan_install_check, not here.
 */
int
plan_request_read(
	const char *text,
	size_t length,
	struct plan_request *request,
	size_t *consumed)
{
	struct plan_reader reader;
	char verb[PLAN_TOKEN_MAX];
	int probe_diff;
	int cancel_diff;
	int edit_diff;
	int install_diff;
	int error;

	/* A missing text or result. */
	if (text == NULL || request == NULL || consumed == NULL)
		return EINVAL;

	/* Reads from the first byte, the request starting empty. */
	memset(request, 0, sizeof(*request));
	reader.text = text;
	reader.length = length;
	if (reader.length > PLAN_REQUEST_MAX)
		reader.length = PLAN_REQUEST_MAX;
	reader.offset = 0;
	reader.ended = 0;

	/* The verb decides the rest. */
	error = plan_token(&reader, verb, sizeof(verb));
	if (error == 0) {
		probe_diff = strcmp(verb, "probe");
		cancel_diff = strcmp(verb, "cancel");
		edit_diff = strcmp(verb, "edit");
		install_diff = strcmp(verb, "install");
		if (probe_diff == 0) {
			/* A probe has nothing more. */
			request->verb = PLAN_PROBE;
		} else if (cancel_diff == 0) {
			/* A cancellation has nothing more. */
			request->verb = PLAN_CANCEL;
		} else if (edit_diff == 0) {
			/* One change of the partition table. */
			error = plan_read_edit(&reader, request);
		} else if (install_diff == 0) {
			/* The installation. */
			error = plan_read_install(&reader, request);
		} else {
			/* No such request. */
			error = EINVAL;
		}
	}

	/* The request must end where its grammar does. */
	if (error == 0)
		error = plan_end(&reader);

	/* A request cut short by the end of what was received is too long once the session can hold no more. */
	if (error == EAGAIN && length >= PLAN_REQUEST_MAX)
		error = EMSGSIZE;

	/* A request not read leaves nothing of it behind, its passwords least of all. */
	if (error != 0) {
		plan_request_forget(request);
		return error;
	}

	/* Succeeded: the request and the bytes it took. */
	*consumed = reader.offset;
	return 0;
}

/*
 * Checks an installation's plan again, as the helper must: the disk's
 * geometry, a whole-disk layout that fits, chosen partitions on the
 * disk that do not overlap, at least one unit and none twice, and the
 * typed fields by their rules.  Returns 0 or EINVAL.  Whether the
 * partitions are still the disk's, the units the index's and the user
 * one the system may have are the session's to check.
 */
int
plan_install_check(
	const struct plan_disk *disk,
	const struct install_plan *plan)
{
	unsigned i;
	unsigned j;
	int same;
	int valid;
	int error;

	/* A missing disk or plan. */
	if (disk == NULL || plan == NULL)
		return EINVAL;

	/* A sector size the tools and the kernel take. */
	if (disk->sector_bytes != 512U && disk->sector_bytes != 4096U)
		return EINVAL;
	if (disk->sectors == 0U)
		return EINVAL;

	/* The layout. */
	if (plan->layout == PLAN_WHOLE)
		error = plan_whole_check(disk, plan);
	else if (plan->layout == PLAN_CHOSEN)
		error = plan_chosen_check(disk, plan);
	else
		error = EINVAL;
	if (error != 0)
		return error;

	/* Something to install, each unit once. */
	if (plan->unit_count == 0U || plan->unit_count > PLAN_UNITS_MAX)
		return EINVAL;
	for (i = 0; i < plan->unit_count; i++) {
		for (j = i + 1U; j < plan->unit_count; j++) {
			same = strcmp(plan->unit[i], plan->unit[j]);
			if (same == 0)
				return EINVAL;
		}
	}

	/* The user, as Settings would allow one. */
	valid = plan_user_valid(plan->user);
	if (!valid)
		return EINVAL;
	valid = plan_fullname_valid(plan->fullname);
	if (!valid)
		return EINVAL;

	/* The passwords, both of them set. */
	valid = plan_password_valid(plan->password);
	if (!valid)
		return EINVAL;
	valid = plan_password_valid(plan->root_password);
	if (!valid)
		return EINVAL;

	/* The hostname. */
	valid = plan_hostname_valid(plan->hostname);
	if (!valid)
		return EINVAL;

	/* Succeeded: the plan holds together. */
	return 0;
}

/*
 * Checks one change of the partition table: a partition to delete on the
 * request's disk, or new extents inside the disk, on the 1 MiB grid, an
 * ESP not smaller than a new one is made and not overlapping the other
 * new partition.  Returns 0 or EINVAL.
 */
int
plan_edit_check(
	const struct plan_request *request)
{
	int overlapping;
	int error;

	/* A missing request. */
	if (request == NULL)
		return EINVAL;

	/* A sector size the tools and the kernel take. */
	if (request->disk.sector_bytes != 512U && request->disk.sector_bytes != 4096U)
		return EINVAL;

	/* A delete names a partition of the disk. */
	if (request->verb == PLAN_EDIT_DELETE) {
		error = plan_tuple_check(&request->disk, &request->part);
		if (error != 0)
			return error;

		/* Succeeded: the delete may be tried. */
		return 0;
	}

	/* Anything else makes partitions. */
	if (request->verb != PLAN_EDIT_CREATE && request->verb != PLAN_EDIT_CREATE_WITH_ESP)
		return EINVAL;

	/* The new partition. */
	error = plan_extent_aligned(&request->disk, &request->part_new);
	if (error != 0)
		return error;

	/* A create without an ESP is done. */
	if (request->verb == PLAN_EDIT_CREATE)
		return 0;

	/* The ESP, as large as a new one is made. */
	error = plan_extent_aligned(&request->disk, &request->esp_new);
	if (error != 0)
		return error;
	if (request->esp_new.sectors < PLAN_ESP_MIN_BYTES / request->disk.sector_bytes)
		return EINVAL;

	/* The two new partitions apart. */
	overlapping = plan_overlap(
		request->esp_new.start,
		request->esp_new.sectors,
		request->part_new.start,
		request->part_new.sectors);
	if (overlapping)
		return EINVAL;

	/* Succeeded: the change may be tried. */
	return 0;
}

/*
 * Reports whether a login name is allowed (the rule of account-admin): a
 * lower-case letter, then up to 31 lower-case letters, digits, '-' or
 * '_', and not root.  The system's own accounts are refused when the
 * installed /etc/passwd is read (target.c).
 */
int
plan_user_valid(
	const char *name)
{
	size_t i;
	char c;
	int root;

	/* A missing name. */
	if (name == NULL)
		return 0;

	/* The first character, a lower-case letter. */
	if (name[0] < 'a' || name[0] > 'z')
		return 0;

	/* The rest, and the length. */
	for (i = 1; name[i] != '\0'; i++) {
		/* Too long. */
		if (i >= PLAN_USER_MAX)
			return 0;

		/* A character allowed. */
		c = name[i];
		if ((c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') ||
		    c == '-' ||
		    c == '_')
			continue;
		return 0;
	}

	/* Root is the administrator, not the user made. */
	root = strcmp(name, "root");
	if (root == 0)
		return 0;

	/* Allowed. */
	return 1;
}

/*
 * Reports whether a display name is allowed (the rule of account-admin):
 * no control character, no ':', not longer than 64 bytes (UTF-8 is taken
 * as it is).  An empty one is allowed.
 */
int
plan_fullname_valid(
	const char *text)
{
	size_t i;
	unsigned char c;

	/* A missing name. */
	if (text == NULL)
		return 0;

	/* Each byte. */
	for (i = 0; text[i] != '\0'; i++) {
		/* Too long. */
		if (i >= PLAN_FULLNAME_MAX)
			return 0;

		/* A control character or the field separator of /etc/passwd. */
		c = (unsigned char)text[i];
		if (c < 0x20U || c == 0x7fU || c == ':')
			return 0;
	}

	/* Allowed. */
	return 1;
}

/*
 * Reports whether a password is allowed: the rule root's choices for an
 * account follow (account.c): not empty, not longer than 256 bytes, no
 * control character.
 */
int
plan_password_valid(
	const char *password)
{
	int reason;

	/* A missing password. */
	if (password == NULL)
		return 0;

	/* The shared rule, as root sets it. */
	reason = account_password_check(password, NULL, 1);
	if (reason != ACCOUNT_PASSWORD_OK)
		return 0;

	/* Allowed. */
	return 1;
}

/*
 * Reports whether a hostname is allowed: one label of lower-case letters,
 * digits and '-', 1 to 63 of them, neither starting nor ending with '-'.
 */
int
plan_hostname_valid(
	const char *name)
{
	size_t i;
	char c;

	/* A missing or empty name. */
	if (name == NULL || name[0] == '\0')
		return 0;

	/* Each character, and the length. */
	for (i = 0; name[i] != '\0'; i++) {
		/* Too long. */
		if (i >= PLAN_HOSTNAME_MAX)
			return 0;

		/* A character allowed. */
		c = name[i];
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			continue;
		if (c == '-' && i != 0U && name[i + 1U] != '\0')
			continue;
		return 0;
	}

	/* Allowed. */
	return 1;
}

/*
 * Reports whether a text is a GUID in lower case (8-4-4-4-12 hexadecimal
 * digits) and not all zeros, the GUID no partition may have.
 */
int
plan_guid_valid(
	const char *text)
{
	size_t length;
	size_t i;
	char c;
	int nonzero;

	/* Its length. */
	if (text == NULL)
		return 0;
	length = strlen(text);
	if (length != PLAN_GUID_TEXT)
		return 0;

	/* The dashes where they go, lower-case digits elsewhere. */
	nonzero = 0;
	for (i = 0; i < length; i++) {
		c = text[i];
		if (i == 8U || i == 13U || i == 18U || i == 23U) {
			/* A dash between the groups. */
			if (c != '-')
				return 0;
		} else if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
			/* A digit, any one but 0 making the GUID one a partition may have. */
			if (c != '0')
				nonzero = 1;
		} else {
			/* Anything else. */
			return 0;
		}
	}

	/* Reports whether the GUID is not the null one. */
	return nonzero;
}

/* Reports whether two tuples name the same partition. */
int
plan_tuple_same(
	const struct plan_tuple *left,
	const struct plan_tuple *right)
{
	int differs;

	/* The disk, the range. */
	if (left->disk != right->disk)
		return 0;
	if (left->start != right->start || left->sectors != right->sectors)
		return 0;

	/* The PARTUUID, both in lower case. */
	differs = strcmp(left->partuuid, right->partuuid);
	if (differs != 0)
		return 0;

	/* The same partition. */
	return 1;
}

/*
 * Writes the text of an installed system's zedbsd.cfg: the kernel and
 * the logo from the root's UFS, the root (and the swap partition, when
 * there is one) by PARTUUID, the graphical login when the desktop is
 * installed, and the kernel's messages kept quiet.  boot0= is not
 * written: the loader adds it from the ESP it was read from.  Returns 0,
 * EINVAL for a PARTUUID that is not one, or ERANGE when the text does not
 * fit.
 */
int
plan_cfg_text(
	const struct plan_cfg *cfg,
	char *text,
	size_t capacity,
	size_t *length)
{
	size_t used;
	int valid;
	int error;

	/* A missing configuration or result. */
	if (cfg == NULL || text == NULL || length == NULL || capacity == 0U)
		return EINVAL;

	/* The PARTUUIDs the lines name. */
	valid = plan_guid_valid(cfg->root_partuuid);
	if (!valid)
		return EINVAL;
	if (cfg->swap_partuuid != NULL) {
		valid = plan_guid_valid(cfg->swap_partuuid);
		if (!valid)
			return EINVAL;
	}

	/* The kernel from the root, and the root. */
	used = 0;
	text[0] = '\0';
	error = plan_append(text, capacity, &used, "kernel=rootpart:/boot/vmunix", NULL);
	if (error != 0)
		return error;
	error = plan_append(text, capacity, &used, "rootpart=PARTUUID=", cfg->root_partuuid);
	if (error != 0)
		return error;

	/* The swap partition, when the plan has one; a swap file is /etc/fstab's. */
	if (cfg->swap_partuuid != NULL) {
		error = plan_append(text, capacity, &used, "swap0=PARTUUID=", cfg->swap_partuuid);
		if (error != 0)
			return error;
	}

	/* The logo from the root. */
	error = plan_append(text, capacity, &used, "logo=rootpart:/boot/logo.ppm", NULL);
	if (error != 0)
		return error;

	/* The graphical login, only with the desktop to log in to. */
	if (cfg->graphical) {
		error = plan_append(text, capacity, &used, "login=graphical", NULL);
		if (error != 0)
			return error;
	}

	/* The kernel's messages, kept off the screen. */
	error = plan_append(text, capacity, &used, "kmsg=quiet", NULL);
	if (error != 0)
		return error;

	/* Succeeded: the text and its length. */
	*length = used;
	return 0;
}

/* Clears a request's passwords from memory. */
void
plan_request_forget(
	struct plan_request *request)
{
	volatile char *byte;
	size_t i;

	/* A missing request. */
	if (request == NULL)
		return;

	/* Each byte of both, through a volatile pointer so the clearing is not left out. */
	byte = request->install.password;
	for (i = 0; i < sizeof(request->install.password); i++)
		byte[i] = '\0';
	byte = request->install.root_password;
	for (i = 0; i < sizeof(request->install.root_password); i++)
		byte[i] = '\0';
}

/*
 * Reads one token, up to the space or the newline after it (which it
 * takes, a newline ending the request).  Returns EAGAIN when the bytes end
 * first, EINVAL after the request's end, for an empty token, a byte that
 * is not printable ASCII, or one longer than the capacity.
 */
static int
plan_token(
	struct plan_reader *reader,
	char *token,
	size_t capacity)
{
	size_t used;
	char c;

	/* Nothing follows the newline. */
	if (reader->ended)
		return EINVAL;

	/* The bytes up to the separator. */
	used = 0;
	while (reader->offset + used < reader->length) {
		c = reader->text[reader->offset + used];
		if (c == ' ' || c == '\n') {
			/* An empty token, as two spaces make. */
			if (used == 0U)
				return EINVAL;

			/* Takes the token and its separator. */
			token[used] = '\0';
			reader->offset += used + 1U;
			if (c == '\n')
				reader->ended = 1;
			return 0;
		}

		/* A byte a token may not have, or one too many. */
		if ((unsigned char)c < 0x21U || (unsigned char)c > 0x7eU)
			return EINVAL;
		if (used + 1U >= capacity)
			return EINVAL;
		token[used] = c;
		used++;
	}

	/* The token goes on past what was received. */
	return EAGAIN;
}

/* Reads one token that must be the word given. */
static int
plan_word(
	struct plan_reader *reader,
	const char *word)
{
	char token[PLAN_TOKEN_MAX];
	int differs;
	int error;

	/* The token. */
	error = plan_token(reader, token, sizeof(token));
	if (error != 0)
		return error;

	/* Another word. */
	differs = strcmp(token, word);
	if (differs != 0)
		return EINVAL;

	/* Succeeded: the word was there. */
	return 0;
}

/*
 * Reads one typed field, LENGTH:BYTES and its separator, into a string.
 * Returns EAGAIN when the bytes end first, EINVAL for a length that is not
 * decimal, longer than the capacity allows, a NUL among the bytes or no
 * separator after them.
 */
static int
plan_field(
	struct plan_reader *reader,
	char *field,
	size_t capacity)
{
	size_t digits;
	size_t size;
	size_t start;
	char c;
	void *nul;

	/* Nothing follows the newline. */
	if (reader->ended)
		return EINVAL;

	/* The length, up to the colon. */
	size = 0;
	digits = 0;
	for (;;) {
		/* The length goes on past what was received. */
		if (reader->offset + digits >= reader->length)
			return EAGAIN;

		/* The colon ends it; anything else must be a digit. */
		c = reader->text[reader->offset + digits];
		if (c == ':')
			break;
		if (c < '0' || c > '9' || digits >= PLAN_FIELD_DIGITS)
			return EINVAL;
		size = size * 10U + (size_t)(c - '0');
		digits++;
	}

	/* A length, and one that fits. */
	if (digits == 0U || size + 1U > capacity)
		return EINVAL;

	/* The bytes and the separator after them. */
	start = reader->offset + digits + 1U;
	if (start + size >= reader->length)
		return EAGAIN;
	c = reader->text[start + size];
	if (c != ' ' && c != '\n')
		return EINVAL;

	/* A NUL would cut the string short of what was sent. */
	nul = memchr(reader->text + start, '\0', size);
	if (nul != NULL)
		return EINVAL;

	/* Takes the field and its separator. */
	memcpy(field, reader->text + start, size);
	field[size] = '\0';
	reader->offset = start + size + 1U;
	if (c == '\n')
		reader->ended = 1;

	/* Succeeded: the field is read. */
	return 0;
}

/* Reads a decimal number without a sign, refusing one that does not fit 64 bits. */
static int
plan_decimal(
	const char *text,
	uint64_t *value)
{
	uint64_t number;
	unsigned digit;
	size_t i;

	/* At least one digit. */
	if (text[0] == '\0')
		return EINVAL;

	/* Each digit, the number checked before it grows. */
	number = 0;
	for (i = 0; text[i] != '\0'; i++) {
		if (text[i] < '0' || text[i] > '9')
			return EINVAL;
		digit = (unsigned)(text[i] - '0');
		if (number > (UINT64_MAX - digit) / 10U)
			return EINVAL;
		number = number * 10U + digit;
	}

	/* Succeeded: the number. */
	*value = number;
	return 0;
}

/* Reads one token that is a decimal number. */
static int
plan_number(
	struct plan_reader *reader,
	uint64_t *value)
{
	char token[PLAN_TOKEN_MAX];
	int error;

	/* The token. */
	error = plan_token(reader, token, sizeof(token));
	if (error != 0)
		return error;

	/* Its number. */
	error = plan_decimal(token, value);
	if (error != 0)
		return error;

	/* Succeeded: the number is read. */
	return 0;
}

/* Reads "disk REGISTRATION NAME SECTOR-BYTES SECTORS DIGEST". */
static int
plan_read_disk(
	struct plan_reader *reader,
	struct plan_disk *disk)
{
	char token[PLAN_TOKEN_MAX];
	uint64_t sector_bytes;
	size_t length;
	int valid;
	int error;

	/* The word, the registration number. */
	error = plan_word(reader, "disk");
	if (error != 0)
		return error;
	error = plan_number(reader, &disk->registration);
	if (error != 0)
		return error;

	/* The device name, a name under /dev of one component. */
	error = plan_token(reader, token, sizeof(token));
	if (error != 0)
		return error;
	length = strlen(token);
	if (length > PLAN_DEVICE_MAX)
		return EINVAL;
	valid = plan_token_valid(token);
	if (!valid)
		return EINVAL;
	memcpy(disk->name, token, length + 1U);

	/* The geometry. */
	error = plan_number(reader, &sector_bytes);
	if (error != 0)
		return error;
	if (sector_bytes > UINT32_MAX)
		return EINVAL;
	disk->sector_bytes = (uint32_t)sector_bytes;
	error = plan_number(reader, &disk->sectors);
	if (error != 0)
		return error;

	/* The digest of the table the screen showed. */
	error = plan_token(reader, token, sizeof(token));
	if (error != 0)
		return error;
	error = index_hex_digest(token, disk->table_digest);
	if (error != 0)
		return error;

	/* Succeeded: the disk is read. */
	return 0;
}

/* Reads a tuple, REGISTRATION:START:SECTORS:PARTUUID. */
static int
plan_read_tuple(
	struct plan_reader *reader,
	struct plan_tuple *tuple)
{
	char token[PLAN_TOKEN_MAX];
	char *part[4];
	char *colon;
	unsigned count;
	size_t i;
	int valid;
	int error;

	/* The token. */
	error = plan_token(reader, token, sizeof(token));
	if (error != 0)
		return error;

	/* Cut at its three colons. */
	part[0] = token;
	count = 1;
	colon = strchr(token, ':');
	while (colon != NULL) {
		if (count >= 4U)
			return EINVAL;
		*colon = '\0';
		part[count] = colon + 1;
		count++;
		colon = strchr(colon + 1, ':');
	}

	/* A tuple has exactly four parts. */
	if (count != 4U)
		return EINVAL;

	/* The numbers. */
	error = plan_decimal(part[0], &tuple->disk);
	if (error != 0)
		return error;
	error = plan_decimal(part[1], &tuple->start);
	if (error != 0)
		return error;
	error = plan_decimal(part[2], &tuple->sectors);
	if (error != 0)
		return error;

	/* The PARTUUID, taken in lower case. */
	for (i = 0; part[3][i] != '\0'; i++) {
		if (part[3][i] >= 'A' && part[3][i] <= 'F')
			part[3][i] = (char)(part[3][i] - 'A' + 'a');
	}

	/* Refuses a PARTUUID that is not a GUID. */
	valid = plan_guid_valid(part[3]);
	if (!valid)
		return EINVAL;
	memcpy(tuple->partuuid, part[3], PLAN_GUID_TEXT + 1U);

	/* Succeeded: the tuple is read. */
	return 0;
}

/* Reads "START COUNT", a new partition's sectors. */
static int
plan_read_extent(
	struct plan_reader *reader,
	struct plan_extent *extent)
{
	int error;

	/* The first sector. */
	error = plan_number(reader, &extent->start);
	if (error != 0)
		return error;

	/* The size in sectors. */
	error = plan_number(reader, &extent->sectors);
	if (error != 0)
		return error;

	/* Succeeded: the extent is read. */
	return 0;
}

/* Reads the rest of an edit request, after "edit". */
static int
plan_read_edit(
	struct plan_reader *reader,
	struct plan_request *request)
{
	char kind[PLAN_TOKEN_MAX];
	int delete_diff;
	int create_diff;
	int create_esp_diff;
	int error;

	/* Which change, and the disk. */
	error = plan_token(reader, kind, sizeof(kind));
	if (error != 0)
		return error;
	error = plan_read_disk(reader, &request->disk);
	if (error != 0)
		return error;

	/* Choose the change. */
	delete_diff = strcmp(kind, "delete");
	create_diff = strcmp(kind, "create");
	create_esp_diff = strcmp(kind, "create-with-esp");
	if (delete_diff == 0) {
		/* The partition to delete. */
		request->verb = PLAN_EDIT_DELETE;
		error = plan_word(reader, "part");
		if (error != 0)
			return error;
		error = plan_read_tuple(reader, &request->part);
	} else if (create_diff == 0) {
		/* The partition to make. */
		request->verb = PLAN_EDIT_CREATE;
		error = plan_word(reader, "new");
		if (error != 0)
			return error;
		error = plan_read_extent(reader, &request->part_new);
	} else if (create_esp_diff == 0) {
		/* The ESP and the partition to make together. */
		request->verb = PLAN_EDIT_CREATE_WITH_ESP;
		error = plan_word(reader, "esp");
		if (error != 0)
			return error;
		error = plan_read_extent(reader, &request->esp_new);
		if (error != 0)
			return error;
		error = plan_word(reader, "new");
		if (error != 0)
			return error;
		error = plan_read_extent(reader, &request->part_new);
	} else {
		/* No such change. */
		error = EINVAL;
	}

	/* Reports why the change could not be read. */
	if (error != 0)
		return error;

	/* Succeeded: the change is read. */
	return 0;
}

/* Reads LAYOUT: "whole ESP-BYTES SWAP-BYTES" or "chosen esp T root T swap T|none". */
static int
plan_read_layout(
	struct plan_reader *reader,
	struct install_plan *plan)
{
	char kind[PLAN_TOKEN_MAX];
	struct plan_reader saved;
	int whole_diff;
	int chosen_diff;
	int error;

	/* Which layout. */
	error = plan_token(reader, kind, sizeof(kind));
	if (error != 0)
		return error;

	/* The whole disk: the sizes of the ESP and of the swap partition. */
	whole_diff = strcmp(kind, "whole");
	chosen_diff = strcmp(kind, "chosen");
	if (whole_diff == 0) {
		plan->layout = PLAN_WHOLE;
		error = plan_number(reader, &plan->whole_esp_bytes);
		if (error != 0)
			return error;
		error = plan_number(reader, &plan->whole_swap_bytes);
		if (error != 0)
			return error;
		plan->has_swap_partition = 0;
		if (plan->whole_swap_bytes != 0U)
			plan->has_swap_partition = 1;

		/* Succeeded: the whole disk's layout is read. */
		return 0;
	}

	/* Anything but the two layouts. */
	if (chosen_diff != 0)
		return EINVAL;

	/* The chosen partitions: the ESP and the root. */
	plan->layout = PLAN_CHOSEN;
	error = plan_word(reader, "esp");
	if (error != 0)
		return error;
	error = plan_read_tuple(reader, &plan->esp);
	if (error != 0)
		return error;
	error = plan_word(reader, "root");
	if (error != 0)
		return error;
	error = plan_read_tuple(reader, &plan->root);
	if (error != 0)
		return error;

	/* The swap partition, or "none" for a swap file on the root. */
	error = plan_word(reader, "swap");
	if (error != 0)
		return error;
	saved = *reader;
	error = plan_word(reader, "none");
	if (error == 0) {
		/* Succeeded: a swap file is to be made. */
		plan->has_swap_partition = 0;
		return 0;
	}

	/* A token not yet whole, or a tuple read again from its start. */
	if (error == EAGAIN)
		return error;
	*reader = saved;
	error = plan_read_tuple(reader, &plan->swap);
	if (error != 0)
		return error;
	plan->has_swap_partition = 1;

	/* Succeeded: the layout is read. */
	return 0;
}

/* Reads "units N NAME...". */
static int
plan_read_units(
	struct plan_reader *reader,
	struct install_plan *plan)
{
	char token[PLAN_TOKEN_MAX];
	uint64_t count;
	size_t length;
	unsigned i;
	int valid;
	int error;

	/* The word and the count. */
	error = plan_word(reader, "units");
	if (error != 0)
		return error;
	error = plan_number(reader, &count);
	if (error != 0)
		return error;
	if (count == 0U || count > PLAN_UNITS_MAX)
		return EINVAL;

	/* Each name. */
	for (i = 0; i < (unsigned)count; i++) {
		error = plan_token(reader, token, sizeof(token));
		if (error != 0)
			return error;
		length = strlen(token);
		if (length >= INDEX_NAME_MAX)
			return EINVAL;
		valid = plan_token_valid(token);
		if (!valid)
			return EINVAL;
		memcpy(plan->unit[i], token, length + 1U);
	}

	/* Keeps the number of names read. */
	plan->unit_count = (unsigned)count;

	/* Succeeded: the units are read. */
	return 0;
}

/* Reads the user, the passwords, the automatic login and the hostname. */
static int
plan_read_people(
	struct plan_reader *reader,
	struct install_plan *plan)
{
	char answer[PLAN_TOKEN_MAX];
	int yes_diff;
	int no_diff;
	int error;

	/* The user's login name and display name. */
	error = plan_word(reader, "user");
	if (error != 0)
		return error;
	error = plan_field(reader, plan->user, sizeof(plan->user));
	if (error != 0)
		return error;
	error = plan_word(reader, "fullname");
	if (error != 0)
		return error;
	error = plan_field(reader, plan->fullname, sizeof(plan->fullname));
	if (error != 0)
		return error;

	/* The user's password and root's. */
	error = plan_word(reader, "password");
	if (error != 0)
		return error;
	error = plan_field(reader, plan->password, sizeof(plan->password));
	if (error != 0)
		return error;
	error = plan_word(reader, "root-password");
	if (error != 0)
		return error;
	error = plan_field(reader, plan->root_password, sizeof(plan->root_password));
	if (error != 0)
		return error;

	/* The automatic login: yes or no. */
	error = plan_word(reader, "autologin");
	if (error != 0)
		return error;
	error = plan_token(reader, answer, sizeof(answer));
	if (error != 0)
		return error;
	yes_diff = strcmp(answer, "yes");
	no_diff = strcmp(answer, "no");
	if (yes_diff == 0)
		plan->autologin = 1;
	else if (no_diff == 0)
		plan->autologin = 0;
	else
		return EINVAL;

	/* The hostname. */
	error = plan_word(reader, "hostname");
	if (error != 0)
		return error;
	error = plan_field(reader, plan->hostname, sizeof(plan->hostname));
	if (error != 0)
		return error;

	/* Succeeded: the people are read. */
	return 0;
}

/* Reads the rest of an install request, after "install". */
static int
plan_read_install(
	struct plan_reader *reader,
	struct plan_request *request)
{
	int error;

	/* The disk and its layout. */
	request->verb = PLAN_INSTALL;
	error = plan_read_disk(reader, &request->disk);
	if (error != 0)
		return error;
	error = plan_read_layout(reader, &request->install);
	if (error != 0)
		return error;

	/* What goes, and for whom. */
	error = plan_read_units(reader, &request->install);
	if (error != 0)
		return error;
	error = plan_read_people(reader, &request->install);
	if (error != 0)
		return error;

	/* Succeeded: the plan is read. */
	return 0;
}

/* Reports whether the request ended (its last token took the newline). */
static int
plan_end(
	const struct plan_reader *reader)
{
	/* A token more than the grammar has. */
	if (!reader->ended)
		return EINVAL;

	/* Succeeded: the request ended where it should. */
	return 0;
}

/*
 * Reports whether a token may name a device or a unit: letters, digits,
 * '-', '_', '.' and '+', not starting with '.' or '-'.
 */
static int
plan_token_valid(
	const char *token)
{
	size_t i;
	char c;

	/* A name that could be read as an option or a hidden file. */
	if (token[0] == '\0' || token[0] == '.' || token[0] == '-')
		return 0;

	/* Each character. */
	for (i = 0; token[i] != '\0'; i++) {
		c = token[i];
		if ((c >= 'a' && c <= 'z') ||
		    (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') ||
		    c == '-' ||
		    c == '_' ||
		    c == '.' ||
		    c == '+')
			continue;
		return 0;
	}

	/* Allowed. */
	return 1;
}

/*
 * Reports whether sectors lie on the disk clear of its partition tables:
 * from 1 MiB on, ending 1 MiB before its end at the latest.
 */
static int
plan_extent_inside(
	const struct plan_disk *disk,
	uint64_t start,
	uint64_t sectors)
{
	uint64_t margin;

	/* The sectors of 1 MiB. */
	margin = PLAN_ALIGN_BYTES / disk->sector_bytes;

	/* Not empty, not before the first 1 MiB, not into the last. */
	if (sectors == 0U || start < margin)
		return 0;
	if (disk->sectors < margin || start > disk->sectors - margin)
		return 0;
	if (sectors > disk->sectors - margin - start)
		return 0;

	/* On the disk. */
	return 1;
}

/* Checks a new partition's extent: on the disk and on the 1 MiB grid. */
static int
plan_extent_aligned(
	const struct plan_disk *disk,
	const struct plan_extent *extent)
{
	uint64_t grid;
	int inside;

	/* On the disk. */
	inside = plan_extent_inside(disk, extent->start, extent->sectors);
	if (!inside)
		return EINVAL;

	/* Its start and its size whole MiB. */
	grid = PLAN_ALIGN_BYTES / disk->sector_bytes;
	if (extent->start % grid != 0U || extent->sectors % grid != 0U)
		return EINVAL;

	/* Succeeded: the extent may be made. */
	return 0;
}

/* Reports whether two ranges of sectors share one. */
static int
plan_overlap(
	uint64_t start,
	uint64_t sectors,
	uint64_t other_start,
	uint64_t other_sectors)
{
	/* One ends before the other starts. */
	if (start + sectors <= other_start)
		return 0;
	if (other_start + other_sectors <= start)
		return 0;

	/* They overlap. */
	return 1;
}

/* Checks that a tuple names a partition of the request's disk, on the disk. */
static int
plan_tuple_check(
	const struct plan_disk *disk,
	const struct plan_tuple *tuple)
{
	/* Another disk's partition. */
	if (tuple->disk != disk->registration)
		return EINVAL;

	/* Not on the disk. */
	if (tuple->sectors == 0U || tuple->start >= disk->sectors)
		return EINVAL;
	if (tuple->sectors > disk->sectors - tuple->start)
		return EINVAL;

	/* Succeeded: the tuple may be the disk's. */
	return 0;
}

/*
 * Checks a whole-disk layout: an ESP of at least 512 MiB and a swap
 * partition (or none), both whole MiB, leaving the root at least 1 MiB
 * between the first and the last 1 MiB of the disk.  The root's size for
 * what is installed is the session's to check.
 */
static int
plan_whole_check(
	const struct plan_disk *disk,
	const struct install_plan *plan)
{
	uint64_t disk_bytes;
	uint64_t taken;

	/* The sizes, whole MiB, the ESP not smaller than a new one is made. */
	if (plan->whole_esp_bytes < PLAN_ESP_MIN_BYTES)
		return EINVAL;
	if (plan->whole_esp_bytes % PLAN_ALIGN_BYTES != 0U || plan->whole_swap_bytes % PLAN_ALIGN_BYTES != 0U)
		return EINVAL;

	/* The disk's size, which no size here may pass. */
	if (disk->sectors > UINT64_MAX / disk->sector_bytes)
		return EINVAL;
	disk_bytes = disk->sectors * disk->sector_bytes;
	if (plan->whole_esp_bytes > disk_bytes)
		return EINVAL;
	if (plan->whole_swap_bytes > disk_bytes - plan->whole_esp_bytes)
		return EINVAL;

	/* The tables' MiB at both ends and 1 MiB of root besides the ESP and the swap. */
	taken = plan->whole_esp_bytes + plan->whole_swap_bytes;
	if (disk_bytes - taken < PLAN_ALIGN_BYTES * 3U)
		return EINVAL;

	/* Succeeded: the layout fits. */
	return 0;
}

/*
 * Checks a chosen layout: the ESP, the root and the swap partition on the
 * disk, the root and the swap partition apart from each other and from
 * the ESP.
 */
static int
plan_chosen_check(
	const struct plan_disk *disk,
	const struct install_plan *plan)
{
	int overlapping;
	int error;

	/* The ESP and the root, on the disk and apart. */
	error = plan_tuple_check(disk, &plan->esp);
	if (error != 0)
		return error;
	error = plan_tuple_check(disk, &plan->root);
	if (error != 0)
		return error;
	overlapping = plan_overlap(plan->esp.start, plan->esp.sectors, plan->root.start, plan->root.sectors);
	if (overlapping)
		return EINVAL;

	/* A swap file needs no partition. */
	if (!plan->has_swap_partition)
		return 0;

	/* The swap partition, on the disk and apart from both. */
	error = plan_tuple_check(disk, &plan->swap);
	if (error != 0)
		return error;
	overlapping = plan_overlap(plan->swap.start, plan->swap.sectors, plan->esp.start, plan->esp.sectors);
	if (overlapping)
		return EINVAL;
	overlapping = plan_overlap(plan->swap.start, plan->swap.sectors, plan->root.start, plan->root.sectors);
	if (overlapping)
		return EINVAL;

	/* Succeeded: the partitions may be used. */
	return 0;
}

/* Appends one line (a text and an optional value) to the cfg's text. */
static int
plan_append(
	char *text,
	size_t capacity,
	size_t *used,
	const char *line,
	const char *value)
{
	int written;

	/* The line, refused when it would not fit. */
	if (value == NULL)
		value = "";
	written = snprintf(text + *used, capacity - *used, "%s%s\n", line, value);
	if (written < 0 || (size_t)written >= capacity - *used)
		return ERANGE;

	/* Succeeded: the line is added. */
	*used += (size_t)written;
	return 0;
}
