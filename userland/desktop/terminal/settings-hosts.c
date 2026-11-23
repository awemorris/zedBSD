/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ssh_config of the settings tab's Hosts pane, read into lines and
 * blocks and changed a line at a time (settings-hosts.h).
 *
 * The lines are read as the SSH library reads them: a line ends at an LF
 * (a CR before it is a blank), a keyword is followed by blanks or an '='
 * with blanks round it, the words of a value are bare or in double quotes,
 * and a word that starts with '#' ends the line as a comment.  A Host or
 * a Match line starts a block that runs to the next one.  Which Host
 * blocks apply to a host follows ssh_config(5)'s Host and PATTERNS: a
 * block applies when one of its patterns matches the name and no negated
 * one ('!') does; '*' stands for any run of characters and '?' for one,
 * the case of letters aside.  The first block that gives a keyword of one
 * value wins; a keyword of a list (IdentityFile, the forwards) takes the
 * lines of every block that applies.
 *
 * Procedure referenced: ssh_config(5) of OpenSSH (man.openbsd.org,
 * 2026-11-23): "the first obtained value will be used", Host, PATTERNS.
 */

#include "settings-hosts.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most changes of one text made at once (each line of a Host's block taken out is one). */
#define HOSTS_SPANS_MAX		4096U

/* The most rounds a check is made again with a problem the text had before set aside. */
#define HOSTS_CHECK_ROUNDS	256U

/* The indent of a line added to a Host's block when no line of the file shows its own. */
#define HOSTS_INDENT		"    "

/* The longest indent copied from a line of the file, with its NUL. */
#define HOSTS_INDENT_MAX	64U

/* The longest line break (CR LF), with its NUL. */
#define HOSTS_BREAK_MAX		3U

/* The highest port number. */
#define HOSTS_PORT_MAX		65535UL

/*
 * One change of a text: the bytes from from to one before to give way to
 * insert (insert_size bytes; none for a removal).  A change never starts
 * before the one before it ends.
 */
struct hosts_span {
	size_t from;
	size_t to;
	const char *insert;
	size_t insert_size;
};

static void hosts_line_read(const char *text, size_t size, size_t start, struct terminal_hosts_line *line);
static size_t hosts_value_end(const char *text, size_t value, size_t end);
static int hosts_next_word(const char *text, size_t *at, size_t end, size_t *word, size_t *word_end);
static size_t hosts_skip_blanks(const char *text, size_t at, size_t end);
static size_t hosts_skip_word(const char *text, size_t at, size_t end);
static int hosts_blank(char byte);
static char hosts_lower(char byte);
static int hosts_equal(const char *text, size_t length, const char *word);
static int hosts_concrete(const char *word, size_t length);
static int hosts_applies_to_block(const struct terminal_hosts *hosts, unsigned other, unsigned block);
static int hosts_word_bad(const char *value, size_t length);
static int hosts_port(const char *text, size_t length, int zero);
static int hosts_endpoint(const char *text, int need_host, int zero, const char **why);
static unsigned hosts_anchor(const struct terminal_hosts *hosts, unsigned block, const char *keyword);
static void hosts_break(const struct terminal_hosts *hosts, unsigned line, char *text);
static void hosts_indent(const struct terminal_hosts *hosts, unsigned block, char *indent);
static int hosts_splice(const char *text, size_t size, const struct hosts_span *spans, unsigned count, char **out, size_t *out_size);
static int hosts_problem_line(const char *report, unsigned *number);
static int hosts_line_was(const char *before, size_t before_size, const char *line, size_t length);

/*
 * Reads a text into its lines and blocks (hosts points into text, which
 * must stay while hosts is used).  Returns 0, or ENOMEM.
 */
int
terminal_hosts_read(
	struct terminal_hosts *hosts,
	const char *text,
	size_t size)
{
	struct terminal_hosts_line *line;
	unsigned count;
	unsigned index;
	unsigned block;
	size_t at;

	/* Nothing read yet. */
	memset(hosts, 0, sizeof(*hosts));
	hosts->text = text;
	hosts->size = size;

	/* The lines: one for each LF. */
	count = 0U;
	for (at = 0U; at < size; at++) {
		if (text[at] == '\n')
			count++;
	}

	/* The words after the last line break are a line too. */
	if (size > 0U && text[size - 1U] != '\n')
		count++;

	/* The lines' table. */
	hosts->lines = calloc((size_t)count + 1U, sizeof(*hosts->lines));
	if (hosts->lines == NULL)
		return ENOMEM;

	/* The blocks' table: one for each Host or Match line, and the part before them. */
	hosts->blocks = calloc((size_t)count + 1U, sizeof(*hosts->blocks));
	if (hosts->blocks == NULL) {
		free(hosts->lines);
		hosts->lines = NULL;
		return ENOMEM;
	}

	/* The part before the first Host or Match. */
	hosts->blocks[0].kind = TERMINAL_HOSTS_BLOCK_BEFORE;
	hosts->blocks[0].first = 0U;
	hosts->block_count = 1U;

	/* Each line, a Host or a Match line starting the next block. */
	block = 0U;
	at = 0U;
	for (index = 0U; index < count; index++) {
		line = &hosts->lines[index];
		hosts_line_read(text, size, at, line);
		line->number = index + 1U;
		at = line->next;

		/* A Host or a Match line ends the block before and starts its own. */
		if (line->kind == TERMINAL_HOSTS_LINE_HOST ||
		    line->kind == TERMINAL_HOSTS_LINE_MATCH) {
			hosts->blocks[block].end = index;
			block = hosts->block_count;
			hosts->blocks[block].kind = TERMINAL_HOSTS_BLOCK_HOST;
			if (line->kind == TERMINAL_HOSTS_LINE_MATCH)
				hosts->blocks[block].kind = TERMINAL_HOSTS_BLOCK_MATCH;
			hosts->blocks[block].first = index;
			hosts->block_count++;
		}

		/* The line is in the block open now. */
		line->block = block;
	}

	/* The last block runs to the end. */
	hosts->blocks[block].end = count;
	hosts->line_count = count;

	/* Succeeded: the text's lines and blocks. */
	return 0;
}

/*
 * Frees the tables of a text read (the text itself is the caller's).
 */
void
terminal_hosts_release(
	struct terminal_hosts *hosts)
{
	/* The tables, and nothing pointed at. */
	free(hosts->lines);
	free(hosts->blocks);
	memset(hosts, 0, sizeof(*hosts));
}

/*
 * Tells whether a line is a keyword's (a Host or a Match line is one too)
 * with the keyword given, the case of letters aside.  Returns 1 or 0.
 */
int
terminal_hosts_keyword_is(
	const struct terminal_hosts *hosts,
	unsigned line,
	const char *keyword)
{
	const struct terminal_hosts_line *read;
	int same;

	/* A line that is there. */
	if (line >= hosts->line_count)
		return 0;
	read = &hosts->lines[line];

	/* Only a line with a keyword has one. */
	if (read->kind != TERMINAL_HOSTS_LINE_KEYWORD &&
	    read->kind != TERMINAL_HOSTS_LINE_HOST &&
	    read->kind != TERMINAL_HOSTS_LINE_MATCH)
		return 0;

	/* The keyword, compared as ssh compares it. */
	same = hosts_equal(hosts->text + read->keyword, read->keyword_end - read->keyword, keyword);
	if (!same)
		return 0;

	/* Succeeded: the line is the keyword's. */
	return 1;
}

