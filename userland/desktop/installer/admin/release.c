/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The release the helper installs from (release.h): its settings and the
 * choice of the asset from the API's answer.
 *
 * The answer is JSON (RFC 8259).  It is read once, front to back, without
 * building a tree: the values the choice needs are copied out as they pass
 * and every other value is passed over, nested no deeper than
 * RELEASE_DEPTH_MAX.  A string longer than its place is still read to its
 * end and then counts as not given; an escape of a character outside ASCII
 * becomes '?', for no name, tag or address the choice compares has one.
 */

#include "release.h"

#include <errno.h>
#include <string.h>

/* The deepest nesting of the answer the helper reads. */
#define RELEASE_DEPTH_MAX	32U

/* The longest digest field ("sha256:" and 64 hex digits, with its NUL). */
#define RELEASE_DIGEST_MAX	80U

/* The settings' longest line and value. */
#define RELEASE_LINE_MAX	(RELEASE_URL_MAX + 32U)

/* The asset of the system for the machine the helper runs on. */
#if defined(__x86_64__)
#define RELEASE_DEFAULT_ASSET	"zedbsd-system-x86_64.tar"
#elif defined(__aarch64__)
#define RELEASE_DEFAULT_ASSET	"zedbsd-system-aarch64.tar"
#elif defined(__riscv)
#define RELEASE_DEFAULT_ASSET	"zedbsd-system-riscv64.tar"
#else
#define RELEASE_DEFAULT_ASSET	"zedbsd-system.tar"
#endif

/* The reader of the answer: the text, the place reached and the nesting. */
struct release_json {
	const char *text;
	size_t length;
	size_t position;
	unsigned depth;
};

/* What one release of the answer said, as far as the choice needs. */
struct release_seen {
	int draft;
	int prerelease;
	char tag[RELEASE_TAG_MAX];
	char published[32];
	int found;
	struct release_asset asset;
	char digest[RELEASE_DIGEST_MAX];
};

/* What one asset of a release said. */
struct release_item {
	char name[RELEASE_NAME_MAX];
	char url[RELEASE_URL_MAX];
	uint64_t size;
	int has_size;
	char digest[RELEASE_DIGEST_MAX];
};

/* What a walk of the releases hands each release to, and the place it collects in. */
typedef int (*release_visit)(void *context, struct release_seen *seen);

/* The releases a list collects. */
struct release_collect {
	struct release_asset *list;
	unsigned capacity;
	unsigned count;
};

/* The release a choice looks for, and the asset it found. */
struct release_find {
	const char *tag;
	struct release_asset *asset;
	int found;
};

static int release_walk(const char *text, size_t length, const struct release_config *config, release_visit visit, void *context);
static int release_collect_one(void *context, struct release_seen *seen);
static int release_find_one(void *context, struct release_seen *seen);
static int release_config_line(char *line, struct release_config *config, unsigned *seen);
static int release_word_valid(const char *word, int path);
static void release_space(struct release_json *json);
static int release_peek(struct release_json *json, char *character);
static int release_expect(struct release_json *json, char character);
static int release_string(struct release_json *json, char *target, size_t capacity, int *fits);
static int release_number(struct release_json *json, uint64_t *value, int *integer);
static int release_literal(struct release_json *json, const char *word);
static int release_skip(struct release_json *json);
static int release_skip_array(struct release_json *json);
static int release_skip_object(struct release_json *json);
static int release_one(struct release_json *json, const struct release_config *config, struct release_seen *seen);
static int release_assets(struct release_json *json, const struct release_config *config, struct release_seen *seen);
static int release_asset_item(struct release_json *json, struct release_item *item);
static int release_digest(const char *text, uint8_t digest[32]);
static int release_hex(char character, unsigned *value);
static int release_named(const char *name, int fits, const char *word);
static int release_take(struct release_seen *seen, struct release_asset *asset);

