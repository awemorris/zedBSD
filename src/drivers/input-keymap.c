/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/input-keymap.h"

#include <string.h>
#include <zedbsd/input.h>

struct symbol_entry {
	const char *name;
	uint16_t evdev;
	uint16_t legacy;
	char normal;
	char shifted;
};

static const uint16_t letter_codes[26] = {
	KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
	KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
	KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
};

static const uint16_t digit_codes[10] = {
	KEY_0, KEY_1, KEY_2, KEY_3, KEY_4,
	KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
};

static const char shifted_digits[10] = {
	')', '!', '@', '#', '$', '%', '^', '&', '*', '(',
};

static const struct symbol_entry symbols[] = {
	{"esc", KEY_ESC, INPUT_KEY_ESCAPE, 0, 0},
	{"backspace", KEY_BACKSPACE, INPUT_KEY_BACKSPACE, 0, 0},
	{"tab", KEY_TAB, INPUT_KEY_TAB, 0, 0},
	{"enter", KEY_ENTER, INPUT_KEY_ENTER, 0, 0},
	{"space", KEY_SPACE, 0, ' ', ' '},
	{"minus", KEY_MINUS, 0, '-', '_'},
	{"equal", KEY_EQUAL, 0, '=', '+'},
	{"leftbrace", KEY_LEFTBRACE, 0, '[', '{'},
	{"rightbrace", KEY_RIGHTBRACE, 0, ']', '}'},
	{"semicolon", KEY_SEMICOLON, 0, ';', ':'},
	{"apostrophe", KEY_APOSTROPHE, 0, '\'', '"'},
	{"grave", KEY_GRAVE, 0, '`', '~'},
	{"backslash", KEY_BACKSLASH, 0, '\\', '|'},
	{"comma", KEY_COMMA, 0, ',', '<'},
	{"dot", KEY_DOT, 0, '.', '>'},
	{"slash", KEY_SLASH, 0, '/', '?'},
	{"jis-1", KEY_1, 0, '1', '!'},
	{"jis-2", KEY_2, 0, '2', '"'},
	{"jis-3", KEY_3, 0, '3', '#'},
	{"jis-4", KEY_4, 0, '4', '$'},
	{"jis-5", KEY_5, 0, '5', '%'},
	{"jis-6", KEY_6, 0, '6', '&'},
	{"jis-7", KEY_7, 0, '7', '\''},
	{"jis-8", KEY_8, 0, '8', '('},
	{"jis-9", KEY_9, 0, '9', ')'},
	{"jis-0", KEY_0, 0, '0', '0'},
	{"jis-minus", KEY_MINUS, 0, '-', '='},
	{"jis-caret", KEY_EQUAL, 0, '^', '~'},
	{"jis-yen", KEY_RESERVED, 0, '\\', '|'},
	{"jis-at", KEY_LEFTBRACE, 0, '@', '`'},
	{"jis-lbrace", KEY_RIGHTBRACE, 0, '[', '{'},
	{"jis-semi", KEY_SEMICOLON, 0, ';', '+'},
	{"jis-colon", KEY_APOSTROPHE, 0, ':', '*'},
	{"jis-rbrace", KEY_BACKSLASH, 0, ']', '}'},
	{"jis-comma", KEY_COMMA, 0, ',', '<'},
	{"jis-dot", KEY_DOT, 0, '.', '>'},
	{"jis-slash", KEY_SLASH, 0, '/', '?'},
	{"jis-ro", KEY_RESERVED, 0, '\\', '_'},
	{"jis-kp-slash", KEY_RESERVED, 0, '/', '/'},
	{"jis-kp-star", KEY_RESERVED, 0, '*', '*'},
	{"jis-kp-minus", KEY_RESERVED, 0, '-', '-'},
	{"jis-kp-7", KEY_RESERVED, 0, '7', '7'},
	{"jis-kp-8", KEY_RESERVED, 0, '8', '8'},
	{"jis-kp-9", KEY_RESERVED, 0, '9', '9'},
	{"jis-kp-plus", KEY_RESERVED, 0, '+', '+'},
	{"jis-kp-4", KEY_RESERVED, 0, '4', '4'},
	{"jis-kp-5", KEY_RESERVED, 0, '5', '5'},
	{"jis-kp-6", KEY_RESERVED, 0, '6', '6'},
	{"jis-kp-equal", KEY_RESERVED, 0, '=', '='},
	{"jis-kp-1", KEY_RESERVED, 0, '1', '1'},
	{"jis-kp-2", KEY_RESERVED, 0, '2', '2'},
	{"jis-kp-3", KEY_RESERVED, 0, '3', '3'},
	{"jis-kp-enter", KEY_RESERVED, INPUT_KEY_ENTER, 0, 0},
	{"jis-kp-0", KEY_RESERVED, 0, '0', '0'},
	{"jis-kp-comma", KEY_RESERVED, 0, ',', ','},
	{"jis-kp-dot", KEY_RESERVED, 0, '.', '.'},
	{"leftshift", KEY_LEFTSHIFT, INPUT_KEY_SHIFT_SYMBOL, 0, 0},
	{"rightshift", KEY_RIGHTSHIFT, INPUT_KEY_SHIFT_SYMBOL, 0, 0},
	{"leftctrl", KEY_LEFTCTRL, INPUT_KEY_CTRL_SYMBOL, 0, 0},
	{"rightctrl", KEY_RIGHTCTRL, INPUT_KEY_CTRL_SYMBOL, 0, 0},
	{"leftalt", KEY_LEFTALT, INPUT_KEY_GRAPH_SYMBOL, 0, 0},
	{"rightalt", KEY_RIGHTALT, INPUT_KEY_GRAPH_SYMBOL, 0, 0},
	{"capslock", KEY_CAPSLOCK, INPUT_KEY_CAPS_LOCK, 0, 0},
	{"kana", KEY_RESERVED, INPUT_KEY_KANA, 0, 0},
	{"home", KEY_HOME, INPUT_KEY_HOME, 0, 0},
	{"up", KEY_UP, INPUT_KEY_UP, 0, 0},
	{"pageup", KEY_PAGEUP, INPUT_KEY_PAGE_UP, 0, 0},
	{"left", KEY_LEFT, INPUT_KEY_LEFT, 0, 0},
	{"right", KEY_RIGHT, INPUT_KEY_RIGHT, 0, 0},
	{"end", KEY_END, INPUT_KEY_END, 0, 0},
	{"down", KEY_DOWN, INPUT_KEY_DOWN, 0, 0},
	{"pagedown", KEY_PAGEDOWN, INPUT_KEY_PAGE_DOWN, 0, 0},
	{"insert", KEY_INSERT, INPUT_KEY_INSERT, 0, 0},
	{"delete", KEY_DELETE, INPUT_KEY_DELETE, 0, 0},
};

