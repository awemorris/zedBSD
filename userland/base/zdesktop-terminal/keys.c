/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keys of zdesktop-terminal: evdev key codes to the bytes a shell reads.
 *
 * zwl forwards evdev codes with no keymap (userland/base/zwl/seat.c), so the
 * terminal carries its own layout: the US one, as the console's.  The keys
 * that are not characters send the xterm sequences.
 */

#include "terminal.h"

#include <string.h>

/* The evdev codes of the keys that are not in the character tables. */
#define KEY_ESC		1U
#define KEY_BACKSPACE	14U
#define KEY_TAB		15U
#define KEY_ENTER	28U
#define KEY_KPENTER	96U
#define KEY_HOME	102U
#define KEY_UP		103U
#define KEY_PAGEUP	104U
#define KEY_LEFT	105U
#define KEY_RIGHT	106U
#define KEY_END		107U
#define KEY_DOWN	108U
#define KEY_PAGEDOWN	109U
#define KEY_INSERT	110U
#define KEY_DELETE	111U
#define KEY_F1		59U
#define KEY_F10		68U
#define KEY_F11		87U
#define KEY_F12		88U

/* How many codes the character tables cover (up to the space bar). */
#define KEYS_TABLE_SIZE	58U

/*
 * The character each key types without shift, by evdev code; 0 for a key
 * that types none.  The US layout.
 */
static const char keys_plain[KEYS_TABLE_SIZE] = {
	0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0,
	'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
	'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

/* The character each key types with shift, by evdev code. */
static const char keys_shifted[KEYS_TABLE_SIZE] = {
	0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
	'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0,
	'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
	'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

/*
 * The xterm sequences of F1 to F12.
 */
static const char *const keys_function[12] = {
	"\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~", "\033[17~",
	"\033[18~", "\033[19~", "\033[20~", "\033[21~", "\033[23~", "\033[24~"
};

static size_t keys_copy(const char *sequence, unsigned char *bytes, size_t size);

/*
 * Writes the bytes a key press sends to the shell and returns how many.
 *
 * Returns 0 for a key that sends nothing (a modifier, an unknown key) or
 * when the bytes do not fit.
 */
size_t
terminal_key_bytes(
	uint32_t key,
	uint32_t modifiers,
	unsigned char *bytes,
	size_t size)
{
	char character;
	size_t length;

	/* The keys that send sequences or control characters of their own. */
	switch (key) {
	case KEY_ESC:
		return keys_copy("\033", bytes, size);
	case KEY_BACKSPACE:
		return keys_copy("\177", bytes, size);
	case KEY_TAB:
		return keys_copy("\t", bytes, size);
	case KEY_ENTER:
	case KEY_KPENTER:
		return keys_copy("\r", bytes, size);
	case KEY_UP:
		return keys_copy("\033[A", bytes, size);
	case KEY_DOWN:
		return keys_copy("\033[B", bytes, size);
	case KEY_RIGHT:
		return keys_copy("\033[C", bytes, size);
	case KEY_LEFT:
		return keys_copy("\033[D", bytes, size);
	case KEY_HOME:
		return keys_copy("\033[H", bytes, size);
	case KEY_END:
		return keys_copy("\033[F", bytes, size);
	case KEY_INSERT:
		return keys_copy("\033[2~", bytes, size);
	case KEY_DELETE:
		return keys_copy("\033[3~", bytes, size);
	case KEY_PAGEUP:
		return keys_copy("\033[5~", bytes, size);
	case KEY_PAGEDOWN:
		return keys_copy("\033[6~", bytes, size);
	case KEY_F11:
		return keys_copy(keys_function[10], bytes, size);
	case KEY_F12:
		return keys_copy(keys_function[11], bytes, size);
	default:
		break;
	}

	/* F1 to F10 are consecutive codes. */
	if (key >= KEY_F1 && key <= KEY_F10)
		return keys_copy(keys_function[key - KEY_F1], bytes, size);

	/* Every other key sending anything is in the character tables. */
	if (key >= KEYS_TABLE_SIZE)
		return 0U;

	/* The character, shifted or not. */
	character = keys_plain[key];
	if ((modifiers & TERMINAL_MODIFIER_SHIFT) != 0U)
		character = keys_shifted[key];
	if (character == 0)
		return 0U;

	/* Control turns a letter and a few symbols into the control character (Ctrl-C is 3). */
	if ((modifiers & TERMINAL_MODIFIER_CONTROL) != 0U) {
		if (character >= 'a' && character <= 'z') {
			character = (char)(character - 'a' + 1);
		} else if (character >= 'A' && character <= 'Z') {
			character = (char)(character - 'A' + 1);
		} else if (character == '[' || character == '{') {
			character = 0x1b;
		} else if (character == '\\' || character == '|') {
			character = 0x1c;
		} else if (character == ']' || character == '}') {
			character = 0x1d;
		} else if (character == ' ' || character == '@' || character == '2') {
			character = 0;
		}
	}

	/* Alt sends ESC before the character, as xterm does. */
	length = 0U;
	if ((modifiers & TERMINAL_MODIFIER_ALT) != 0U) {
		if (size < 1U)
			return 0U;
		bytes[length] = 0x1bU;
		length++;
	}

	/* The character itself. */
	if (length >= size)
		return 0U;
	bytes[length] = (unsigned char)character;
	length++;

	/* Succeeded: the bytes of the key. */
	return length;
}

/* Copies a sequence into the buffer and returns its length, or 0 when it does not fit. */
static size_t
keys_copy(
	const char *sequence,
	unsigned char *bytes,
	size_t size)
{
	size_t length;

	/* A sequence longer than the buffer is not sent in part. */
	length = strlen(sequence);
	if (length > size)
		return 0U;

	/* Succeeded: the sequence's bytes. */
	memcpy(bytes, sequence, length);
	return length;
}