/*
 * Finds the first line of a block with the keyword after the line given
 * (TERMINAL_HOSTS_NONE: from the block's first).  Returns the line's
 * index, or TERMINAL_HOSTS_NONE when the block has no more.
 */
unsigned
terminal_hosts_find(
	const struct terminal_hosts *hosts,
	unsigned block,
	const char *keyword,
	unsigned after)
{
	const struct terminal_hosts_block *read;
	unsigned index;
	int same;

	/* A block that is there. */
	if (block >= hosts->block_count)
		return TERMINAL_HOSTS_NONE;
	read = &hosts->blocks[block];

	/* From the block's first line, or the one after the line given. */
	index = read->first;
	if (after != TERMINAL_HOSTS_NONE && after + 1U > index)
		index = after + 1U;

	/* Each line of the block from there. */
	for (; index < read->end; index++) {
		same = terminal_hosts_keyword_is(hosts, index, keyword);
		if (same)
			return index;
	}

	/* The block has no more. */
	return TERMINAL_HOSTS_NONE;
}

/*
 * Copies a line's value as it is written (quotes and all), cut to size - 1
 * bytes and ended.  Returns the value's whole length.
 */
size_t
terminal_hosts_value(
	const struct terminal_hosts *hosts,
	unsigned line,
	char *value,
	size_t size)
{
	const struct terminal_hosts_line *read;
	size_t length;
	size_t copied;

	/* None for a line that is not there. */
	value[0] = '\0';
	if (line >= hosts->line_count)
		return 0U;
	read = &hosts->lines[line];

	/* The value, as much as fits. */
	length = read->value_end - read->value;
	copied = length;
	if (copied > size - 1U)
		copied = size - 1U;
	memcpy(value, hosts->text + read->value, copied);
	value[copied] = '\0';

	/* Reports the value's whole length. */
	return length;
}

/*
 * Copies the index'th word (from 0) of a line's value, without its
 * quotes, cut to size - 1 bytes and ended.  Returns the word's whole
 * length (0, with an empty word, when the value has no such word).
 */
size_t
terminal_hosts_word(
	const struct terminal_hosts *hosts,
	unsigned line,
	unsigned index,
	char *word,
	size_t size)
{
	const struct terminal_hosts_line *read;
	unsigned count;
	size_t start;
	size_t end;
	size_t at;
	size_t copied;
	int found;

	/* None for a line that is not there. */
	word[0] = '\0';
	if (line >= hosts->line_count)
		return 0U;
	read = &hosts->lines[line];

	/* The words up to the one asked for. */
	at = read->value;
	count = 0U;
	for (;;) {
		found = hosts_next_word(hosts->text, &at, read->value_end, &start, &end);
		if (!found)
			return 0U;

		/* The word asked for. */
		if (count == index)
			break;
		count++;
	}

	/* The word, as much as fits. */
	copied = end - start;
	if (copied > size - 1U)
		copied = size - 1U;
	memcpy(word, hosts->text + start, copied);
	word[copied] = '\0';

	/* Reports the word's whole length. */
	return end - start;
}

/*
 * Tells whether a block is a Host's with a pattern that names one host
 * (no '*', '?' or '!'), which the blocks before it can be compared for.
 * Returns 1 or 0.
 */
int
terminal_hosts_has_name(
	const struct terminal_hosts *hosts,
	unsigned block)
{
	const struct terminal_hosts_line *header;
	size_t start;
	size_t end;
	size_t at;
	int concrete;
	int found;

	/* Only a Host's block has names. */
	if (block >= hosts->block_count || hosts->blocks[block].kind != TERMINAL_HOSTS_BLOCK_HOST)
		return 0;
	header = &hosts->lines[hosts->blocks[block].first];

	/* Each of its patterns, until one names one host. */
	at = header->value;
	for (;;) {
		found = hosts_next_word(hosts->text, &at, header->value_end, &start, &end);
		if (!found)
			break;

		/* A pattern of one host. */
		concrete = hosts_concrete(hosts->text + start, end - start);
		if (concrete)
			return 1;
	}

	/* Every pattern is a wildcard's or a negation. */
	return 0;
}

/*
 * Tells whether a pattern (length bytes, no negation) matches a host's
 * name: '*' matches any run of characters, '?' any one, and the rest
 * themselves with the case of letters aside.  Returns 1 or 0.
 *
 * Procedure referenced: ssh_config(5) of OpenSSH, PATTERNS.
 */
int
terminal_hosts_match(
	const char *pattern,
	size_t length,
	const char *name)
{
	size_t at;
	size_t star;
	size_t marked;
	size_t place;
	char left;
	char right;
	int starred;
	int same;

	/* The pattern and the name side by side, back to the last '*' on a mismatch. */
	at = 0U;
	place = 0U;
	star = 0U;
	marked = 0U;
	starred = 0;
	for (;;) {
		/* Both at their end: a match. */
		if (at == length && name[place] == '\0')
			return 1;

		/* A '*' takes no character first, remembering where to take one more. */
		if (at < length && pattern[at] == '*') {
			starred = 1;
			star = at;
			marked = place;
			at++;
			continue;
		}

		/* A '?' or the same character (a capital as its small letter) goes on with both. */
		same = 0;
		if (at < length && name[place] != '\0') {
			left = hosts_lower(pattern[at]);
			right = hosts_lower(name[place]);
			if (pattern[at] == '?' || left == right)
				same = 1;
		}

		/* Both go on to their next character. */
		if (same) {
			at++;
			place++;
			continue;
		}

		/* A mismatch with no '*' before, or with the name at its end: no match. */
		if (!starred || name[marked] == '\0')
			return 0;

		/* The last '*' takes one more character. */
		marked++;
		place = marked;
		at = star + 1U;
	}
}

/*
 * Tells whether a block applies to a host's name: the part before any Host
 * always does, a Host's when one of its patterns matches the name and no
 * negated one does, and a Match's is not known (0).  Returns 1 or 0.
 */