/* Fills the settings with the defaults: the project's releases, the newest with the asset, no extra CA. */
void
release_config_default(
	struct release_config *config)
{
	/* The defaults. */
	memset(config, 0, sizeof(*config));
	(void)strncpy(config->api, RELEASE_DEFAULT_API, sizeof(config->api) - 1U);
	(void)strncpy(config->asset, RELEASE_DEFAULT_ASSET, sizeof(config->asset) - 1U);
}

/*
 * Reads the settings' text over the defaults.  A line is "KEY VALUE",
 * blank lines and lines starting with '#' passed over.  An unknown key, a
 * key given twice, a value with a space or a control character in it, an
 * address that is not https or a CA file that is not an absolute path is
 * refused with EINVAL.
 */
int
release_config_parse(
	const char *text,
	size_t length,
	struct release_config *config)
{
	char line[RELEASE_LINE_MAX];
	unsigned seen;
	size_t used;
	size_t i;
	int error;

	/* Each line, cut at its newline; a line too long is refused. */
	seen = 0;
	used = 0;
	for (i = 0; i <= length; i++) {
		/* The end of a line (or of the text) hands the line on. */
		if (i == length || text[i] == '\n') {
			line[used] = '\0';
			error = release_config_line(line, config, &seen);
			if (error != 0)
				return error;
			used = 0;
			continue;
		}

		/* A NUL or a carriage return is not text. */
		if (text[i] == '\0' || text[i] == '\r')
			return EINVAL;
		if (used + 1U >= sizeof(line))
			return EINVAL;
		line[used] = text[i];
		used++;
	}

	/* Succeeded: the settings hold the file's values over the defaults. */
	return 0;
}

/*
 * Reports whether an address may be fetched (0, or EINVAL): https, a host,
 * shorter than RELEASE_URL_MAX, and only visible ASCII.
 */
int
release_url_valid(
	const char *url)
{
	size_t length;
	size_t i;
	int differs;

	/* An https address with a host after its scheme. */
	differs = strncmp(url, "https://", 8U);
	if (differs != 0)
		return EINVAL;
	length = strlen(url);
	if (length <= 8U || length >= RELEASE_URL_MAX)
		return EINVAL;
	if (url[8] == '/' || url[8] == ':')
		return EINVAL;

	/* Visible ASCII only: no space, no control character, nothing beyond ASCII. */
	for (i = 0; i < length; i++) {
		if ((unsigned char)url[i] <= 0x20U || (unsigned char)url[i] >= 0x7fU)
			return EINVAL;
	}

	/* The address may be fetched. */
	return 0;
}

/*
 * Reports whether a tag may be offered and passed back as a word (0, or
 * EINVAL): letters, digits, '.', '_', '-' and '+', not starting with '.'
 * or '-', shorter than RELEASE_TAG_MAX.
 */
int
release_tag_valid(
	const char *tag)
{
	size_t length;
	int error;

	/* A word of the settings' kind, not an option, of a length the tag's place holds. */
	error = release_word_valid(tag, 0);
	if (error != 0)
		return error;
	if (tag[0] == '-')
		return EINVAL;
	length = strlen(tag);
	if (length >= RELEASE_TAG_MAX)
		return EINVAL;

	/* The tag may be offered. */
	return 0;
}

/*
 * Lists the published releases of the API's answer (one release, or a
 * list of them newest first) that have the asset: at most capacity of
 * them, in the answer's order.  Returns 0 with their count (0 when there
 * is none), or EINVAL for an answer that is not JSON of that form.
 */
int
release_list(
	const char *text,
	size_t length,
	const struct release_config *config,
	struct release_asset *list,
	unsigned capacity,
	unsigned *count)
{
	struct release_collect collect;
	int error;

	/* Each release, the ones that fit collected. */
	collect.list = list;
	collect.capacity = capacity;
	collect.count = 0;
	*count = 0;
	error = release_walk(text, length, config, release_collect_one, &collect);
	if (error != 0)
		return error;

	/* Succeeded: the releases offered. */
	*count = collect.count;
	return 0;
}

/*
 * Chooses the asset of the published release of a tag from the API's
 * answer.  Returns 0 with the asset, ENOENT when no published release of
 * that tag has the asset, or EINVAL for an answer that is not JSON of
 * that form.
 */
