/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rules of THERMAL (thermal-rules.h says what they are).
 *
 * A request is one of
 *
 *   strategy performance|balanced|powersave
 *   strategy manual SPEED NOISE GATING
 *   boost all|FAN LEVEL
 *
 * with words separated by one space and nothing else on the line; the
 * numbers are decimal, without a sign or a leading zero, each axis
 * 0..100, a fan 0..255 and a level 0..255.  Whether a fan exists and how
 * many boost levels it has is the kernel's to say.  Only root or a member
 * of wheel may ask, as for POWER and SERVICE; the words are checked first,
 * so a malformed request is ERROR for anyone.
 *
 * The answer is one line: OK, with the words partial, pending and
 * superseded for what the kernel reported beyond its success; FAIL wheel;
 * or ERROR and a word for the failure (invalid, unsupported, nofan,
 * unavailable, failed).
 */

#include "thermal-rules.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The most words a request has (strategy manual SPEED NOISE GATING). */
#define RULES_WORDS_MAX		5U

/* The most digits a number of a request has. */
#define RULES_DIGITS_MAX	3U

/*
 * A request cut into its words: a copy of the line with each separating
 * space replaced by an end, and where each word starts.
 */
struct rules_words {
	char line[SESSIOND_THERMAL_REQUEST_MAX];
	const char *word[RULES_WORDS_MAX];
	unsigned count;
};

static int rules_split(const char *arguments, struct rules_words *words);
static int rules_number(const char *word, unsigned max, unsigned *value);
static int rules_strategy(const struct rules_words *words, struct sessiond_thermal_request *request);
static int rules_boost(const struct rules_words *words, struct sessiond_thermal_request *request);
static const char *rules_failure(int error);

/*
 * Decides a THERMAL request's arguments (the line after "THERMAL ") for a
 * user (in_wheel: whether the user is a member of wheel).  Returns 0 with
 * the request, EINVAL for words that are not a request, or EPERM for a
 * user who may not ask.
 */
int
sessiond_thermal_decide(
	const char *arguments,
	uid_t uid,
	int in_wheel,
	struct sessiond_thermal_request *request)
{
	struct rules_words words;
	int differs;
	int error;

	/* The words, one space apart. */
	memset(request, 0, sizeof(*request));
	error = rules_split(arguments, &words);
	if (error != 0)
		return error;

	/* A strategy, or a boost; nothing else. */
	differs = strcmp(words.word[0], "strategy");
	if (differs == 0) {
		error = rules_strategy(&words, request);
	} else {
		differs = strcmp(words.word[0], "boost");
		if (differs != 0)
			return EINVAL;
		error = rules_boost(&words, request);
	}

	/* Words that do not make one of the two. */
	if (error != 0) {
		memset(request, 0, sizeof(*request));
		return error;
	}

	/* Root, or a member of wheel. */
	if (uid != 0 && !in_wheel) {
		memset(request, 0, sizeof(*request));
		return EPERM;
	}

	/* Succeeded: the request may be carried out. */
	return 0;
}

/*
 * Writes the line that answers a request (with its newline and end) from
 * the errno it came to and, on success, the SESSIOND_THERMAL_* bits the
 * kernel reported.  Returns the length of the line, or 0 when it does not
 * fit in size.
 */
int
sessiond_thermal_answer(
	int error,
	unsigned flags,
	char *answer,
	size_t size)
{
	const char *partial;
	const char *pending;
	const char *superseded;
	const char *reason;
	int length;

	/* A refusal or a failure says which. */
	if (error != 0) {
		reason = rules_failure(error);
		length = snprintf(answer, size, "%s\n", reason);
		if (length < 0 || (size_t)length >= size)
			return 0;

		/* Succeeded: the failure's line. */
		return length;
	}

	/* Each bit the kernel reported is one word after OK. */
	partial = "";
	if ((flags & SESSIOND_THERMAL_PARTIAL) != 0U)
		partial = " partial";
	pending = "";
	if ((flags & SESSIOND_THERMAL_PENDING) != 0U)
		pending = " pending";
	superseded = "";
	if ((flags & SESSIOND_THERMAL_SUPERSEDED) != 0U)
		superseded = " superseded";

	/* The success line. */
	length = snprintf(answer, size, "OK%s%s%s\n", partial, pending, superseded);
	if (length < 0 || (size_t)length >= size)
		return 0;

	/* Succeeded: the success line. */
	return length;
}

/*
 * Cuts a request into its words.  Returns EINVAL for a line too long, an
 * empty word (a space at either end or two together), or too many words.
 */
static int
rules_split(
	const char *arguments,
	struct rules_words *words)
{
	size_t length;
	size_t index;
	char *start;

	/* A copy that fits, and is not empty. */
	memset(words, 0, sizeof(*words));
	length = strlen(arguments);
	if (length == 0U || length >= sizeof(words->line))
		return EINVAL;
	memcpy(words->line, arguments, length + 1U);