int
terminal_hosts_applies(
	const struct terminal_hosts *hosts,
	unsigned block,
	const char *name)
{
	const struct terminal_hosts_line *header;
	size_t start;
	size_t end;
	size_t at;
	int matched;
	int negated;
	int found;
	int hit;

	/* The lines before any Host apply to every host. */
	if (block >= hosts->block_count)
		return 0;
	if (hosts->blocks[block].kind == TERMINAL_HOSTS_BLOCK_BEFORE)
		return 1;

	/* A Match's condition is not read here. */
	if (hosts->blocks[block].kind != TERMINAL_HOSTS_BLOCK_HOST)
		return 0;

	/* Each pattern of the Host line against the name. */
	header = &hosts->lines[hosts->blocks[block].first];
	matched = 0;
	negated = 0;
	at = header->value;
	for (;;) {
		found = hosts_next_word(hosts->text, &at, header->value_end, &start, &end);
		if (!found)
			break;

		/* A negated pattern that matches rules the block out. */
		if (end > start && hosts->text[start] == '!') {
			hit = terminal_hosts_match(hosts->text + start + 1U, end - start - 1U, name);
			if (hit)
				negated = 1;
			continue;
		}

		/* A plain pattern that matches lets it in. */
		hit = terminal_hosts_match(hosts->text + start, end - start, name);
		if (hit)
			matched = 1;
	}

	/* The block applies when a pattern matched and no negated one did. */
	if (matched && !negated)
		return 1;

	/* It does not. */
	return 0;
}

/*
 * Finds the line of a keyword of one value that wins over a block's own:
 * the first such line of a block before it (the part before any Host
 * among them) that applies to one of the names the block's Host line
 * names.  Returns the line's index, or TERMINAL_HOSTS_NONE when no block
 * before wins (or the block names no single host).
 */
unsigned
terminal_hosts_overrides(
	const struct terminal_hosts *hosts,
	unsigned block,
	const char *keyword)
{
	unsigned other;
	unsigned line;
	int applies;

	/* Only a Host's block with a name is compared. */
	if (block >= hosts->block_count || hosts->blocks[block].kind != TERMINAL_HOSTS_BLOCK_HOST)
		return TERMINAL_HOSTS_NONE;

	/* Each block before it, the first that applies and has the keyword winning. */
	for (other = 0U; other < block; other++) {
		line = terminal_hosts_find(hosts, other, keyword, TERMINAL_HOSTS_NONE);
		if (line == TERMINAL_HOSTS_NONE)
			continue;

		/* The block's names the other one applies to. */
		applies = hosts_applies_to_block(hosts, other, block);
		if (applies)
			return line;
	}

	/* No block before wins. */
	return TERMINAL_HOSTS_NONE;
}

/*
 * Finds the next line of a keyword of a list (after the line given;
 * TERMINAL_HOSTS_NONE: from the first) in another block that applies to
 * one of the names a block's Host line names, which ssh adds to the
 * block's own.  Returns the line's index, or TERMINAL_HOSTS_NONE.
 */
unsigned
terminal_hosts_other(
	const struct terminal_hosts *hosts,
	unsigned block,
	const char *keyword,
	unsigned after)
{
	unsigned index;
	int applies;
	int same;

	/* Only a Host's block with a name is compared. */
	if (block >= hosts->block_count || hosts->blocks[block].kind != TERMINAL_HOSTS_BLOCK_HOST)
		return TERMINAL_HOSTS_NONE;

	/* From the first line, or the one after the line given. */
	index = 0U;
	if (after != TERMINAL_HOSTS_NONE)
		index = after + 1U;

	/* Each line of the keyword outside the block whose block applies. */
	for (; index < hosts->line_count; index++) {
		if (hosts->lines[index].block == block)
			continue;
		same = terminal_hosts_keyword_is(hosts, index, keyword);
		if (!same)
			continue;
		applies = hosts_applies_to_block(hosts, hosts->lines[index].block, block);
		if (applies)
			return index;
	}

	/* No other block adds the keyword. */
	return TERMINAL_HOSTS_NONE;
}

/*
 * Writes a value of one word as the file holds it: in double quotes when
 * it has a blank or starts with '#' or '=' (which a bare word cannot).
 * Returns 0, or EINVAL for an empty value or one with a double quote or a
 * control character (no line can hold them), or ENAMETOOLONG.
 */
int
terminal_hosts_put_word(
	const char *value,
	char *written,
	size_t size)
{
	size_t length;
	size_t index;
	int quoted;
	int bad;
	int made;

	/* A word without what no line can hold. */
	length = strlen(value);
	if (length == 0U)
		return EINVAL;
	bad = hosts_word_bad(value, length);
	if (bad)
		return EINVAL;

	/* Quoted when a bare word could not hold it. */
	quoted = 0;
	if (value[0] == '#' || value[0] == '=')
		quoted = 1;
	for (index = 0U; index < length; index++) {
		if (value[index] == ' ')
			quoted = 1;
	}

	/* The word, quoted or bare. */
	if (quoted) {
		made = snprintf(written, size, "\"%s\"", value);
	} else {
		made = snprintf(written, size, "%s", value);
	}

	/* A word longer than the room is refused. */
	if (made < 0 || (size_t)made >= size)
		return ENAMETOOLONG;

	/* Succeeded: the word as the file holds it. */
	return 0;
}

/*
 * Writes a Host line's patterns as the file holds them: the words of the
 * value (split at blanks) one blank apart.  Returns 0, or EINVAL for no
 * pattern or one that starts with '#' or '=' or has a double quote or a
 * control character, or ENAMETOOLONG.
 */
int
terminal_hosts_put_names(
	const char *value,
	char *written,
	size_t size)
{
	size_t length;
	size_t at;
	size_t start;
	size_t made;
	int bad;

	/* The words, one after another. */
	length = strlen(value);
	made = 0U;
	at = 0U;
	for (;;) {
		/* The blanks before a word. */
		while (at < length && (value[at] == ' ' || value[at] == '\t'))
			at++;
		if (at == length)
			break;

		/* The word, up to a blank. */
		start = at;
		while (at < length && value[at] != ' ' && value[at] != '\t')
			at++;

		/* A pattern a bare word holds. */
		bad = hosts_word_bad(value + start, at - start);
		if (bad || value[start] == '#' || value[start] == '=')
			return EINVAL;

		/* The pattern after a blank, when the room holds it. */
		if (made + (made != 0U) + (at - start) + 1U > size)
			return ENAMETOOLONG;
		if (made != 0U)
			written[made++] = ' ';
		memcpy(written + made, value + start, at - start);
		made += at - start;
	}

	/* No pattern at all is refused. */
	if (made == 0U)
		return EINVAL;

	/* Succeeded: the patterns. */
	written[made] = '\0';
	return 0;
}

/*
 * Writes a RemoteCommand's value as the file holds it: the command without
 * the blanks round it, as written (ssh keeps the rest of the line).
 * Returns 0, or EINVAL for an empty command, one that starts with '='
 * (the separator would take it) or one with a control character other
 * than the tab, or ENAMETOOLONG.
 */
int
terminal_hosts_put_command(
	const char *value,
	char *written,
	size_t size)
{
	size_t length;
	size_t start;
	size_t index;
	unsigned char byte;

	/* The command without the blanks round it. */
	length = strlen(value);
	start = 0U;
	while (start < length && (value[start] == ' ' || value[start] == '\t'))
		start++;
	while (length > start && (value[length - 1U] == ' ' || value[length - 1U] == '\t'))
		length--;

	/* Something, and not what the separator takes. */
	if (length == start || value[start] == '=')
		return EINVAL;

	/* No line break or other control character. */
	for (index = start; index < length; index++) {
		byte = (unsigned char)value[index];
		if ((byte < 0x20U && byte != '\t') || byte == 0x7fU)
			return EINVAL;
	}

	/* A command longer than the room is refused. */
	if (length - start + 1U > size)
		return ENAMETOOLONG;

	/* Succeeded: the command. */
	memcpy(written, value + start, length - start);
	written[length - start] = '\0';
	return 0;
}