int
release_choose(
	const char *text,
	size_t length,
	const struct release_config *config,
	const char *tag,
	struct release_asset *asset)
{
	struct release_find find;
	int error;

	/* Each release until the one of the tag. */
	memset(asset, 0, sizeof(*asset));
	find.tag = tag;
	find.asset = asset;
	find.found = 0;
	error = release_walk(text, length, config, release_find_one, &find);
	if (error != 0 && error != EEXIST)
		return error;
	if (!find.found)
		return ENOENT;

	/* Succeeded: the asset is chosen. */
	return 0;
}

/*
 * Reads the API's answer, one release or a list of them, and hands each
 * release read whole to visit.  A result of visit other than 0 stops the
 * walk with that result.  EINVAL for an answer that is not JSON of that
 * form.
 */
static int
release_walk(
	const char *text,
	size_t length,
	const struct release_config *config,
	release_visit visit,
	void *context)
{
	struct release_json json;
	struct release_seen seen;
	char character;
	int error;

	/* The reader, at the first value. */
	json.text = text;
	json.length = length;
	json.position = 0;
	json.depth = 0;
	release_space(&json);
	error = release_peek(&json, &character);
	if (error != 0)
		return error;

	/* One release. */
	if (character == '{') {
		error = release_one(&json, config, &seen);
		if (error != 0)
			return error;
		error = visit(context, &seen);
		return error;
	}

	/* Anything else than a release or a list of them is not the API's answer. */
	if (character != '[')
		return EINVAL;

	/* A list: an empty one, or each release in order. */
	json.position++;
	release_space(&json);
	error = release_peek(&json, &character);
	if (error != 0)
		return error;
	if (character == ']')
		return 0;
	for (;;) {
		/* A release, handed on once it is read. */
		release_space(&json);
		error = release_peek(&json, &character);
		if (error != 0)
			return error;
		if (character != '{')
			return EINVAL;
		error = release_one(&json, config, &seen);
		if (error != 0)
			return error;
		error = visit(context, &seen);
		if (error != 0)
			return error;

		/* The next release, or the end of the list. */
		release_space(&json);
		error = release_peek(&json, &character);
		if (error != 0)
			return error;
		json.position++;
		if (character == ']')
			break;
		if (character != ',')
			return EINVAL;
	}

	/* Succeeded: every release was handed on. */
	return 0;
}

/* Collects a release that fits, while there is room. */
static int
release_collect_one(
	void *context,
	struct release_seen *seen)
{
	struct release_collect *collect;
	struct release_asset asset;
	int error;

	/* A release that does not fit, or no room left, is passed over. */
	collect = context;
	error = release_take(seen, &asset);
	if (error != 0)
		return 0;
	if (collect->count == collect->capacity)
		return 0;

	/* Collected. */
	collect->list[collect->count] = asset;
	collect->count++;
	return 0;
}

/* Takes the release of the tag looked for, when it fits; EEXIST stops the walk there. */
static int
release_find_one(
	void *context,
	struct release_seen *seen)
{
	struct release_find *find;
	int differs;
	int error;

	/* Another tag is passed over. */
	find = context;
	differs = strcmp(seen->tag, find->tag);
	if (differs != 0)
		return 0;

	/* The tag's release, when it fits. */
	error = release_take(seen, find->asset);
	if (error != 0)
		return 0;
	find->found = 1;
	return EEXIST;
}

