/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The text input and input method protocols in zdesktop (ws095-p004,
 * plan/ws095/design.md sections 2 to 4).
 *
 * text-input.c keeps each application's zwp_text_input_v3 and follows the
 * keyboard focus; input-method.c starts the system's input method, serves
 * its zwp_input_method_v2, keyboard grab, virtual keyboard and status, and
 * decides where each key goes.
 */

#ifndef KWL_IME_H
#define KWL_IME_H

#include "kwl.h"

#include <vulkan/vulkan.h>

/* The evdev codes whose presses are remembered, so that a release goes where its press went. */
#define KWL_IME_KEYS		768U

/* The longest text one message carries (the protocols' 4000 bytes), with its NUL. */
#define KWL_IME_TEXT_MAX	4001U

/*
 * Where the press of a key went, so that its release goes there too.
 */
enum kwl_ime_route {
	KWL_IME_ROUTE_NONE,
	KWL_IME_ROUTE_GRAB,
	KWL_IME_ROUTE_TAKEN
};

/*
 * One application's zwp_text_input_v3.
 *
 * The state a request sets waits in the pending fields until the client's
 * commit; the current fields are what the input method is told.  commits
 * counts the client's commits, and text_commit is the number of the commit
 * that last set the surrounding text (0 for none since the enable), so
 * that the on-screen keyboard can tell whether the text is newer than what
 * it sent (ws166-p003).  The record lives as long as the object and is
 * freed with it.
 */
struct kwl_text_input {
	struct kwl_text_input *next;
	struct kwl_object *object;
	struct kwl_object *surface;
	unsigned pending_enable;
	unsigned pending_disable;
	char *pending_text;
	unsigned pending_text_set;
	int32_t pending_cursor;
	int32_t pending_anchor;
	uint32_t pending_cause;
	uint32_t pending_hint;
	uint32_t pending_purpose;
	int32_t pending_rectangle[4];
	unsigned enabled;
	char *text;
	int32_t cursor;
	int32_t anchor;
	uint32_t cause;
	uint32_t hint;
	uint32_t purpose;
	int32_t rectangle[4];
	uint32_t commits;
	uint32_t text_commit;
};

/* The most applications whose language is remembered (ws095-p016), and an application key's longest text. */
#define KWL_IME_APPS			32U
#define KWL_IME_APP_KEY			80U

/*
 * The language one application last had (ws095-p016): its key -- "app:" and
 * its windows' application ID, or "client:" and its connection's number
 * when its window names none -- and the language's ID.  A key empty means
 * the entry is free.
 */
struct kwl_ime_app {
	char key[KWL_IME_APP_KEY];
	char language[16];
};

/*
 * The system's input method: its process and connection, its objects, the
 * text input it is activated for, and where each held key went.
 *
 * The language is kept for each application (ws095-p016, the user's
 * request of 2026-10-04): apps holds the language each application last
 * had, and desktop_language the desktop's (when no window has the
 * keyboard, or the desktop surface has it; desktop_known once it had one).
 * focus_key names whose the language chosen now is (empty for the
 * desktop); when the keyboard moves to another application, its language
 * is chosen again, and an application seen for the first time starts with
 * the desktop's.  Windows, fields and carets within one application share
 * its language.  An application's entry goes when its last connection
 * ends.
 *
 * The language's chip in the bars: indicator_shown while it shows,
 * indicator_x where it was last drawn in the system bar (logged when it
 * moves), and indicators where it was drawn on each output's bar (the
 * system bar's and each head's, ws113-p015), which a click is looked up
 * in.
 *
 * It is made by kwl_ime_start when the program exists, and lives for the
 * compositor's lifetime; the connection and the objects come and go with
 * the process, which is started again after a crash a few times.
 */
struct kwl_ime {
	pid_t pid;
	struct kwl_client *client;
	uint64_t starts[3];
	unsigned start_index;
	uint64_t restart_ms;
	unsigned given_up;
	struct kwl_object *method;
	struct kwl_object *grab;
	struct kwl_object *keyboard;
	unsigned keyboard_keymap;
	struct kwl_object *status;
	struct kwl_object *popups[4];
	char language[16];
	char label[16];
	int32_t indicator_x;
	unsigned indicator_shown;
	struct kwl_plane_places indicators;
	unsigned composing;
	struct kwl_text_input *active;
	unsigned activated;
	uint32_t done_count;
	char *pending_commit;
	char *pending_preedit;
	int32_t pending_begin;
	int32_t pending_end;
	uint32_t pending_before;
	uint32_t pending_after;
	char *preedit_shown;
	unsigned char route[KWL_IME_KEYS];
	unsigned char keyboard_down[KWL_IME_KEYS];
	unsigned char keyboard_taken[KWL_IME_KEYS];
	unsigned watching;
	uint64_t watch_ms;
	unsigned bypass;
	struct kwl_ime_app apps[KWL_IME_APPS];
	char desktop_language[16];
	unsigned desktop_known;
	char focus_key[KWL_IME_APP_KEY];
	/*
	 * Nonzero from a change of the input method chosen (WS154) until the
	 * program is started again with it: that start does not wait, nor
	 * count among the three a minute.
	 */
	unsigned replacing;
	int32_t method_started;
};

/* text-input.c */
int kwl_text_input_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_text_input_object_gone(struct kwl_object *object);
void kwl_text_input_focus(struct kwl_server *server, struct kwl_object *previous);
struct kwl_text_input *kwl_text_input_current(struct kwl_server *server);
void kwl_text_input_deliver(struct kwl_text_input *input, const char *preedit, int32_t begin, int32_t end, const char *commit, uint32_t before, uint32_t after);

/* input-method.c */
void kwl_ime_start(struct kwl_server *server);
void kwl_ime_tick(struct kwl_server *server, uint64_t now);
int kwl_ime_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_ime_object_gone(struct kwl_object *object);
void kwl_ime_client_gone(struct kwl_client *client);
int kwl_ime_global_visible(struct kwl_client *client, enum kwl_kind kind);
int kwl_ime_predict(struct kwl_server *server, uint32_t serial, const char *reading);
void kwl_ime_learn(struct kwl_server *server, const char *reading, const char *word);
void kwl_ime_repeat_changed(struct kwl_server *server);
void kwl_ime_method_changed(struct kwl_server *server);
int kwl_ime_key_early(struct kwl_server *server, uint32_t time, uint32_t key, uint32_t state);
int kwl_ime_key_grab(struct kwl_server *server, uint32_t time, uint32_t key, uint32_t state, int composing_only);
void kwl_ime_modifiers(struct kwl_server *server);
void kwl_ime_focus(struct kwl_server *server, struct kwl_object *previous);
void kwl_ime_update(struct kwl_server *server, struct kwl_text_input *committed);
void kwl_ime_field_changed(struct kwl_server *server);
int kwl_ime_field_key(struct kwl_server *server, uint32_t time, uint32_t key, uint32_t state);
int kwl_ime_home_key(struct kwl_server *server, uint32_t time, uint32_t key, uint32_t state);
void kwl_ime_text_input_gone(struct kwl_server *server, struct kwl_text_input *input);
void kwl_ime_surface_commit(struct kwl_object *surface);
void kwl_ime_popup_draw(struct kwl_server *server, VkCommandBuffer command);
int32_t kwl_ime_indicator_width(struct kwl_server *server);
void kwl_ime_indicator_draw(struct kwl_server *server, VkCommandBuffer command, int32_t x, int32_t top, const float *ink);
int kwl_ime_indicator_button(struct kwl_server *server, uint32_t button, uint32_t state);

#endif