/*
 * Writes a LocalForward's or a RemoteForward's value as the file holds it:
 * where it listens ([address:]port) and where it goes (host:port, an IPv6
 * address in brackets), a blank between.  A remote listen may be on port
 * 0 (the server chooses).  Returns 0, or EINVAL with *why saying what is
 * wrong, or ENAMETOOLONG.  The program's check reads it finally.
 */
int
terminal_hosts_put_forward(
	int remote,
	const char *listen,
	const char *destination,
	char *written,
	size_t size,
	const char **why)
{
	int made;
	int bad;

	/* Where it listens: a port, after an address or not. */
	*why = "";
	bad = hosts_endpoint(listen, 0, remote, why);
	if (bad)
		return EINVAL;

	/* Where it goes: a host and a port. */
	bad = hosts_endpoint(destination, 1, 0, why);
	if (bad)
		return EINVAL;

	/* The two words. */
	made = snprintf(written, size, "%s %s", listen, destination);
	if (made < 0 || (size_t)made >= size) {
		*why = "too long";
		return ENAMETOOLONG;
	}

	/* Succeeded: the forward's value. */
	return 0;
}

/*
 * Makes the text with a keyword of one value set in a block: the first
 * line's value written in place (the keyword's spelling, its indent, the
 * separator and a comment after it kept), a line added when the block has
 * none, and every line of the keyword taken out for an empty value (so no
 * later one takes effect).  Host's names are its Host line's value.
 * Returns 0, EINVAL or ENOMEM.
 */
int
terminal_hosts_set(
	const struct terminal_hosts *hosts,
	unsigned block,
	const char *keyword,
	const char *value,
	char **out,
	size_t *out_size)
{
	const struct terminal_hosts_line *line;
	struct hosts_span *spans;
	char *spaced;
	unsigned count;
	unsigned index;
	size_t length;
	int host;
	int error;

	/* A block that is there. */
	if (block >= hosts->block_count)
		return EINVAL;

	/* Host is the block's own line. */
	host = hosts_equal(keyword, strlen(keyword), "Host");
	if (host) {
		if (hosts->blocks[block].kind != TERMINAL_HOSTS_BLOCK_HOST || value[0] == '\0')
			return EINVAL;
		index = hosts->blocks[block].first;
	} else {
		index = terminal_hosts_find(hosts, block, keyword, TERMINAL_HOSTS_NONE);
	}

	/* A value the block does not have yet is a new line. */
	if (index == TERMINAL_HOSTS_NONE && value[0] != '\0') {
		error = terminal_hosts_add(hosts, block, keyword, value, out, out_size);
		if (error != 0)
			return error;

		/* Succeeded: the line added. */
		return 0;
	}

	/* The changes: one for a value, one a line for a removal. */
	spans = calloc(HOSTS_SPANS_MAX, sizeof(*spans));
	if (spans == NULL)
		return ENOMEM;

	/* The value in place; a keyword without a separator is given a blank. */
	count = 0U;
	spaced = NULL;
	if (value[0] != '\0') {
		line = &hosts->lines[index];
		spans[0].from = line->value;
		spans[0].to = line->value_end;
		spans[0].insert = value;
		spans[0].insert_size = strlen(value);
		if (line->value == line->keyword_end) {
			length = strlen(value);
			spaced = malloc(length + 2U);
			if (spaced == NULL) {
				free(spans);
				return ENOMEM;
			}

			/* The blank, then the value. */
			spaced[0] = ' ';
			memcpy(spaced + 1, value, length + 1U);
			spans[0].insert = spaced;
			spans[0].insert_size = length + 1U;
		}

		/* One change. */
		count = 1U;
	}

	/* An empty value: every line of the keyword in the block goes. */
	while (value[0] == '\0' && index != TERMINAL_HOSTS_NONE && count < HOSTS_SPANS_MAX) {
		line = &hosts->lines[index];
		spans[count].from = line->start;
		spans[count].to = line->next;
		count++;
		index = terminal_hosts_find(hosts, block, keyword, index);
	}

	/* The new text. */
	error = hosts_splice(hosts->text, hosts->size, spans, count, out, out_size);
	free(spaced);
	free(spans);
	if (error != 0)
		return error;

	/* Succeeded: the text with the value set. */
	return 0;
}

/*
 * Makes the text with a line of a keyword added to a block (a keyword of a
 * list, or one of one value the block lacks): after the block's last line
 * of the keyword, else after its last keyword's line, else after its Host
 * line, indented as the block's lines are, ending as the line before it
 * does.  Returns 0, EINVAL or ENOMEM.
 */
int
terminal_hosts_add(
	const struct terminal_hosts *hosts,
	unsigned block,
	const char *keyword,
	const char *value,
	char **out,
	size_t *out_size)
{
	struct hosts_span span;
	char indent[HOSTS_INDENT_MAX];
	char line_break[HOSTS_BREAK_MAX];
	const char *before_break;
	char *added;
	size_t length;
	unsigned anchor;
	int lacks_break;
	int made;
	int error;

	/* A block that is there, and a value. */
	if (block >= hosts->block_count || value[0] == '\0')
		return EINVAL;

	/* Where the line goes, its indent and how it ends. */
	anchor = hosts_anchor(hosts, block, keyword);
	hosts_indent(hosts, block, indent);
	hosts_break(hosts, anchor, line_break);

	/* After the line before; one without a break first gets one. */
	lacks_break = 0;
	span.from = 0U;
	if (anchor != TERMINAL_HOSTS_NONE) {
		span.from = hosts->lines[anchor].next;
		if (hosts->lines[anchor].next == hosts->lines[anchor].end)
			lacks_break = 1;
	} else if (hosts->blocks[block].first < hosts->line_count) {
		span.from = hosts->lines[hosts->blocks[block].first].start;
	}

	/* An insertion: nothing of the text is covered. */
	span.to = span.from;

	/* The line. */
	length = strlen(indent) + strlen(keyword) + strlen(value) + 2U * strlen(line_break) + 2U;
	added = malloc(length);
	if (added == NULL)
		return ENOMEM;
	before_break = "";
	if (lacks_break)
		before_break = line_break;
	made = snprintf(added, length, "%s%s%s %s%s", before_break, indent, keyword, value, line_break);
	if (made < 0 || (size_t)made >= length) {
		free(added);
		return ENOMEM;
	}

	/* The line as the change. */
	span.insert = added;
	span.insert_size = (size_t)made;

	/* The new text. */
	error = hosts_splice(hosts->text, hosts->size, &span, 1U, out, out_size);
	free(added);
	if (error != 0)
		return error;

	/* Succeeded: the text with the line added. */
	return 0;
}

