/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rules of sessiond's failure counts (auth.h; ws172-p002).  Pure: no
 * file, no process, no clock, so the host test runs them alone.
 *
 * An attempt counts before passkey runs, and only a success clears the
 * counts, so an attempt cut short counts as a failure; but a security
 * key's login or unlock counts by its answer (ws199-p001, review-3 R4):
 * a wrong key PIN, a cloned key or an answer that does not verify counts,
 * a key that was not there, not touched, or taken away does not (the key
 * counts its own wrong PINs).  The answer to a
 * failure waits 2 seconds, twice as long after every three in a row, at
 * most 16.  The PIN is offered from the start, so that a machine without
 * a keyboard (a tablet) can log in, and is turned off by five wrong PINs in
 * a row until the account's password or security key is accepted.  The
 * PIN's wrong tries are kept in a file of root's too (auth.c reads and
 * writes it; the text is made and read here), so that a restart, which
 * anyone at the login screen can cause, gives no new tries.
 */

#include "auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The first line of the file of the PIN's tries. */
#define POLICY_TRIES_HEADER	"# sessiond pin-tries 1\n"

/* passkey's reasons and the words the greeter and the session are told. */
struct policy_reason {
	const char *passkey;
	const char *told;
};

/* The mapping; any other reason is told as bad-secret. */
static const struct policy_reason policy_reasons[] = {
	{ "bad-secret", "bad-secret" },
	{ "no-such-user", "bad-secret" },
	{ "not-enrolled", "bad-secret" },
	{ "locked-account", "locked" },
	{ "pin-off", "pin-off" },
	{ "style-off", "style-off" },
	{ "no-key", "no-key" },
	{ "many-keys", "many-keys" },
	{ "key-locked", "locked" },
	{ "bad-key-pin", "bad-secret" },
	{ "key-replug", "locked" },
	{ "canceled", "timeout" },
	{ "no-pin", "device" },
	{ "pin-policy", "bad-secret" },
	{ "not-allowed", "device" },
	{ "pin-set", "device" },
	{ "timeout", "timeout" },
	{ "device", "device" },
	{ "cloned", "cloned" },
	{ "pin-required", "pin-required" },
	{ "busy", "busy" },
	{ "bad-request", "bad-request" },
	{ "internal", "internal" },
};

/*
 * Finds (or makes) an account's counts: known accounts by user ID, the
 * names of no account in one shared entry.  A full table gives the shared
 * entry too.
 */
struct sessiond_count *
sessiond_policy_count(
	struct sessiond_policy *policy,
	int known,
	uid_t uid)
{
	unsigned index;

	/* A name of no account. */
	if (!known)
		return &policy->unknown;

	/* The account's entry, or a free one. */
	for (index = 0U; index < SESSIOND_COUNTS_MAX; index++) {
		if (policy->counts[index].used && policy->counts[index].uid == uid)
			return &policy->counts[index];
	}

	/* A free entry becomes the account's. */
	for (index = 0U; index < SESSIOND_COUNTS_MAX; index++) {
		if (!policy->counts[index].used) {
			memset(&policy->counts[index], 0, sizeof(policy->counts[index]));
			policy->counts[index].used = 1;
			policy->counts[index].uid = uid;
			return &policy->counts[index];
		}
	}

	/* No room: the shared entry. */
	return &policy->unknown;
}

/*
 * Splits a string in place (the C library has no strtok_r): gives the next
 * word ended by the separator, or NULL at the end; empty words are
 * skipped.
 */
char *
sessiond_policy_word(
	char **cursor,
	char separator)
{
	char *word;
	char *end;

	/* Past the separators. */
	word = *cursor;
	while (*word == separator)
		word++;
	if (*word == '\0') {
		*cursor = word;
		return NULL;
	}

	/* The word, ended in place. */
	end = strchr(word, separator);
	if (end == NULL) {
		*cursor = word + strlen(word);
		return word;
	}

	/* Succeeded: the word, and the rest after its separator. */
	*end = '\0';
	*cursor = end + 1;
	return word;
}

/* Gives a style's number from its word, or -1. */
int
sessiond_policy_style(
	const char *word)
{
	int match;

	/* The password. */
	match = strcmp(word, "password");
	if (match == 0)
		return SESSIOND_STYLE_PASSWORD;

	/* The PIN. */
	match = strcmp(word, "pin");
	if (match == 0)
		return SESSIOND_STYLE_PIN;

	/* A security key. */
	match = strcmp(word, "fido2");
	if (match == 0)
		return SESSIOND_STYLE_FIDO2;

	/* No style. */
	return -1;
}

/* Tells whether the PIN may be tried now. */
int
sessiond_policy_pin_allowed(
	const struct sessiond_count *count)
{
	/* Not after five wrong in a row. */
	if (count->pin_wrong >= SESSIOND_PIN_TRIES)
		return 0;
	return 1;
}

/* Counts an attempt before it runs. */
void
sessiond_policy_attempt(
	struct sessiond_count *count,
	int style)
{
	count->wrong++;
	if (style == SESSIOND_STYLE_PIN)
		count->pin_wrong++;
}

/* Clears the counts after a success of any style (a right PIN too, before the fifth wrong one). */
void
sessiond_policy_success(
	struct sessiond_count *count,
	int style)
{
	(void)style;
	count->wrong = 0U;
	count->pin_wrong = 0U;
}

/*
 * Takes the PIN's wrong tries from the file's text: after its first line,
 * a line "UID TRIES" for each account with some.  A line that is not of
 * that form is skipped; an account already counted keeps the larger
 * number.
 */