static const struct symbol_entry *
find_symbol(const char *name)
{
	unsigned index;
	for (index = 0; index < sizeof(symbols) / sizeof(symbols[0]); index++)
		if (strcmp(name, symbols[index].name) == 0)
			return &symbols[index];
	return NULL;
}

static int
function_number(const char *symbol)
{
	if (symbol[0] != 'f')
		return 0;
	if (symbol[1] >= '1' && symbol[1] <= '9' && symbol[2] == '\0')
		return symbol[1] - '0';
	if (strcmp(symbol, "f10") == 0)
		return 10;
	return 0;
}

void
input_keymap_init(struct input_keymap_state *state)
{
	memset(state, 0, sizeof(*state));
}

uint16_t
input_key_from_symbol(const char *symbol)
{
	const struct symbol_entry *entry;
	int number;
	if (symbol == NULL || symbol[0] == '\0')
		return KEY_RESERVED;
	if (symbol[1] == '\0') {
		unsigned char original = (unsigned char)symbol[0];
		char value = symbol[0];
		switch (original) {
		case 0x08: return KEY_BACKSPACE;
		case 0x09: return KEY_TAB;
		case 0x0a:
		case 0x0d: return KEY_ENTER;
		case 0x1b: return KEY_ESC;
		default: break;
		}
		if (value >= 'A' && value <= 'Z')
			value = (char)(value - 'A' + 'a');
		if (value >= 'a' && value <= 'z')
			return letter_codes[value - 'a'];
		if (value >= '0' && value <= '9')
			return digit_codes[value - '0'];
		for (unsigned index = 0;
		     index < sizeof(shifted_digits) / sizeof(shifted_digits[0]);
		     index++)
			if (value == shifted_digits[index])
				return digit_codes[index];
		switch (value) {
		case ' ': return KEY_SPACE;
		case '-': case '_': return KEY_MINUS;
		case '=': case '+': return KEY_EQUAL;
		case '[': case '{': return KEY_LEFTBRACE;
		case ']': case '}': return KEY_RIGHTBRACE;
		case ';': case ':': return KEY_SEMICOLON;
		case '\'': case '"': return KEY_APOSTROPHE;
		case '`': case '~': return KEY_GRAVE;
		case '\\': case '|': return KEY_BACKSLASH;
		case ',': case '<': return KEY_COMMA;
		case '.': case '>': return KEY_DOT;
		case '/': case '?': return KEY_SLASH;
		default: break;
		}
	}
	number = function_number(symbol);
	if (number != 0)
		return (uint16_t)(KEY_F1 + number - 1);
	entry = find_symbol(symbol);
	return entry != NULL ? entry->evdev : KEY_RESERVED;
}

int
input_key_symbol_supported(const char *symbol)
{
	if (symbol == NULL || symbol[0] == '\0')
		return 0;
	if (symbol[1] == '\0')
		return 1;
	return function_number(symbol) != 0 || find_symbol(symbol) != NULL;
}