/*
 * Makes the text without one line (its line break too).  Returns 0,
 * EINVAL or ENOMEM.
 */
int
terminal_hosts_remove(
	const struct terminal_hosts *hosts,
	unsigned line,
	char **out,
	size_t *out_size)
{
	struct hosts_span span;
	int error;

	/* A line that is there. */
	if (line >= hosts->line_count)
		return EINVAL;

	/* Its bytes and its break. */
	span.from = hosts->lines[line].start;
	span.to = hosts->lines[line].next;
	span.insert = NULL;
	span.insert_size = 0U;

	/* The new text. */
	error = hosts_splice(hosts->text, hosts->size, &span, 1U, out, out_size);
	if (error != 0)
		return error;

	/* Succeeded: the text without the line. */
	return 0;
}

/*
 * Makes the text with a new Host block of the names (as the file holds
 * them) where ssh reads it before what would win over it: before the first
 * Host block that applies to its first name (comments right above that
 * block stay with it) or the first Match, else at the end after a blank
 * line.  *block is the new block's index in the new text.  Returns 0,
 * EINVAL or ENOMEM.
 */
int
terminal_hosts_add_host(
	const struct terminal_hosts *hosts,
	const char *names,
	char **out,
	size_t *out_size,
	unsigned *block)
{
	struct hosts_span span;
	char first[TERMINAL_HOSTS_VALUE_MAX];
	char line_break[HOSTS_BREAK_MAX];
	char *added;
	size_t length;
	unsigned index;
	unsigned line;
	int applies;
	int made;
	int error;

	/* The first name, which the blocks are compared for. */
	length = strcspn(names, " \t");
	if (length == 0U || length >= sizeof(first))
		return EINVAL;
	memcpy(first, names, length);
	first[length] = '\0';

	/* The first Match, or the first Host block that would apply to the name. */
	for (index = 1U; index < hosts->block_count; index++) {
		if (hosts->blocks[index].kind == TERMINAL_HOSTS_BLOCK_MATCH)
			break;
		applies = terminal_hosts_applies(hosts, index, first);
		if (applies)
			break;
	}

	/* Before that block's line and the comments right above it, a blank line between. */
	*block = index;
	if (index < hosts->block_count) {
		line = hosts->blocks[index].first;
		while (line > 0U && hosts->lines[line - 1U].kind == TERMINAL_HOSTS_LINE_COMMENT)
			line--;
		hosts_break(hosts, line, line_break);
		span.from = hosts->lines[line].start;
		span.to = span.from;
		length = strlen(names) + 3U * HOSTS_BREAK_MAX + 8U;
		added = malloc(length);
		if (added == NULL)
			return ENOMEM;
		made = snprintf(added, length, "Host %s%s%s", names, line_break, line_break);
	} else {
		/* At the end: the last line ended, a blank line after the text, then the block. */
		hosts_break(hosts, TERMINAL_HOSTS_NONE, line_break);
		span.from = hosts->size;
		span.to = span.from;
		length = strlen(names) + 3U * HOSTS_BREAK_MAX + 8U;
		added = malloc(length);
		if (added == NULL)
			return ENOMEM;
		if (hosts->size == 0U) {
			made = snprintf(added, length, "Host %s%s", names, line_break);
		} else if (hosts->text[hosts->size - 1U] != '\n') {
			made = snprintf(added, length, "%s%sHost %s%s", line_break, line_break, names, line_break);
		} else if (hosts->line_count > 0U && hosts->lines[hosts->line_count - 1U].kind == TERMINAL_HOSTS_LINE_EMPTY) {
			made = snprintf(added, length, "Host %s%s", names, line_break);
		} else {
			made = snprintf(added, length, "%sHost %s%s", line_break, names, line_break);
		}
	}

	/* The block's line as the change. */
	if (made < 0 || (size_t)made >= length) {
		free(added);
		return ENOMEM;
	}

	/* What it inserts. */
	span.insert = added;
	span.insert_size = (size_t)made;

	/* The new text. */
	error = hosts_splice(hosts->text, hosts->size, &span, 1U, out, out_size);
	free(added);
	if (error != 0)
		return error;

	/* Succeeded: the text with the new block. */
	return 0;
}

/*
 * Makes the text without a Host block: its Host line and every line of a
 * keyword in it (the keywords the page does not know and Include too, or
 * they would fall to the block before), its comments and blank lines kept.
 * *removed counts the lines taken out.  Returns 0, EINVAL or ENOMEM.
 */
int
terminal_hosts_remove_host(
	const struct terminal_hosts *hosts,
	unsigned block,
	char **out,
	size_t *out_size,
	unsigned *removed)
{
	const struct terminal_hosts_line *line;
	struct hosts_span *spans;
	unsigned index;
	unsigned count;
	int error;

	/* Only a Host's block goes. */
	*removed = 0U;
	if (block >= hosts->block_count || hosts->blocks[block].kind != TERMINAL_HOSTS_BLOCK_HOST)
		return EINVAL;

	/* The changes, one a line. */
	spans = calloc(HOSTS_SPANS_MAX, sizeof(*spans));
	if (spans == NULL)
		return ENOMEM;

	/* Each line of the block but its comments and blank lines. */
	count = 0U;
	for (index = hosts->blocks[block].first; index < hosts->blocks[block].end && count < HOSTS_SPANS_MAX; index++) {
		line = &hosts->lines[index];
		if (line->kind == TERMINAL_HOSTS_LINE_EMPTY || line->kind == TERMINAL_HOSTS_LINE_COMMENT)
			continue;
		spans[count].from = line->start;
		spans[count].to = line->next;
		count++;
	}

	/* The new text. */
	error = hosts_splice(hosts->text, hosts->size, spans, count, out, out_size);
	free(spans);
	if (error != 0)
		return error;

	/* Succeeded: the text without the block. */
	*removed = count;
	return 0;
}

/*
 * Tells whether a text has a problem the text before it did not: the
 * program's check reads it, and a problem on a line the text before had
 * the same is set aside (that line made a comment in a copy) and the check
 * made again, until no problem is left (0) or one is on a line the change
 * made (1, report naming it).  A problem without a line counts as the
 * text before's when its check finds the same.  Returns 1 or 0 (1 too
 * when the check cannot be made).
 */