void
sessiond_policy_tries_read(
	struct sessiond_policy *policy,
	const char *text)
{
	struct sessiond_count *count;
	unsigned long uid;
	unsigned long tries;
	const char *line;
	const char *next;
	char *end;
	int match;

	/* Only a file of this form. */
	match = strncmp(text, POLICY_TRIES_HEADER, strlen(POLICY_TRIES_HEADER));
	if (match != 0)
		return;

	/* Each line after the first. */
	for (line = text + strlen(POLICY_TRIES_HEADER); *line != '\0'; line = next) {
		next = strchr(line, '\n');
		if (next == NULL)
			next = line + strlen(line);
		else
			next++;

		/* The user ID, a space, the tries and the line's end. */
		if (*line < '0' || *line > '9')
			continue;
		uid = strtoul(line, &end, 10);
		if (*end != ' ' || end[1] < '0' || end[1] > '9' || uid >= 0xffffffffUL)
			continue;
		tries = strtoul(end + 1, &end, 10);
		if (*end != '\n' && *end != '\0')
			continue;

		/* The account's count, the larger of the two. */
		if (tries > SESSIOND_PIN_TRIES)
			tries = SESSIOND_PIN_TRIES;
		count = sessiond_policy_count(policy, 1, (uid_t)uid);
		if (count == &policy->unknown)
			continue;
		if (count->pin_wrong < (unsigned)tries)
			count->pin_wrong = (unsigned)tries;
	}
}

/*
 * Writes the file's text from the counts: the first line, then "UID TRIES"
 * for each account with a PIN's wrong try.  Returns its length, or 0 when
 * it does not fit.
 */
size_t
sessiond_policy_tries_write(
	const struct sessiond_policy *policy,
	char *out,
	size_t size)
{
	const struct sessiond_count *count;
	unsigned index;
	size_t used;
	int length;

	/* The first line. */
	length = snprintf(out, size, "%s", POLICY_TRIES_HEADER);
	if (length < 0 || (size_t)length >= size)
		return 0U;
	used = (size_t)length;

	/* Each account with tries. */
	for (index = 0U; index < SESSIOND_COUNTS_MAX; index++) {
		count = &policy->counts[index];
		if (!count->used || count->pin_wrong == 0U)
			continue;
		length = snprintf(out + used, size - used, "%u %u\n", (unsigned)count->uid, count->pin_wrong);
		if (length < 0 || (size_t)length >= size - used)
			return 0U;
		used += (size_t)length;
	}

	/* Succeeded: the text. */
	return used;
}

/* Gives the delay of a failure's answer, in seconds. */
unsigned
sessiond_policy_delay(
	const struct sessiond_count *count)
{
	unsigned doublings;
	unsigned delay;

	/* 2 seconds, twice as long after every three failures in a row, at most 16. */
	delay = SESSIOND_DELAY_SECONDS;
	if (count->wrong == 0U)
		return delay;
	doublings = (count->wrong - 1U) / 3U;
	while (doublings > 0U && delay < SESSIOND_DELAY_MAX) {
		delay *= 2U;
		doublings--;
	}

	/* Succeeded: the delay. */
	return delay;
}

/* Gives the word the greeter or the session is told for passkey's reason. */
const char *
sessiond_policy_reason(
	const char *passkey_reason)
{
	size_t index;
	int match;

	/* The reason's entry. */
	for (index = 0U; index < sizeof(policy_reasons) / sizeof(policy_reasons[0]); index++) {
		match = strcmp(policy_reasons[index].passkey, passkey_reason);
		if (match == 0)
			return policy_reasons[index].told;
	}

	/* Any other reason tells nothing more than a wrong secret. */
	return "bad-secret";
}

/*
 * Writes the styles the account may use now ("password pin fido2"): those
 * passkey listed (comma-separated), without the PIN when it may not be
 * tried.
 */
void
sessiond_policy_styles(
	const char *listed,
	const struct sessiond_count *count,
	char *out,
	size_t size)
{
	char copy[128];
	char *word;
	char *cursor;
	const char *separator;
	size_t used;
	size_t length;
	int style;
	int allowed;

	/* Each listed style that may be tried, those that fit. */
	snprintf(copy, sizeof(copy), "%s", listed);
	used = 0U;
	out[0] = '\0';
	separator = "";
	cursor = copy;
	word = sessiond_policy_word(&cursor, ',');
	while (word != NULL) {
		style = sessiond_policy_style(word);
		allowed = 0;
		if (style >= 0)
			allowed = 1;
		if (style == SESSIOND_STYLE_PIN)
			allowed = sessiond_policy_pin_allowed(count);

		/* The style is written after a space, when it fits. */
		length = strlen(separator) + strlen(word);
		if (allowed && used + length < size) {
			snprintf(out + used, size - used, "%s%s", separator, word);
			used += length;
			separator = " ";
		}

		/* The next listed style. */
		word = sessiond_policy_word(&cursor, ',');
	}
}

/*
 * Tells whether a security key's failed login or unlock counts as a wrong
 * attempt (ws199-p001, review-3 R4): the key's PIN was wrong (passkey's
 * bad-key-pin, or bad-secret), the key is a clone, or its answer did not
 * verify; not a key that was not there, not touched, cancelled, busy or
 * broken.  Returns 1 when it counts.
 */
int
sessiond_policy_key_counts(
	const char *passkey_reason)
{
	static const char *const counted[] = {
		"bad-key-pin",
		"bad-secret",
		"cloned",
		"key-locked",
		"key-replug",
	};
	size_t index;
	int match;

	/* One of the words that count. */
	for (index = 0U; index < sizeof(counted) / sizeof(counted[0]); index++) {
		match = strcmp(counted[index], passkey_reason);
		if (match == 0)
			return 1;
	}

	/* Any other does not. */
	return 0;
}
