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
 * most 16.  The PIN is offered only after the account's password or
 * security key has been accepted since sessiond started, and is turned off
 * again by five wrong PINs in a row.
 */

#include "auth.h"

#include <stdio.h>
#include <string.h>

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
	/* Not before a password or a key since sessiond started. */
	if (!count->signed_in)
		return 0;

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

/* Clears the counts after a success: the password or a key also lets the PIN be offered again. */
void
sessiond_policy_success(
	struct sessiond_count *count,
	int style)
{
	count->wrong = 0U;
	count->pin_wrong = 0U;
	if (style != SESSIOND_STYLE_PIN)
		count->signed_in = 1;
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