int
terminal_hosts_new_problem(
	terminal_hosts_checker check,
	void *data,
	const char *before,
	size_t before_size,
	const char *text,
	size_t size,
	char *report,
	size_t capacity)
{
	struct terminal_hosts copy_hosts;
	struct terminal_hosts_line *line;
	char *earlier;
	char *copy;
	unsigned round;
	unsigned number;
	size_t at;
	int numbered;
	int was;
	int same;
	int error;

	/* A copy the problems before are set aside in. */
	report[0] = '\0';
	copy = malloc(size + 1U);
	if (copy == NULL) {
		(void)snprintf(report, capacity, "there is no memory to check the file");
		return 1;
	}

	/* The text's bytes in it. */
	memcpy(copy, text, size);
	copy[size] = '\0';

	/* The check, again for each problem set aside. */
	for (round = 0U; round < HOSTS_CHECK_ROUNDS; round++) {
		report[0] = '\0';
		error = check(data, copy, size, report, capacity);
		if (error != 0) {
			(void)snprintf(report, capacity, "the file could not be checked (%s)", strerror(error));
			free(copy);
			return 1;
		}

		/* No problem left. */
		if (report[0] == '\0') {
			free(copy);
			return 0;
		}

		/* A problem without a line: the text before's when it had the same. */
		numbered = hosts_problem_line(report, &number);
		if (!numbered) {
			earlier = calloc(1U, capacity);
			if (earlier == NULL) {
				free(copy);
				return 1;
			}

			/* The text before's report beside this one. */
			error = check(data, before, before_size, earlier, capacity);
			same = strcmp(earlier, report);
			free(earlier);
			free(copy);

			/* The same problem before is not new; another is. */
			if (error == 0 && same == 0)
				return 0;
			return 1;
		}

		/* The line of the problem. */
		error = terminal_hosts_read(&copy_hosts, copy, size);
		if (error != 0) {
			free(copy);
			return 1;
		}

		/* A line the text does not have is told as new. */
		if (number == 0U || number > copy_hosts.line_count) {
			terminal_hosts_release(&copy_hosts);
			free(copy);
			return 1;
		}

		/* The problem's line. */
		line = &copy_hosts.lines[number - 1U];

		/* A line the change made is a new problem. */
		was = hosts_line_was(before, before_size, copy + line->start, line->end - line->start);
		if (!was) {
			terminal_hosts_release(&copy_hosts);
			free(copy);
			return 1;
		}

		/* A line the text had before is made a comment, its length and the line numbers kept. */
		copy[line->start] = '#';
		for (at = line->start + 1U; at < line->end; at++)
			copy[at] = ' ';
		terminal_hosts_release(&copy_hosts);
	}

	/* Too many problems to set aside: told as the last one found. */
	free(copy);
	return 1;
}

/* Reads one line from start: where it ends, its kind, and its keyword's and value's places. */
static void
hosts_line_read(
	const char *text,
	size_t size,
	size_t start,
	struct terminal_hosts_line *line)
{
	const char *found;
	size_t at;
	size_t end;
	int blank;
	int named;
	int command;

	/* The line's end at its LF (or the text's end), a CR before the LF left out. */
	memset(line, 0, sizeof(*line));
	line->start = start;
	found = memchr(text + start, '\n', size - start);
	end = size;
	line->next = size;
	if (found != NULL) {
		end = (size_t)(found - text);
		line->next = end + 1U;
	}

	/* A CR before the LF is the break's. */
	if (end > start && text[end - 1U] == '\r')
		end--;
	line->end = end;

	/* The blanks it is indented by. */
	at = hosts_skip_blanks(text, start, end);
	line->keyword = at;
	line->keyword_end = at;
	line->value = at;
	line->value_end = at;

	/* A line of blanks, or a comment. */
	if (at == end) {
		line->kind = TERMINAL_HOSTS_LINE_EMPTY;
		return;
	}

	/* A comment. */
	if (text[at] == '#') {
		line->kind = TERMINAL_HOSTS_LINE_COMMENT;
		return;
	}

	/* A line with a NUL, or with no keyword before its '=', cannot be read. */
	found = memchr(text + start, '\0', end - start);
	if (found != NULL || text[at] == '=') {
		line->kind = TERMINAL_HOSTS_LINE_BAD;
		return;
	}

	/* The keyword: up to a blank or an '='. */
	while (at < end) {
		blank = hosts_blank(text[at]);
		if (blank || text[at] == '=')
			break;
		at++;
	}

	/* Where the keyword ends. */
	line->keyword_end = at;

	/* The separator: blanks, at most one '=', blanks. */
	at = hosts_skip_blanks(text, at, end);
	if (at < end && text[at] == '=')
		at++;
	at = hosts_skip_blanks(text, at, end);
	line->value = at;

	/* The kind of the keyword's line: a Host's, a Match's, or another's. */
	line->kind = TERMINAL_HOSTS_LINE_KEYWORD;
	named = hosts_equal(text + line->keyword, line->keyword_end - line->keyword, "Host");
	if (named)
		line->kind = TERMINAL_HOSTS_LINE_HOST;
	named = hosts_equal(text + line->keyword, line->keyword_end - line->keyword, "Match");
	if (named)
		line->kind = TERMINAL_HOSTS_LINE_MATCH;

	/* RemoteCommand's value is the rest of the line, without the blanks after it. */
	command = hosts_equal(text + line->keyword, line->keyword_end - line->keyword, "RemoteCommand");
	if (command) {
		at = end;
		while (at > line->value) {
			blank = hosts_blank(text[at - 1U]);
			if (!blank)
				break;
			at--;
		}

		/* The command ends there. */
		line->value_end = at;
		return;
	}

	/* The words of the value, up to a comment. */
	line->value_end = hosts_value_end(text, line->value, end);
}

/* Finds where a value's words end: before a word that starts with '#' (a comment) and the blanks before it. */
static size_t
hosts_value_end(
	const char *text,
	size_t value,
	size_t end)
{
	size_t at;
	size_t last;
	size_t start;
	size_t word_end;
	int found;

	/* Each word, up to a comment or the end. */
	at = value;
	last = value;
	for (;;) {
		/* A comment ends the words. */
		at = hosts_skip_blanks(text, at, end);
		if (at >= end || text[at] == '#')
			break;

		/* The next word, and where it ends (its closing quote too). */
		found = hosts_next_word(text, &at, end, &start, &word_end);
		if (!found)
			break;
		last = at;
	}

	/* Reports the end of the last word. */
	return last;
}

/*
 * Reads the next word of a value from *at (blanks first): a quoted word's
 * bytes inside its quotes (an unterminated one to the end), or a bare
 * word's up to a blank.  *at moves past it.  Returns 1, or 0 when there is
 * no more.
 */
static int
hosts_next_word(
	const char *text,
	size_t *at,
	size_t end,
	size_t *word,
	size_t *word_end)
{
	size_t position;

	/* The blanks before the word. */
	position = hosts_skip_blanks(text, *at, end);

	/* No more words. */
	if (position >= end) {
		*at = end;
		return 0;
	}

	/* A quoted word: inside its quotes. */
	if (text[position] == '"') {
		position++;
		*word = position;
		while (position < end && text[position] != '"')
			position++;
		*word_end = position;
		if (position < end)
			position++;
		*at = position;
		return 1;
	}

	/* A bare word: up to a blank. */
	*word = position;
	position = hosts_skip_word(text, position, end);
	*word_end = position;
	*at = position;

	/* Succeeded: a word. */
	return 1;
}

/* Moves past the blanks from at, to end at most; reports where they stop. */
static size_t
hosts_skip_blanks(
	const char *text,
	size_t at,
	size_t end)
{
	int blank;

	/* Each blank. */
	while (at < end) {
		blank = hosts_blank(text[at]);
		if (!blank)
			break;
		at++;
	}

	/* Reports the first byte that is not one. */
	return at;
}

