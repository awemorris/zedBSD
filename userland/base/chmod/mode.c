/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Computes file mode bits from a chmod mode operand.
 *
 * A mode is an octal number or a symbolic mode: comma-separated clauses,
 * each a list of who letters (u, g, o, a) and one or more actions.  An
 * action is an operator (+, - or =) and either permission letters (r, w,
 * x, X, s, t) or the letter of a class whose current bits are copied (u,
 * g, o).  Without who letters the action applies to all classes but
 * leaves the bits of the file mode creation mask alone.
 *
 * A directory keeps its set-user-ID and set-group-ID bits unless an
 * action names s or an octal mode of five or more digits is given, as on
 * other systems: those bits of a directory decide the group of new files
 * and are rarely meant to be dropped by a plain mode.
 */

#include "userland/base/chmod/mode.h"
#include <sys/stat.h>

/* The set-user-ID and set-group-ID bits. */
#define MODE_SET_ID (S_ISUID | S_ISGID)

/* Every bit a mode sets: the permission bits and the three special bits. */
#define MODE_ALL_BITS 07777

/* The number of octal digits from which a directory's set-ID bits follow the mode. */
#define MODE_EXPLICIT_DIGITS 5

static int apply_numeric(const char *text, mode_t original, int directory, mode_t *result);
static int apply_symbolic(const char *text, mode_t original, mode_t mask, int directory, mode_t *result);
static int apply_action(const char **cursor, mode_t *mode, mode_t who, int who_given, mode_t mask, int directory);
static mode_t read_permissions(const char **cursor, mode_t mode, int directory, int *names_set_id);
static mode_t class_bits(int letter);
static mode_t copy_class(mode_t mode, int letter);
static int is_octal_digit(int letter);

/*
 * Tells whether a mode operand is a valid octal or symbolic mode.
 */
int
mode_valid(
	const char *text)
{
	mode_t ignored;
	int status;

	/* A mode is valid when it can be applied at all. */
	status = mode_apply(text, 0, 0, 0, &ignored);
	if (status != 0)
		return 0;

	/* Succeeded: the mode is valid. */
	return 1;
}

/*
 * Computes the mode bits a mode operand gives a file whose bits are now
 * original.  mask is the file mode creation mask and directory says
 * whether the file is a directory.  Returns -1 for an invalid mode.
 */
int
mode_apply(
	const char *text,
	mode_t original,
	mode_t mask,
	int directory,
	mode_t *result)
{
	int status;
	int numeric;

	/* An operand starting with a digit is an octal mode. */
	numeric = is_octal_digit(text[0]);
	if (text[0] >= '8' && text[0] <= '9')
		return -1;
	if (numeric)
		status = apply_numeric(text, original, directory, result);
	else
		status = apply_symbolic(text, original, mask, directory, result);

	/* Refuses an invalid mode. */
	if (status != 0)
		return -1;

	/* Succeeded: the new bits. */
	return 0;
}

/*
 * Applies an octal mode.  A directory keeps its set-ID bits unless the
 * mode has five or more digits.
 */
static int
apply_numeric(
	const char *text,
	mode_t original,
	int directory,
	mode_t *result)
{
	const char *digit;
	unsigned long value;
	int count;
	int octal;

	/* Reads every digit; anything else makes the mode invalid. */
	value = 0;
	count = 0;
	for (digit = text; *digit != '\0'; digit++) {
		octal = is_octal_digit(*digit);
		if (!octal)
			return -1;
		value = value * 8 + (unsigned long)(*digit - '0');
		count++;

		/* A value beyond the mode bits is refused. */
		if (value > MODE_ALL_BITS)
			return -1;
	}

	/* The mode is the value, with a directory's set-ID bits kept. */
	*result = (mode_t)value;
	if (directory && count < MODE_EXPLICIT_DIGITS)
		*result |= original & MODE_SET_ID;

	/* Succeeded: the new bits. */
	return 0;
}

/* Applies a symbolic mode, clause by clause. */
static int
apply_symbolic(
	const char *text,
	mode_t original,
	mode_t mask,
	int directory,
	mode_t *result)
{
	const char *cursor;
	mode_t mode;
	mode_t who;
	mode_t letter_bits;
	int who_given;
	int status;

	/* Starts from the current bits. */
	mode = original & MODE_ALL_BITS;
	cursor = text;

	/* Applies each clause in turn. */
	for (;;) {
		/* Reads the who letters of the clause. */
		who = 0;
		who_given = 0;
		for (;;) {
			letter_bits = class_bits(*cursor);
			if (letter_bits == 0 && *cursor != 'a')
				break;
			if (*cursor == 'a')
				letter_bits = MODE_ALL_BITS;
			who |= letter_bits;
			who_given = 1;
			cursor++;
		}

		/* A clause needs at least one action. */
		if (*cursor != '+' && *cursor != '-' && *cursor != '=')
			return -1;

		/* Applies each action of the clause. */
		while (*cursor == '+' || *cursor == '-' || *cursor == '=') {
			status = apply_action(&cursor, &mode, who, who_given, mask, directory);
			if (status != 0)
				return -1;
		}

		/* The mode ends here, or a comma starts the next clause. */
		if (*cursor == '\0')
			break;
		if (*cursor != ',')
			return -1;
		cursor++;

		/* A comma must be followed by a clause. */
		if (*cursor == '\0')
			return -1;
	}

	/* Succeeded: the new bits. */
	*result = mode;
	return 0;
}