/* Takes one line of the settings: a key and its value. */
static int
release_config_line(
	char *line,
	struct release_config *config,
	unsigned *seen)
{
	char *key;
	char *value;
	char *end;
	unsigned bit;
	size_t length;
	int ca_file;
	int asset;
	int api;
	int error;

	/* The line without its leading and trailing blanks; blank and comment lines pass. */
	key = line;
	while (*key == ' ' || *key == '\t')
		key++;
	length = strlen(key);
	while (length > 0U && (key[length - 1U] == ' ' || key[length - 1U] == '\t')) {
		key[length - 1U] = '\0';
		length--;
	}
	if (length == 0U || key[0] == '#')
		return 0;

	/* The key, then one value after the blanks. */
	end = key + strcspn(key, " \t");
	if (*end == '\0')
		return EINVAL;
	*end = '\0';
	value = end + 1;
	while (*value == ' ' || *value == '\t')
		value++;
	length = strlen(value);

	/* Compares the key with each one the settings know. */
	api = strcmp(key, "api");
	asset = strcmp(key, "asset");
	ca_file = strcmp(key, "ca-file");
	if (api == 0) {
		bit = 0x1U;
		error = release_url_valid(value);
		if (error != 0 || length >= sizeof(config->api))
			return EINVAL;
		(void)memcpy(config->api, value, length + 1U);
	} else if (asset == 0) {
		bit = 0x4U;
		error = release_word_valid(value, 0);
		if (error != 0 || length >= sizeof(config->asset))
			return EINVAL;
		(void)memcpy(config->asset, value, length + 1U);
	} else if (ca_file == 0) {
		bit = 0x8U;
		error = release_word_valid(value, 1);
		if (error != 0 || length >= sizeof(config->ca_file))
			return EINVAL;
		(void)memcpy(config->ca_file, value, length + 1U);
	} else {
		/* A key the settings do not know. */
		return EINVAL;
	}

	/* A key given twice is refused. */
	if ((*seen & bit) != 0U)
		return EINVAL;
	*seen |= bit;

	/* Succeeded: the value is taken. */
	return 0;
}

/*
 * Reports whether a word of the settings may be taken (0, or EINVAL): a
 * tag or a name of letters, digits, '.', '_', '-' and '+', or (path) an
 * absolute path of those and '/', without a "." or ".." component.
 */
static int
release_word_valid(
	const char *word,
	int path)
{
	size_t i;
	char character;

	/* Not empty; a path starts at the root. */
	if (word[0] == '\0')
		return EINVAL;
	if (path && word[0] != '/')
		return EINVAL;
	if (!path && word[0] == '.')
		return EINVAL;

	/* Each character is one the word may have. */
	for (i = 0; word[i] != '\0'; i++) {
		character = word[i];
		if (character >= 'a' && character <= 'z')
			continue;
		if (character >= 'A' && character <= 'Z')
			continue;
		if (character >= '0' && character <= '9')
			continue;
		if (character == '.' || character == '_' || character == '-' || character == '+')
			continue;
		if (path && character == '/')
			continue;
		return EINVAL;
	}

	/* A path has no "." or ".." component (the component's end is a slash or the end). */
	if (path) {
		for (i = 0; word[i] != '\0'; i++) {
			if (word[i] != '/' || word[i + 1U] != '.')
				continue;
			if (word[i + 2U] == '/' || word[i + 2U] == '\0')
				return EINVAL;
			if (word[i + 2U] == '.' && (word[i + 3U] == '/' || word[i + 3U] == '\0'))
				return EINVAL;
		}
	}

	/* The word may be taken. */
	return 0;
}

/* Passes over the blanks JSON allows between tokens. */
static void
release_space(
	struct release_json *json)
{
	char character;

	/* Space, tab, newline and carriage return. */
	while (json->position < json->length) {
		character = json->text[json->position];
		if (character != ' ' && character != '\t' && character != '\n' && character != '\r')
			return;
		json->position++;
	}
}

/* Gives the character at the reader's place; EINVAL at the end of the text. */
static int
release_peek(
	struct release_json *json,
	char *character)
{
	/* The text ends before the value does. */
	if (json->position >= json->length)
		return EINVAL;

	/* The character there. */
	*character = json->text[json->position];
	return 0;
}

/* Takes one character the grammar requires there, after any blanks; EINVAL for another. */
static int
release_expect(
	struct release_json *json,
	char character)
{
	char found;
	int error;

	/* The next character after the blanks. */
	release_space(json);
	error = release_peek(json, &found);
	if (error != 0)
		return error;
	if (found != character)
		return EINVAL;

	/* Taken. */
	json->position++;
	return 0;
}

/*
 * Reads a string at the reader's place into target (when not NULL), its
 * escapes undone.  fits says whether the whole string had room; a string
 * without room is still read to its end.
 */
