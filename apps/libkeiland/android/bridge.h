/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The queue that carries what Java (threads U and J) tells the native thread
 * (M), and the conversions between the UTF-16 of Java and the UTF-8 of C.
 *
 * Java's threads push an event and return at once; M takes everything that was
 * pushed, in the order it was pushed, when its looper is woken.  Nothing here
 * calls JNI, ALooper or any Android function, so the host can test it.
 */

#ifndef KEILAND_ANDROID_BRIDGE_H
#define KEILAND_ANDROID_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

/* The kinds of event, with what the fields of struct keiland_bridge_event hold for each. */

/* text: the committed text */
#define KEILAND_BRIDGE_COMMIT		1U

/* text: the composing text; a: the cursor in code points (-1 hides it) */
#define KEILAND_BRIDGE_COMPOSE		2U

/* a: characters to delete before the cursor; b: after it (code points) */
#define KEILAND_BRIDGE_DELETE		3U

/* a, b, c, d, e: left, top, right and bottom insets and the keyboard's height, in pixels */
#define KEILAND_BRIDGE_INSETS		4U

/* a: dark; b: accent as 0xRRGGBB (-1 for none); f: font scale; text: the locale */
#define KEILAND_BRIDGE_CONFIG		5U

/* a: whether the window has the focus */
#define KEILAND_BRIDGE_FOCUS		6U

/* text: the clipboard's text (NULL when it is empty) */
#define KEILAND_BRIDGE_CLIPBOARD	7U

/* a: the menu's request number; b: the chosen item (-1 when it was dismissed) */
#define KEILAND_BRIDGE_MENU		8U

/* code and data: what the program posted to itself */
#define KEILAND_BRIDGE_APP_POST		9U

/* a: the Android key code; b: the meta state; c: whether it is pressed */
#define KEILAND_BRIDGE_KEY		10U

/*
 * One thing Java told M.
 *
 * An event is made by keiland_bridge_push, which copies it, and is owned by
 * the list that keiland_bridge_take_all hands out; keiland_bridge_free ends it.
 */
struct keiland_bridge_event {
	struct keiland_bridge_event *next;
	unsigned kind;
	int32_t a;
	int32_t b;
	int32_t c;
	int32_t d;
	int32_t e;
	float f;
	uint32_t code;
	void *data;

	/* UTF-8 ended by NUL, in memory from malloc; NULL for none. */
	char *text;
};

/*
 * Sets the function that wakes M after an event is pushed.
 *
 * Called once, before the first push.  NULL means no wake-up (a test).
 */
void keiland_bridge_set_wake(void (*wake)(void));

/*
 * Appends a copy of the event, and of length bytes of text (NULL for none), to
 * the queue and wakes M.  Called from any thread.
 *
 * Returns 0, EINVAL (event NULL, or text NULL with a length above 0) or
 * ENOMEM (the event is dropped and counted).
 */
int keiland_bridge_push(const struct keiland_bridge_event *event, const char *text, size_t length);

/*
 * Takes every queued event, oldest first, as a list ended by NULL, or NULL when
 * there is none.  Called from M.
 */
struct keiland_bridge_event *keiland_bridge_take_all(void);

/*
 * Frees a list from keiland_bridge_take_all with the text of each event; the
 * data pointer belongs to whoever posted it and is not freed.
 */
void keiland_bridge_free(struct keiland_bridge_event *list);

/*
 * Returns how many events were dropped for lack of memory.
 */
unsigned keiland_bridge_dropped(void);

/*
 * Converts UTF-16 to UTF-8.  A surrogate that has no partner becomes U+FFFD.
 *
 * Writes at most size bytes with a NUL after them, and never half a character:
 * the text stops before a character that does not fit.  Returns the number of
 * bytes written without the NUL.  With out NULL it writes nothing and returns
 * the number the whole text needs.
 */
size_t keiland_utf16_to_utf8(const uint16_t *in, size_t count, char *out, size_t size);

/*
 * Converts UTF-8 to UTF-16.  A byte sequence that is not valid UTF-8 becomes
 * U+FFFD.
 *
 * Writes at most count units, never half of a surrogate pair, and no NUL.
 * Returns the number of units written.  With out NULL it writes nothing and
 * returns the number the whole text needs.
 */
size_t keiland_utf8_to_utf16(const char *in, size_t length, uint16_t *out, size_t count);

#endif /* KEILAND_ANDROID_BRIDGE_H */