/* Moves past a bare word's bytes from at, to end at most; reports the blank (or the end) after it. */
static size_t
hosts_skip_word(
	const char *text,
	size_t at,
	size_t end)
{
	int blank;

	/* Each byte up to a blank. */
	while (at < end) {
		blank = hosts_blank(text[at]);
		if (blank)
			break;
		at++;
	}

	/* Reports where the word ends. */
	return at;
}

/* Reports whether a byte separates words: a space, a tab, a CR or another white-space character but the LF. */
static int
hosts_blank(
	char byte)
{
	/* The characters C calls white space, other than the LF that ends a line. */
	if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\v' || byte == '\f')
		return 1;

	/* Any other byte is a word's. */
	return 0;
}

/* Turns an ASCII capital into its small letter and leaves every other byte alone. */
static char
hosts_lower(
	char byte)
{
	/* A capital. */
	if (byte >= 'A' && byte <= 'Z')
		return (char)(byte - 'A' + 'a');

	/* Any other byte. */
	return byte;
}

/* Reports whether length bytes of text are a word, the case of letters aside. */
static int
hosts_equal(
	const char *text,
	size_t length,
	const char *word)
{
	size_t word_length;
	size_t index;
	char left;
	char right;

	/* The same length. */
	word_length = strlen(word);
	if (word_length != length)
		return 0;

	/* Each byte the same, a capital as its small letter. */
	for (index = 0U; index < length; index++) {
		left = hosts_lower(text[index]);
		right = hosts_lower(word[index]);
		if (left != right)
			return 0;
	}

	/* Succeeded: the same word. */
	return 1;
}

/* Reports whether a pattern names one host: not empty, and without '*', '?' or '!'. */
static int
hosts_concrete(
	const char *word,
	size_t length)
{
	size_t index;

	/* An empty pattern names nothing. */
	if (length == 0U)
		return 0;

	/* A wildcard or a negation makes a pattern of more (or no) hosts. */
	for (index = 0U; index < length; index++) {
		if (word[index] == '*' || word[index] == '?' || word[index] == '!')
			return 0;
	}

	/* The pattern is one name. */
	return 1;
}

/* Reports whether a block applies to one of the names (patterns of one host) of another Host block's line. */
static int
hosts_applies_to_block(
	const struct terminal_hosts *hosts,
	unsigned other,
	unsigned block)
{
	const struct terminal_hosts_line *header;
	char name[TERMINAL_HOSTS_VALUE_MAX];
	size_t start;
	size_t end;
	size_t at;
	int concrete;
	int applies;
	int found;

	/* Each name of the block's Host line. */
	header = &hosts->lines[hosts->blocks[block].first];
	at = header->value;
	for (;;) {
		found = hosts_next_word(hosts->text, &at, header->value_end, &start, &end);
		if (!found)
			break;

		/* Only a pattern of one host is a name. */
		concrete = hosts_concrete(hosts->text + start, end - start);
		if (!concrete || end - start >= sizeof(name))
			continue;
		memcpy(name, hosts->text + start, end - start);
		name[end - start] = '\0';

		/* The other block applies to it. */
		applies = terminal_hosts_applies(hosts, other, name);
		if (applies)
			return 1;
	}

	/* It applies to none of them. */
	return 0;
}

/* Reports whether a word has what no line's word can hold: a double quote or a control character. */
static int
hosts_word_bad(
	const char *value,
	size_t length)
{
	size_t index;
	unsigned char byte;

	/* Each byte. */
	for (index = 0U; index < length; index++) {
		byte = (unsigned char)value[index];
		if (byte == '"' || byte < 0x20U || byte == 0x7fU)
			return 1;
	}

	/* Every byte may be in a word. */
	return 0;
}

/* Reports whether length bytes are a port: digits, 1 to 65535 (0 too when zero is set). */
static int
hosts_port(
	const char *text,
	size_t length,
	int zero)
{
	unsigned long number;
	size_t index;

	/* Digits, not too many. */
	if (length == 0U || length > 5U)
		return 0;
	number = 0UL;
	for (index = 0U; index < length; index++) {
		if (text[index] < '0' || text[index] > '9')
			return 0;
		number = number * 10UL + (unsigned long)(text[index] - '0');
	}

	/* Within the ports. */
	if (number > HOSTS_PORT_MAX)
		return 0;
	if (number == 0UL && !zero)
		return 0;

	/* Succeeded: a port. */
	return 1;
}

/*
 * Reports whether an end of a forward is wrong (1, with *why): a port
 * after an address ([address]:port for IPv6) or alone, the address needed
 * when need_host is set; no blank, quote, control character or '/' (a
 * Unix socket, which the forwards do not take).
 */
static int
hosts_endpoint(
	const char *text,
	int need_host,
	int zero,
	const char **why)
{
	const char *colon;
	const char *blank;
	const char *slash;
	const void *inner;
	size_t length;
	size_t host;
	int bad;
	int port;

	/* Something, as a bare word. */
	length = strlen(text);
	if (length == 0U && need_host) {
		*why = "the destination is missing";
		return 1;
	}

	/* Nor where it listens. */
	if (length == 0U) {
		*why = "the listening port is missing";
		return 1;
	}

	/* A bare word. */
	bad = hosts_word_bad(text, length);
	blank = strchr(text, ' ');
	if (bad || blank != NULL || text[0] == '#' || text[0] == '=') {
		*why = "a blank, a quote or a control character is not taken";
		return 1;
	}

	/* A path is a Unix socket's. */
	slash = strchr(text, '/');
	if (slash != NULL) {
		*why = "a Unix socket is not supported";
		return 1;
	}

	/* The port after the last ':', or the whole when there is none. */
	colon = strrchr(text, ':');
	host = 0U;
	if (colon != NULL)
		host = (size_t)(colon - text);
	if (colon == NULL && need_host) {
		*why = "the destination is host:port";
		return 1;
	}

	/* The port. */
	if (colon != NULL)
		port = hosts_port(colon + 1, length - host - 1U, zero);
	else
		port = hosts_port(text, length, zero);
	if (!port) {
		*why = "the port is a number from 1 to 65535";
		return 1;
	}

	/* An address before the port, an IPv6 one in brackets. */
	if (colon != NULL && host == 0U) {
		*why = "the address before the ':' is missing";
		return 1;
	}

	/* An address in brackets closes them before the port. */
	if (colon != NULL && text[0] == '[' && text[host - 1U] != ']') {
		*why = "an IPv6 address is written in brackets";
		return 1;
	}

	/* An address with a ':' (IPv6) must be in brackets. */
	inner = NULL;
	if (colon != NULL && text[0] != '[')
		inner = memchr(text, ':', host);
	if (inner != NULL) {
		*why = "an IPv6 address is written in brackets";
		return 1;
	}

	/* Succeeded: the end is right as far as the page reads it. */
	return 0;
}