static int
release_string(
	struct release_json *json,
	char *target,
	size_t capacity,
	int *fits)
{
	unsigned code;
	unsigned digit;
	unsigned i;
	size_t used;
	char character;
	int error;

	/* The opening quote. */
	*fits = 1;
	used = 0;
	error = release_expect(json, '"');
	if (error != 0)
		return error;

	/* Each character to the closing quote. */
	for (;;) {
		error = release_peek(json, &character);
		if (error != 0)
			return error;
		json->position++;
		if (character == '"')
			break;

		/* A control character must be escaped. */
		if ((unsigned char)character < 0x20U)
			return EINVAL;

		/* An escape: one of JSON's, or a code unit in hex. */
		if (character == '\\') {
			error = release_peek(json, &character);
			if (error != 0)
				return error;
			json->position++;

			/* The escape's character. */
			switch (character) {
			case '"':
			case '\\':
			case '/':
				break;
			case 'b':
				character = '\b';
				break;
			case 'f':
				character = '\f';
				break;
			case 'n':
				character = '\n';
				break;
			case 'r':
				character = '\r';
				break;
			case 't':
				character = '\t';
				break;
			case 'u':
				code = 0;
				for (i = 0; i < 4U; i++) {
					error = release_peek(json, &character);
					if (error != 0)
						return error;
					error = release_hex(character, &digit);
					if (error != 0)
						return error;
					json->position++;
					code = (code << 4) | digit;
				}

				/* ASCII stays; anything beyond is not a character the choice compares. */
				character = '?';
				if (code < 0x80U)
					character = (char)code;
				break;
			default:
				return EINVAL;
			}
		}

		/* Kept while there is room (one byte stays for the NUL). */
		if (target != NULL && used + 1U < capacity) {
			target[used] = character;
			used++;
		} else {
			*fits = 0;
		}
	}

	/* The string's end. */
	if (target != NULL && capacity > 0U)
		target[used] = '\0';

	/* Succeeded: the string is read. */
	return 0;
}

/*
 * Reads a number at the reader's place.  integer says whether it was a
 * whole number of at most 19 digits without a sign, which value then holds.
 */
static int
release_number(
	struct release_json *json,
	uint64_t *value,
	int *integer)
{
	size_t start;
	size_t digits;
	char character;

	/* The characters a number may have, to its end. */
	*value = 0;
	*integer = 1;
	digits = 0;
	start = json->position;
	while (json->position < json->length) {
		character = json->text[json->position];
		if (character >= '0' && character <= '9') {
			if (digits < 19U)
				*value = *value * 10U + (uint64_t)(character - '0');
			digits++;
		} else if (character == '-' || character == '+' || character == '.' || character == 'e' || character == 'E') {
			*integer = 0;
		} else {
			break;
		}

		/* The next character of the number. */
		json->position++;
	}

	/* A number has a digit; a whole one has at most 19. */
	if (json->position == start || digits == 0U)
		return EINVAL;
	if (digits > 19U)
		*integer = 0;

	/* Succeeded: the number is read. */
	return 0;
}

/* Reads one of the words true, false and null at the reader's place. */
static int
release_literal(
	struct release_json *json,
	const char *word)
{
	size_t length;
	int differs;

	/* The word, whole. */
	length = strlen(word);
	if (json->length - json->position < length)
		return EINVAL;
	differs = memcmp(json->text + json->position, word, length);
	if (differs != 0)
		return EINVAL;

	/* Taken. */
	json->position += length;
	return 0;
}

/* Passes over one value of any kind at the reader's place. */
static int
release_skip(
	struct release_json *json)
{
	uint64_t number;
	char character;
	int integer;
	int fits;
	int error;

	/* By the value's first character. */
	release_space(json);
	error = release_peek(json, &character);
	if (error != 0)
		return error;
	switch (character) {
	case '{':
		return release_skip_object(json);
	case '[':
		return release_skip_array(json);
	case '"':
		return release_string(json, NULL, 0, &fits);
	case 't':
		return release_literal(json, "true");
	case 'f':
		return release_literal(json, "false");
	case 'n':
		return release_literal(json, "null");
	default:
		break;
	}