	/* Cuts the copy at each space; every word has at least one letter. */
	start = words->line;
	for (index = 0; index <= length; index++) {
		if (words->line[index] != ' ' && words->line[index] != '\0')
			continue;

		/* An empty word: a space at an end, or two together. */
		if (&words->line[index] == start)
			return EINVAL;

		/* One word more than a request has. */
		if (words->count == RULES_WORDS_MAX)
			return EINVAL;

		/* The word ends here and the next starts after the space. */
		words->line[index] = '\0';
		words->word[words->count] = start;
		words->count++;
		start = &words->line[index + 1U];
	}

	/* Succeeded: at least one word. */
	return 0;
}

/*
 * Reads one number of a request: decimal digits only, no leading zero
 * but in 0 itself, at most RULES_DIGITS_MAX digits and at most max.
 * Returns 0 with the value, or EINVAL.
 */
static int
rules_number(
	const char *word,
	unsigned max,
	unsigned *value)
{
	size_t length;
	size_t index;
	unsigned number;

	/* One to three digits, and no leading zero. */
	length = strlen(word);
	if (length == 0U || length > RULES_DIGITS_MAX)
		return EINVAL;
	if (word[0] == '0' && length > 1U)
		return EINVAL;

	/* The value, one digit at a time. */
	number = 0;
	for (index = 0; index < length; index++) {
		if (word[index] < '0' || word[index] > '9')
			return EINVAL;
		number = number * 10U + (unsigned)(word[index] - '0');
	}

	/* No more than the largest the request may name. */
	if (number > max)
		return EINVAL;

	/* Succeeded: the number. */
	*value = number;
	return 0;
}

/*
 * Decides the words of a strategy: one of the three named strategies
 * alone, or manual with its three axes.  Returns 0 or EINVAL.
 */
static int
rules_strategy(
	const struct rules_words *words,
	struct sessiond_thermal_request *request)
{
	static const struct {
		const char *name;
		unsigned value;
	} named[] = {
		{ "performance", SESSIOND_THERMAL_PERFORMANCE },
		{ "balanced", SESSIOND_THERMAL_BALANCED },
		{ "powersave", SESSIOND_THERMAL_POWERSAVE }
	};
	size_t index;
	int differs;
	int error;

	/* The strategy's word. */
	if (words->count < 2U)
		return EINVAL;
	request->kind = SESSIOND_THERMAL_STRATEGY;

	/* A named strategy takes no axes. */
	for (index = 0; index < sizeof(named) / sizeof(named[0]); index++) {
		differs = strcmp(words->word[1], named[index].name);
		if (differs != 0)
			continue;

		/* Words after the name are not part of it. */
		if (words->count != 2U)
			return EINVAL;

		/* Succeeded: the named strategy. */
		request->strategy = named[index].value;
		return 0;
	}

	/* Otherwise manual, with exactly its three axes. */
	differs = strcmp(words->word[1], "manual");
	if (differs != 0)
		return EINVAL;
	if (words->count != 5U)
		return EINVAL;
	request->strategy = SESSIOND_THERMAL_MANUAL;

	/* The speed, the noise and the gating, in that order. */
	error = rules_number(words->word[2], SESSIOND_THERMAL_AXIS_MAX, &request->speed);
	if (error != 0)
		return error;
	error = rules_number(words->word[3], SESSIOND_THERMAL_AXIS_MAX, &request->noise);
	if (error != 0)
		return error;
	error = rules_number(words->word[4], SESSIOND_THERMAL_AXIS_MAX, &request->gating);
	if (error != 0)
		return error;

	/* Succeeded: manual with its axes. */
	return 0;
}

/*
 * Decides the words of a boost: every fan or one by its number, and the
 * level.  Returns 0 or EINVAL.
 */
static int
rules_boost(
	const struct rules_words *words,
	struct sessiond_thermal_request *request)
{
	unsigned fan;
	unsigned level;
	int differs;
	int error;

	/* The fan and the level, and nothing after. */
	if (words->count != 3U)
		return EINVAL;
	request->kind = SESSIOND_THERMAL_BOOST;

	/* Every fan, or the one the number names. */
	differs = strcmp(words->word[1], "all");
	if (differs == 0) {
		request->fan = SESSIOND_THERMAL_FAN_ALL;
	} else {
		error = rules_number(words->word[1], SESSIOND_THERMAL_FAN_MAX, &fan);
		if (error != 0)
			return error;
		request->fan = fan;
	}

	/* The level the fan is to stay at or above. */
	error = rules_number(words->word[2], SESSIOND_THERMAL_LEVEL_MAX, &level);
	if (error != 0)
		return error;
	request->level = level;

	/* Succeeded: the boost. */
	return 0;
}

/* Names a refusal or a failure for its answer line. */
static const char *
rules_failure(
	int error)
{
	/* The session's user is neither root nor a member of wheel. */
	if (error == EPERM)
		return "FAIL wheel";

	/* Words that are not a request, or values the kernel refused. */
	if (error == EINVAL)
		return "ERROR invalid";

	/* Nothing on this machine acts on the request. */
	if (error == EOPNOTSUPP)
		return "ERROR unsupported";

	/* The fan named is not there. */
	if (error == ENOENT)
		return "ERROR nofan";

	/* The kernel's device for heat and power is not there. */
	if (error == ENODEV)
		return "ERROR unavailable";

	/* Any other failure. */
	return "ERROR failed";
}