/*
 * Applies one action at the cursor, moving the cursor past it.  who holds
 * the bits of the classes named, or nothing when who_given is 0.
 */
static int
apply_action(
	const char **cursor,
	mode_t *mode,
	mode_t who,
	int who_given,
	mode_t mask,
	int directory)
{
	mode_t value;
	mode_t affected;
	mode_t copied;
	int operation;
	int names_set_id;

	/* Reads the operator. */
	operation = **cursor;
	(*cursor)++;

	/* Reads the bits: another class's current bits, or permission letters. */
	names_set_id = 0;
	copied = copy_class(*mode, **cursor);
	if (copied != (mode_t)-1) {
		value = copied;
		(*cursor)++;
	} else {
		value = read_permissions(cursor, *mode, directory, &names_set_id);
	}

	/*
	 * Named classes limit the action to their bits; without them the
	 * action covers every class but not the bits of the creation mask.
	 */
	if (who_given) {
		affected = who;
		value &= who;
	} else {
		affected = MODE_ALL_BITS;
		value &= ~(mask & 0777);
	}

	/* Adds, removes or sets the bits. */
	if (operation == '+') {
		*mode |= value;
	} else if (operation == '-') {
		*mode &= ~value;
	} else {
		/* = keeps a directory's set-ID bits unless it names s. */
		if (directory && !names_set_id)
			affected &= ~(mode_t)MODE_SET_ID;
		*mode = (*mode & ~affected) | value;
	}

	/* Succeeded: the action was applied. */
	return 0;
}

/*
 * Reads permission letters at the cursor into bits for every class, moving
 * the cursor past them.  X counts only for a directory or a file with an
 * execute bit already set.
 */
static mode_t
read_permissions(
	const char **cursor,
	mode_t mode,
	int directory,
	int *names_set_id)
{
	mode_t value;
	int more;

	/* Reads letters until one that is not a permission. */
	value = 0;
	more = 1;
	while (more) {
		/* Adds the bits a letter stands for. */
		switch (**cursor) {
		case 'r':
			value |= S_IRUSR | S_IRGRP | S_IROTH;
			break;
		case 'w':
			value |= S_IWUSR | S_IWGRP | S_IWOTH;
			break;
		case 'x':
			value |= S_IXUSR | S_IXGRP | S_IXOTH;
			break;
		case 'X':
			/* Execute only where searching or running already makes sense. */
			if (directory || (mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0)
				value |= S_IXUSR | S_IXGRP | S_IXOTH;
			break;
		case 's':
			value |= MODE_SET_ID;
			*names_set_id = 1;
			break;
		case 't':
			value |= S_ISVTX;
			break;
		default:
			more = 0;
			break;
		}

		/* Moves past a letter that was a permission. */
		if (more)
			(*cursor)++;
	}

	/* Reports the bits. */
	return value;
}

/* Gives the bits of the class a who letter names, or 0 for another letter. */
static mode_t
class_bits(
	int letter)
{
	/* Each class owns its three permission bits and one special bit. */
	switch (letter) {
	case 'u':
		return S_ISUID | S_IRWXU;
	case 'g':
		return S_ISGID | S_IRWXG;
	case 'o':
		return S_ISVTX | S_IRWXO;
	default:
		return 0;
	}
}

/*
 * Copies the permission bits of the class a letter names to every class,
 * or gives (mode_t)-1 when the letter names no class.
 */
static mode_t
copy_class(
	mode_t mode,
	int letter)
{
	mode_t bits;

	/* Takes the three bits of the class. */
	switch (letter) {
	case 'u':
		bits = (mode & S_IRWXU) >> 6;
		break;
	case 'g':
		bits = (mode & S_IRWXG) >> 3;
		break;
	case 'o':
		bits = mode & S_IRWXO;
		break;
	default:
		return (mode_t)-1;
	}

	/* Succeeded: the same bits in all three classes. */
	return (bits << 6) | (bits << 3) | bits;
}

/* Tells whether a character is an octal digit. */
static int
is_octal_digit(
	int letter)
{
	/* 0 to 7 are the octal digits. */
	if (letter >= '0' && letter <= '7')
		return 1;

	/* Anything else is not. */
	return 0;
}