	/* What is left is a number. */
	if (character != '-' && (character < '0' || character > '9'))
		return EINVAL;
	error = release_number(json, &number, &integer);
	return error;
}

/* Passes over an array, no deeper than the limit. */
static int
release_skip_array(
	struct release_json *json)
{
	char character;
	int error;

	/* One level deeper. */
	if (json->depth >= RELEASE_DEPTH_MAX)
		return EINVAL;
	json->depth++;
	json->position++;

	/* An empty array. */
	release_space(json);
	error = release_peek(json, &character);
	if (error != 0)
		return error;
	if (character == ']') {
		json->position++;
		json->depth--;
		return 0;
	}

	/* Each value, then a comma or the end. */
	for (;;) {
		error = release_skip(json);
		if (error != 0)
			return error;
		release_space(json);
		error = release_peek(json, &character);
		if (error != 0)
			return error;
		json->position++;
		if (character == ']')
			break;
		if (character != ',')
			return EINVAL;
	}

	/* Back to the level above. */
	json->depth--;
	return 0;
}

/* Passes over an object, no deeper than the limit. */
static int
release_skip_object(
	struct release_json *json)
{
	char character;
	int fits;
	int error;

	/* One level deeper. */
	if (json->depth >= RELEASE_DEPTH_MAX)
		return EINVAL;
	json->depth++;
	json->position++;

	/* An empty object. */
	release_space(json);
	error = release_peek(json, &character);
	if (error != 0)
		return error;
	if (character == '}') {
		json->position++;
		json->depth--;
		return 0;
	}

	/* Each member: its name, a colon and its value, then a comma or the end. */
	for (;;) {
		error = release_string(json, NULL, 0, &fits);
		if (error != 0)
			return error;
		error = release_expect(json, ':');
		if (error != 0)
			return error;
		error = release_skip(json);
		if (error != 0)
			return error;
		release_space(json);
		error = release_peek(json, &character);
		if (error != 0)
			return error;
		json->position++;
		if (character == '}')
			break;
		if (character != ',')
			return EINVAL;
		release_space(json);
	}

	/* Back to the level above. */
	json->depth--;
	return 0;
}

/*
 * Reads one release object: whether it is a draft, its tag and, of its
 * assets, the first of the asked name.
 */
static int
release_one(
	struct release_json *json,
	const struct release_config *config,
	struct release_seen *seen)
{
	char name[32];
	char character;
	int *flag;
	int is_prerelease;
	int is_published;
	int is_assets;
	int is_draft;
	int is_tag;
	int fits;
	int error;

	/* Nothing seen yet; the object's level. */
	memset(seen, 0, sizeof(*seen));
	if (json->depth >= RELEASE_DEPTH_MAX)
		return EINVAL;
	json->depth++;
	error = release_expect(json, '{');
	if (error != 0)
		return error;

	/* An empty object is a release with nothing. */
	release_space(json);
	error = release_peek(json, &character);
	if (error != 0)
		return error;
	if (character == '}') {
		json->position++;
		json->depth--;
		return 0;
	}

	/* Each member, the ones the choice needs read, the others passed over. */
	for (;;) {
		error = release_string(json, name, sizeof(name), &fits);
		if (error != 0)
			return error;
		error = release_expect(json, ':');
		if (error != 0)
			return error;
		release_space(json);
		error = release_peek(json, &character);
		if (error != 0)
			return error;

		/* By the member's name. */
		is_draft = release_named(name, fits, "draft");
		is_prerelease = release_named(name, fits, "prerelease");
		is_published = release_named(name, fits, "published_at");
		is_tag = release_named(name, fits, "tag_name");
		is_assets = release_named(name, fits, "assets");
		if (is_draft || is_prerelease) {
			flag = &seen->draft;
			if (is_prerelease)
				flag = &seen->prerelease;
			error = release_peek(json, &character);
			if (error != 0)
				return error;
			*flag = 1;
			if (character == 'f') {
				*flag = 0;
				error = release_literal(json, "false");
			} else {
				error = release_literal(json, "true");
			}
		} else if (is_published && character == '"') {
			error = release_string(json, seen->published, sizeof(seen->published), &fits);
			if (error == 0 && !fits)
				seen->published[0] = '\0';
		} else if (is_tag) {
			error = release_string(json, seen->tag, sizeof(seen->tag), &fits);
			if (error == 0 && !fits)
				seen->tag[0] = '\0';
		} else if (is_assets) {
			error = release_assets(json, config, seen);
		} else {
			error = release_skip(json);
		}
		if (error != 0)
			return error;

		/* A comma, or the end of the release. */
		release_space(json);
		error = release_peek(json, &character);
		if (error != 0)
			return error;
		json->position++;
		if (character == '}')
			break;
		if (character != ',')
			return EINVAL;
		release_space(json);
	}