/*
 * Finds the line a block's new line of a keyword goes after: its last line
 * of the keyword, else its last keyword's line, else its Host or Match
 * line; TERMINAL_HOSTS_NONE for a block without a line.
 */
static unsigned
hosts_anchor(
	const struct terminal_hosts *hosts,
	unsigned block,
	const char *keyword)
{
	const struct terminal_hosts_block *read;
	unsigned index;
	unsigned last;
	unsigned kept;
	int same;

	/* The block's lines from its last. */
	read = &hosts->blocks[block];
	last = TERMINAL_HOSTS_NONE;
	kept = TERMINAL_HOSTS_NONE;
	for (index = read->first; index < read->end; index++) {
		same = terminal_hosts_keyword_is(hosts, index, keyword);
		if (same)
			last = index;
		if (hosts->lines[index].kind == TERMINAL_HOSTS_LINE_KEYWORD ||
		    hosts->lines[index].kind == TERMINAL_HOSTS_LINE_BAD)
			kept = index;
	}

	/* The keyword's last line, else the last keyword's. */
	if (last != TERMINAL_HOSTS_NONE)
		return last;
	if (kept != TERMINAL_HOSTS_NONE)
		return kept;

	/* The Host or Match line; nothing in the part before a first Host. */
	if (read->kind != TERMINAL_HOSTS_BLOCK_BEFORE)
		return read->first;

	/* A block without a line. */
	return TERMINAL_HOSTS_NONE;
}

/* Writes the line break a line ends with: its own, else the file's first, else LF. */
static void
hosts_break(
	const struct terminal_hosts *hosts,
	unsigned line,
	char *text)
{
	const struct terminal_hosts_line *read;
	unsigned index;

	/* LF unless a line shows otherwise. */
	text[0] = '\n';
	text[1] = '\0';

	/* The line's own break. */
	if (line != TERMINAL_HOSTS_NONE && line < hosts->line_count) {
		read = &hosts->lines[line];
		if (read->next > read->end) {
			if (read->next - read->end == 2U) {
				text[0] = '\r';
				text[1] = '\n';
				text[2] = '\0';
			}

			/* The line's own. */
			return;
		}
	}

	/* The file's first break. */
	for (index = 0U; index < hosts->line_count; index++) {
		read = &hosts->lines[index];
		if (read->next == read->end)
			continue;
		if (read->next - read->end == 2U) {
			text[0] = '\r';
			text[1] = '\n';
			text[2] = '\0';
		}

		/* The file's first. */
		return;
	}
}

/*
 * Writes the indent of a block's new line: its first keyword's line's, else
 * (in a Host's block) the first keyword's line's of any Host block, else
 * four spaces (none before a first Host).
 */
static void
hosts_indent(
	const struct terminal_hosts *hosts,
	unsigned block,
	char *indent)
{
	const struct terminal_hosts_line *read;
	unsigned index;
	size_t length;
	int found;

	/* The block's first keyword's line, then any Host block's. */
	found = 0;
	indent[0] = '\0';
	for (index = hosts->blocks[block].first; index < hosts->blocks[block].end; index++) {
		if (hosts->lines[index].kind == TERMINAL_HOSTS_LINE_KEYWORD) {
			found = 1;
			break;
		}
	}

	/* A Host's block without one: the first keyword's line of any Host block. */
	if (!found && hosts->blocks[block].kind == TERMINAL_HOSTS_BLOCK_HOST) {
		for (index = 0U; index < hosts->line_count; index++) {
			read = &hosts->lines[index];
			if (read->kind == TERMINAL_HOSTS_LINE_KEYWORD &&
			    hosts->blocks[read->block].kind == TERMINAL_HOSTS_BLOCK_HOST) {
				found = 1;
				break;
			}
		}
	}

	/* No line shows it: four spaces in a Host's block, none before. */
	if (!found) {
		if (hosts->blocks[block].kind == TERMINAL_HOSTS_BLOCK_HOST)
			(void)snprintf(indent, HOSTS_INDENT_MAX, "%s", HOSTS_INDENT);
		return;
	}

	/* That line's blanks, as they are (tabs too). */
	read = &hosts->lines[index];
	length = read->keyword - read->start;
	if (length >= HOSTS_INDENT_MAX)
		length = HOSTS_INDENT_MAX - 1U;
	memcpy(indent, hosts->text + read->start, length);
	indent[length] = '\0';
}

/* Makes a new text of the old with the changes (in order, none overlapping); 0 or ENOMEM. */
static int
hosts_splice(
	const char *text,
	size_t size,
	const struct hosts_span *spans,
	unsigned count,
	char **out,
	size_t *out_size)
{
	char *made;
	size_t length;
	size_t at;
	size_t put;
	unsigned index;

	/* The new length. */
	length = size;
	for (index = 0U; index < count; index++)
		length = length - (spans[index].to - spans[index].from) + spans[index].insert_size;

	/* The new text, a NUL after it. */
	made = malloc(length + 1U);
	if (made == NULL)
		return ENOMEM;

	/* The old bytes up to each change, its insert in place of what it covers. */
	at = 0U;
	put = 0U;
	for (index = 0U; index < count; index++) {
		memcpy(made + put, text + at, spans[index].from - at);
		put += spans[index].from - at;
		if (spans[index].insert_size > 0U)
			memcpy(made + put, spans[index].insert, spans[index].insert_size);
		put += spans[index].insert_size;
		at = spans[index].to;
	}

	/* The bytes after the last change. */
	memcpy(made + put, text + at, size - at);
	put += size - at;
	made[put] = '\0';

	/* Succeeded: the new text. */
	*out = made;
	*out_size = put;
	return 0;
}

/* Reads the line a check's report names ("line N: ..."); 1 with *number, or 0 for a report without one. */
static int
hosts_problem_line(
	const char *report,
	unsigned *number)
{
	unsigned long value;
	char *end;
	int differs;

	/* The word "line", then the number and a ':'. */
	differs = strncmp(report, "line ", 5U);
	if (differs != 0)
		return 0;
	value = strtoul(report + 5, &end, 10);
	if (end == report + 5 || *end != ':' || value > 0xffffffUL)
		return 0;

	/* Succeeded: the line's number. */
	*number = (unsigned)value;
	return 1;
}

/* Reports whether the text before had a line of the same words (its break aside). */
static int
hosts_line_was(
	const char *before,
	size_t before_size,
	const char *line,
	size_t length)
{
	size_t start;
	size_t end;
	size_t next;
	const char *found;
	int differs;

	/* Each line of the text before. */
	start = 0U;
	while (start < before_size) {
		found = memchr(before + start, '\n', before_size - start);
		end = before_size;
		next = before_size;
		if (found != NULL) {
			end = (size_t)(found - before);
			next = end + 1U;
		}

		/* A CR before the LF is the break's. */
		if (end > start && before[end - 1U] == '\r')
			end--;

		/* The same words. */
		if (end - start == length) {
			differs = memcmp(before + start, line, length);
			if (differs == 0)
				return 1;
		}

		/* The next line. */
		start = next;
	}

	/* No such line before. */
	return 0;
}