int
input_keymap_event_from_code(uint16_t code, int32_t value,
	struct hal_key_event *event)
{
	const struct symbol_entry *entry = NULL;
	const char *symbol = NULL;
	char character[2];
	unsigned index;

	if (event == NULL || (value != 0 && value != 1 && value != 2))
		return 0;
	memset(event, 0, sizeof(*event));
	for (index = 0;
	     index < sizeof(letter_codes) / sizeof(letter_codes[0]); index++)
		if (letter_codes[index] == code) {
			character[0] = (char)('a' + index);
			character[1] = '\0';
			symbol = character;
			break;
		}
	if (symbol == NULL)
		for (index = 0;
		     index < sizeof(digit_codes) / sizeof(digit_codes[0]);
		     index++)
			if (digit_codes[index] == code) {
				character[0] = (char)('0' + index);
				character[1] = '\0';
				symbol = character;
				break;
			}
	if (symbol == NULL && code >= KEY_F1 && code <= KEY_F10) {
		static const char *const functions[] = {
		    "f1", "f2", "f3", "f4", "f5",
		    "f6", "f7", "f8", "f9", "f10"};
		symbol = functions[code - KEY_F1];
	}
	if (symbol == NULL) {
		for (index = 0; index < sizeof(symbols) / sizeof(symbols[0]);
		     index++)
			if (symbols[index].evdev == code) {
				entry = &symbols[index];
				break;
			}
		symbol = entry != NULL ? entry->name : NULL;
	}
	if (symbol == NULL)
		return 0;
	for (index = 0; index + 1U < HAL_KEY_SYMBOL_SIZE &&
	     symbol[index] != '\0'; index++)
		event->symbol[index] = symbol[index];
	event->flags = value == 0 ? HAL_KEY_EVENT_RELEASE :
	    value == 2 ? HAL_KEY_EVENT_REPEAT : HAL_KEY_EVENT_PRESS;
	return 1;
}

static void
update_modifier(struct input_keymap_state *state, const char *symbol,
	int down, int press)
{
	if (strcmp(symbol, "leftshift") == 0)
		state->left_shift = (uint8_t)down;
	else if (strcmp(symbol, "rightshift") == 0)
		state->right_shift = (uint8_t)down;
	else if (strcmp(symbol, "leftctrl") == 0)
		state->left_control = (uint8_t)down;
	else if (strcmp(symbol, "rightctrl") == 0)
		state->right_control = (uint8_t)down;
	else if (strcmp(symbol, "leftalt") == 0)
		state->left_graph = (uint8_t)down;
	else if (strcmp(symbol, "rightalt") == 0)
		state->right_graph = (uint8_t)down;
	else if (strcmp(symbol, "capslock") == 0 && press)
		state->caps_lock ^= 1U;
	else if (strcmp(symbol, "kana") == 0 && press)
		state->kana_lock ^= 1U;
}

int
input_keymap_translate(struct input_keymap_state *state,
	const struct hal_key_event *event, uint32_t *result)
{
	const struct symbol_entry *entry;
	uint32_t key = 0, modifiers;
	int number, release, press, shift, control, graph;
	char value;
	if (state == NULL || event == NULL || result == NULL ||
	    event->symbol[HAL_KEY_SYMBOL_SIZE - 1U] != '\0')
		return 0;
	release = (event->flags & HAL_KEY_EVENT_RELEASE) != 0;
	press = (event->flags & HAL_KEY_EVENT_PRESS) != 0;
	if (event->flags != HAL_KEY_EVENT_PRESS &&
	    event->flags != HAL_KEY_EVENT_RELEASE &&
	    event->flags != HAL_KEY_EVENT_REPEAT)
		return 0;
	update_modifier(state, event->symbol, !release, press);
	shift = state->left_shift || state->right_shift;
	control = state->left_control || state->right_control;
	graph = state->left_graph || state->right_graph;
	if (event->symbol[0] != '\0' && event->symbol[1] == '\0') {
		value = event->symbol[0];
		if (value >= 'a' && value <= 'z' &&
		    (shift ^ state->caps_lock))
			value = (char)(value - 'a' + 'A');
		else if (value >= '0' && value <= '9' && shift)
			value = shifted_digits[value - '0'];
		key = (uint8_t)value;
	} else if ((number = function_number(event->symbol)) != 0) {
		key = INPUT_KEY_F1 + (uint32_t)number - 1U;
	} else if ((entry = find_symbol(event->symbol)) != NULL) {
		key = entry->legacy != 0 ? entry->legacy :
		    (uint8_t)(shift && entry->shifted != 0 ?
		    entry->shifted : entry->normal);
	}
	if (key == 0)
		return 0;
	modifiers = (shift ? INPUT_KEY_SHIFT : 0U) |
	    (control ? INPUT_KEY_CTRL : 0U) |
	    (graph ? INPUT_KEY_GRAPH : 0U);
	*result = (key & INPUT_KEY_MASK) | modifiers |
	    (release ? INPUT_KEY_RELEASE : 0U);
	return 1;
}