	/* Back to the level above. */
	json->depth--;
	return 0;
}

/* Reads a release's array of assets, keeping the first of the asked name. */
static int
release_assets(
	struct release_json *json,
	const struct release_config *config,
	struct release_seen *seen)
{
	struct release_item item;
	char character;
	int same;
	int error;

	/* One level deeper. */
	if (json->depth >= RELEASE_DEPTH_MAX)
		return EINVAL;
	json->depth++;
	error = release_expect(json, '[');
	if (error != 0)
		return error;

	/* An empty array. */
	release_space(json);
	error = release_peek(json, &character);
	if (error != 0)
		return error;
	if (character == ']') {
		json->position++;
		json->depth--;
		return 0;
	}

	/* Each asset, then a comma or the end. */
	for (;;) {
		error = release_asset_item(json, &item);
		if (error != 0)
			return error;

		/* The first asset of the asked name is kept. */
		same = strcmp(item.name, config->asset);
		if (same == 0 && !seen->found) {
			seen->found = 1;
			(void)memcpy(seen->asset.name, item.name, sizeof(seen->asset.name));
			(void)memcpy(seen->asset.url, item.url, sizeof(seen->asset.url));
			(void)memcpy(seen->digest, item.digest, sizeof(seen->digest));
			seen->asset.size = 0;
			if (item.has_size)
				seen->asset.size = item.size;
		}

		/* A comma, or the end of the assets. */
		release_space(json);
		error = release_peek(json, &character);
		if (error != 0)
			return error;
		json->position++;
		if (character == ']')
			break;
		if (character != ',')
			return EINVAL;
	}

	/* Back to the level above. */
	json->depth--;
	return 0;
}

/* Reads one asset object: its name, address, size and digest. */
static int
release_asset_item(
	struct release_json *json,
	struct release_item *item)
{
	char name[32];
	char character;
	int is_digest;
	int is_name;
	int is_size;
	int is_url;
	int integer;
	int fits;
	int error;

	/* Nothing seen yet; the object's level. */
	memset(item, 0, sizeof(*item));
	if (json->depth >= RELEASE_DEPTH_MAX)
		return EINVAL;
	json->depth++;
	error = release_expect(json, '{');
	if (error != 0)
		return error;

	/* An empty object is an asset with nothing. */
	release_space(json);
	error = release_peek(json, &character);
	if (error != 0)
		return error;
	if (character == '}') {
		json->position++;
		json->depth--;
		return 0;
	}

	/* Each member, the ones the choice needs read, the others passed over. */
	for (;;) {
		error = release_string(json, name, sizeof(name), &fits);
		if (error != 0)
			return error;
		error = release_expect(json, ':');
		if (error != 0)
			return error;
		release_space(json);
		error = release_peek(json, &character);
		if (error != 0)
			return error;

		/* By the member's name; a value of another kind than expected counts as not given. */
		is_name = release_named(name, fits, "name");
		is_url = release_named(name, fits, "browser_download_url");
		is_digest = release_named(name, fits, "digest");
		is_size = release_named(name, fits, "size");
		if (is_name && character == '"') {
			error = release_string(json, item->name, sizeof(item->name), &fits);
			if (error == 0 && !fits)
				item->name[0] = '\0';
		} else if (is_url && character == '"') {
			error = release_string(json, item->url, sizeof(item->url), &fits);
			if (error == 0 && !fits)
				item->url[0] = '\0';
		} else if (is_digest && character == '"') {
			error = release_string(json, item->digest, sizeof(item->digest), &fits);
			if (error == 0 && !fits)
				item->digest[0] = '\0';
		} else if (is_size && character >= '0' && character <= '9') {
			error = release_number(json, &item->size, &integer);
			if (error == 0 && integer)
				item->has_size = 1;
		} else {
			error = release_skip(json);
		}
		if (error != 0)
			return error;

		/* A comma, or the end of the asset. */
		release_space(json);
		error = release_peek(json, &character);
		if (error != 0)
			return error;
		json->position++;
		if (character == '}')
			break;
		if (character != ',')
			return EINVAL;
		release_space(json);
	}

	/* Back to the level above. */
	json->depth--;
	return 0;
}

/* Reads a digest field "sha256:" and 64 hex digits; EINVAL for anything else. */
static int
release_digest(
	const char *text,
	uint8_t digest[32])
{
	unsigned high;
	unsigned low;
	unsigned i;
	int differs;
	int error;

	/* The algorithm, then exactly 64 digits. */
	differs = strncmp(text, "sha256:", 7U);
	if (differs != 0)
		return EINVAL;
	text += 7;
	if (strlen(text) != 64U)
		return EINVAL;

	/* Each byte from its two digits. */
	for (i = 0; i < 32U; i++) {
		error = release_hex(text[i * 2U], &high);
		if (error != 0)
			return error;
		error = release_hex(text[i * 2U + 1U], &low);
		if (error != 0)
			return error;
		digest[i] = (uint8_t)((high << 4) | low);
	}

	/* Succeeded: the digest is read. */
	return 0;
}

/* Gives the value of a hex digit; EINVAL for another character. */
static int
release_hex(
	char character,
	unsigned *value)
{
	/* By the digit's range. */
	if (character >= '0' && character <= '9') {
		*value = (unsigned)(character - '0');
		return 0;
	}
	if (character >= 'a' && character <= 'f') {
		*value = (unsigned)(character - 'a') + 10U;
		return 0;
	}
	if (character >= 'A' && character <= 'F') {
		*value = (unsigned)(character - 'A') + 10U;
		return 0;
	}

	/* Not a hex digit. */
	return EINVAL;
}

/* Reports whether a member's name, read whole, is the word. */
static int
release_named(
	const char *name,
	int fits,
	const char *word)
{
	int differs;

	/* A name cut short is no name the choice knows. */
	if (!fits)
		return 0;
	differs = strcmp(name, word);
	if (differs != 0)
		return 0;

	/* The same name. */
	return 1;
}

/*
 * Takes the asset of a release read whole, when the release fits: not a
 * draft, not a pre-release, of a tag that may be passed as a word, with
 * the asset, which carries its SHA-256, an https address and a size the
 * helper takes.  Returns 0, or ENOENT when it does not fit.
 */
static int
release_take(
	struct release_seen *seen,
	struct release_asset *asset)
{
	int error;

	/* A draft, a pre-release, a tag that is no word, or no such asset does not fit. */
	if (seen->draft || seen->prerelease || !seen->found)
		return ENOENT;
	error = release_tag_valid(seen->tag);
	if (error != 0)
		return ENOENT;

	/* The asset must carry its SHA-256, an https address and a size the helper takes. */
	error = release_digest(seen->digest, seen->asset.sha256);
	if (error != 0)
		return ENOENT;
	error = release_url_valid(seen->asset.url);
	if (error != 0)
		return ENOENT;
	if (seen->asset.size == 0U || seen->asset.size > RELEASE_ASSET_MAX)
		return ENOENT;

	/* The asset, with its release's tag and date. */
	*asset = seen->asset;
	(void)memcpy(asset->tag, seen->tag, sizeof(asset->tag));
	(void)memcpy(asset->published, seen->published, sizeof(asset->published));
	return 0;
}
