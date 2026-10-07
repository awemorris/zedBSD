/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system's input method, /usr/libexec/keiland-ime (ws095-p004,
 * plan/ws095/design.md section 5).
 *
 * The compositor starts it on a socket pair named by WAYLAND_SOCKET and starts it
 * again if it dies.  It binds the seat and the three globals only it is
 * shown (the input method manager, the virtual keyboard manager and
 * the compositor's status), makes its languages (direct input first, then
 * Japanese), and serves the keyboard until the connection ends or it is
 * told to stop by a signal.  The languages save what they learned once no
 * key has come for a while, and when the program ends.
 */

#include "program.h"
#include "skk.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Where the Japanese dictionary is installed (the package ime-dict-ja): one file, the supplement and the system dictionary (ws095-p017). */
#define MAIN_DICTIONARY			KEILAND_DATADIR "/keiland/ime/ja/SKK-JISYO.ja"

/* Where the SKK engine's dictionaries are installed (the package ime-dict-skk, WS154): the fuller, then the compact. */
#define MAIN_SKK_DICTIONARY		KEILAND_DATADIR "/keiland/ime/skk/SKK-JISYO.X"
#define MAIN_SKK_COMPACT		KEILAND_DATADIR "/keiland/ime/skk/SKK-JISYO.remacs"

/* The input methods the Languages page offers (ime.method, WS154): none, Japanese, SKK. */
#define MAIN_METHOD_NONE		0
#define MAIN_METHOD_JA			1
#define MAIN_METHOD_SKK			2

/* The version of the compositor's status with the on-screen keyboard's predictions (ws166-p002). */
#define MAIN_STATUS_VERSION		2U

/*
 * Set by a SIGTERM, SIGHUP or SIGINT: the loop ends at its next turn, so
 * that the languages save what they learned before the program goes.  It
 * is zero for the program's life until then.
 */
static volatile sig_atomic_t main_stopping;

/*
 * The pipe the signal handler writes a byte into, so that the loop's poll
 * wakes even when the signal went to another thread (the user
 * dictionary's writer) or the wait was not interrupted.  Made once at the
 * start; -1 at both ends when it could not be made (the loop then relies
 * on the interrupted poll alone).
 */
static int main_wake[2] = { -1, -1 };

static void main_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void main_global_remove(void *data, struct wl_registry *registry, uint32_t name);
static int main_method(int count, char **arguments);
static int main_engines(struct program *program, int method);
static void main_user_path(char *path, size_t size, const char *name);
static void main_warm(struct program *program, struct ime_engine *engine);
static int main_serve(struct program *program);
static int main_earlier(int left, int right);
static void main_stop(int signal_number);

/*
 * The registry's events.
 */
static const struct wl_registry_listener main_registry_listener = {
	main_global,
	main_global_remove
};

int
main(
	int count,
	char **arguments)
{
	struct program program;
	struct sigaction action;
	int status;
	int method;
	unsigned i;

	memset(&program, 0, sizeof(program));
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* The wake-up pipe the stop writes into; the loop polls its reading end. */
	status = pipe(main_wake);
	if (status == 0) {
		(void)fcntl(main_wake[0], F_SETFL, O_NONBLOCK);
		(void)fcntl(main_wake[1], F_SETFL, O_NONBLOCK);
		(void)fcntl(main_wake[0], F_SETFD, FD_CLOEXEC);
		(void)fcntl(main_wake[1], F_SETFD, FD_CLOEXEC);
	} else {
		main_wake[0] = -1;
		main_wake[1] = -1;
	}

	/* A stop asked by a signal ends the loop rather than the program, and wakes its wait. */
	memset(&action, 0, sizeof(action));
	action.sa_handler = main_stop;
	sigemptyset(&action.sa_mask);
	(void)sigaction(SIGTERM, &action, NULL);
	(void)sigaction(SIGHUP, &action, NULL);
	(void)sigaction(SIGINT, &action, NULL);

	/* The engines' output is large; it lives on the heap. */
	program.out = malloc(sizeof(*program.out));
	if (program.out == NULL) {
		printf("KEI-IME FAILED step=memory\n");
		return 1;
	}

	/* The connection the compositor gave (WAYLAND_SOCKET). */
	program.display = wl_display_connect(NULL);
	if (program.display == NULL) {
		printf("KEI-IME FAILED step=connect errno=%d\n", errno);
		return 1;
	}

	/* The globals: the seat and the input method's own. */
	program.registry = wl_display_get_registry(program.display);
	if (program.registry == NULL) {
		printf("KEI-IME FAILED step=registry\n");
		return 1;
	}

	status = wl_registry_add_listener(program.registry, &main_registry_listener, &program);
	if (status != 0) {
		printf("KEI-IME FAILED step=registry-listener\n");
		return 1;
	}

	status = wl_display_roundtrip(program.display);
	if (status < 0) {
		printf("KEI-IME FAILED step=roundtrip errno=%d\n", errno);
		return 1;
	}

	/* Without every global there is nothing to serve. */
	if (program.seat == NULL || program.method_manager == NULL ||
	    program.keyboard_manager == NULL || program.status_manager == NULL) {
		printf("KEI-IME FAILED step=globals\n");
		return 1;
	}

	/* The languages of the input method chosen (the compositor's --method, the Languages page's choice). */
	method = main_method(count, arguments);
	status = main_engines(&program, method);
	if (status != 0) {
		printf("KEI-IME FAILED step=engines\n");
		return 1;
	}

	/* The input method's objects. */
	status = program_method_start(&program);
	if (status != 0) {
		printf("KEI-IME FAILED step=method\n");
		return 1;
	}

	status = wl_display_roundtrip(program.display);
	if (status < 0) {
		printf("KEI-IME FAILED step=roundtrip errno=%d\n", errno);
		return 1;
	}

	/* The candidate window; without it the program still converts, only the candidates are not shown. */
	status = program_popup_start(&program);
	printf("KEI-IME POPUP ready=%d error=%d\n", program.popup.ready, status);

	/* Serves the keyboard until the connection ends, another input method holds the seat, or a signal stops it. */
	printf("KEI-IME READY languages=%u\n", program.engine_count);
	while (!program.unavailable && !main_stopping) {
		status = main_serve(&program);
		if (status != 0)
			break;
	}

	/* The engines go with the program, writing what they learned and have not saved. */
	for (i = 0; i < program.engine_count; i++)
		program.engines[i].ops->destroy(&program.engines[i]);

	printf("KEI-IME DONE\n");
	free(program.out);
	kl_appearance_close(program.popup.appearance);
	wl_display_disconnect(program.display);

	/* Succeeded: the program ends with its connection. */
	return 0;
}

/*
 * Binds the globals the program needs.
 */
static void
main_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct program *program;
	uint32_t wanted;
	int order;

	program = data;

	/* The seat. */
	order = strcmp(interface, "wl_seat");
	if (order == 0) {
		program->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
		return;
	}

	/* The input method manager. */
	order = strcmp(interface, "zwp_input_method_manager_v2");
	if (order == 0) {
		program->method_manager = wl_registry_bind(registry, name, &zwp_input_method_manager_v2_interface, 1);
		return;
	}

	/* The virtual keyboard manager. */
	order = strcmp(interface, "zwp_virtual_keyboard_manager_v1");
	if (order == 0) {
		program->keyboard_manager = wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
		return;
	}

	/* The compositor's status: version 2 for the on-screen keyboard's predictions (ws166-p002) when the compositor has it. */
	order = strcmp(interface, "kl_ime_status_manager_v1");
	if (order == 0) {
		wanted = 1U;
		if (version >= MAIN_STATUS_VERSION)
			wanted = MAIN_STATUS_VERSION;
		program->status_manager = wl_registry_bind(registry, name, &kl_ime_status_manager_v1_interface, wanted);
		return;
	}

	/* The compositor, for the candidate window's surface. */
	order = strcmp(interface, "wl_compositor");
	if (order == 0) {
		program->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
		return;
	}

	/* Shared memory, for its buffers. */
	order = strcmp(interface, "wl_shm");
	if (order == 0)
		program->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
}

/*
 * Ignores a global that goes; the compositor's globals do not.
 */
static void
main_global_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(registry);
	UNUSED_PARAMETER(name);
}

/*
 * Reads the input method chosen from the command line (--method=none, ja
 * or skk, as the compositor passes the Languages page's choice); Japanese when
 * none is given or the word is unknown.
 */
static int
main_method(
	int count,
	char **arguments)
{
	int i;
	int differs;

	/* The last --method given counts. */
	for (i = count - 1; i >= 1; i--) {
		/* None: direct input alone. */
		differs = strcmp(arguments[i], "--method=none");
		if (differs == 0)
			return MAIN_METHOD_NONE;

		/* SKK. */
		differs = strcmp(arguments[i], "--method=skk");
		if (differs == 0)
			return MAIN_METHOD_SKK;

		/* Japanese, said outright. */
		differs = strcmp(arguments[i], "--method=ja");
		if (differs == 0)
			return MAIN_METHOD_JA;
	}

	/* Japanese, as before the choice existed. */
	return MAIN_METHOD_JA;
}

/*
 * Makes the languages: direct input, then the input method chosen with its
 * dictionaries (Japanese, SKK, or nothing more for none).
 *
 * Returns 0, or -1 when an engine cannot be made.
 */
static int
main_engines(
	struct program *program,
	int method)
{
	struct ja_config config;
	struct skk_config skk;
	char user[1024];
	int error;

	/* Direct input comes first: the program starts in it. */
	error = ime_direct_create(&program->engines[0]);
	if (error != 0)
		return -1;

	/* One language so far, and the method chosen in the log. */
	program->engine_count = 1;
	printf("KEI-IME METHOD %d\n", method);

	/* None: direct input alone. */
	if (method == MAIN_METHOD_NONE)
		return 0;

	/* SKK, with REmacs's dictionaries and the user's own. */
	if (method == MAIN_METHOD_SKK) {
		main_user_path(user, sizeof(user), "skk-jisyo");
		memset(&skk, 0, sizeof(skk));
		skk.dictionaries[0] = MAIN_SKK_DICTIONARY;
		skk.dictionaries[1] = MAIN_SKK_COMPACT;
		skk.user_dictionary = user;
		error = skk_engine_create(&program->engines[1], &skk);
		if (error != 0)
			return -1;

		/* Succeeded: direct input and SKK. */
		program->engine_count = 2;
		return 0;
	}

	/* Japanese, with its one dictionary (the supplement and the system dictionary, ws095-p017) and the user's. */
	main_user_path(user, sizeof(user), "ja-user.dict");
	config.system_dictionary = MAIN_DICTIONARY;
	config.supplement_dictionary = NULL;
	config.user_dictionary = user;
	error = ja_engine_create(&program->engines[1], &config);
	if (error != 0)
		return -1;

	/* Direct input and Japanese. */
	program->engine_count = 2;

	/* A first conversion is made and dropped, so that the first key typed does not wait for the code to be read in. */
	main_warm(program, &program->engines[1]);

	/* Succeeded: the languages are ready. */
	return 0;
}

/*
 * Types a reading into an engine, converts it and drops it: the engine's
 * code and the dictionaries' pages are read in before the first real key.
 */
static void
main_warm(
	struct program *program,
	struct ime_engine *engine)
{
	static const char letters[] = "kyouhaiitenkidesu";
	struct ime_key key;
	size_t i;

	/* Each letter, then Space to convert. */
	key.modifiers = 0;
	for (i = 0; letters[i] != '\0'; i++) {
		key.code = 30U;
		key.character = (unsigned char)letters[i];
		engine->ops->key(engine, &key, program->out);
	}

	key.code = IME_KEY_SPACE;
	key.character = ' ';
	engine->ops->key(engine, &key, program->out);

	/* Nothing of it is kept. */
	engine->ops->reset(engine, false, program->out);
}

/*
 * Gives the path of a user dictionary (by its file name), making its directory
 * (~/.config/kei/ime, the user's alone).
 */
static void
main_user_path(
	char *path,
	size_t size,
	const char *name)
{
	const char *home;
	char directory[1024];

	/* Without a home the choices are not kept. */
	home = getenv("HOME");
	if (home == NULL || home[0] == '\0') {
		snprintf(path, size, "/nonexistent/%s", name);
		return;
	}

	/* Each level of ~/.config/kei/ime, made when missing. */
	snprintf(directory, sizeof(directory), "%s/.config", home);
	(void)mkdir(directory, 0700);
	snprintf(directory, sizeof(directory), "%s/.config/kei", home);
	(void)mkdir(directory, 0700);
	snprintf(directory, sizeof(directory), "%s/.config/kei/ime", home);
	(void)mkdir(directory, 0700);

	/* The file in it. */
	snprintf(path, size, "%s/%s", directory, name);
}

/*
 * Serves one turn of the connection: the events queued, then a wait for
 * more no longer than the held key's next repeat or the languages' save
 * (or until a signal asks the program to stop), then the repeat and the
 * save when they are due.
 *
 * Returns 0, or -1 when the connection ended.
 */
static int
main_serve(
	struct program *program)
{
	struct pollfd descriptors[2];
	char drained[16];
	int repeat_timeout;
	int save_timeout;
	int timeout;
	int status;

	/* The events already read. */
	status = wl_display_dispatch_pending(program->display);
	if (status < 0)
		return -1;

	/* What the program sends, sent now. */
	(void)wl_display_flush(program->display);

	/* Waits for the compositor, or until the held key repeats or the languages are due to save. */
	repeat_timeout = program_repeat_timeout(program, program_clock_ms());
	save_timeout = program_save_timeout(program, program_clock_ms());
	timeout = main_earlier(repeat_timeout, save_timeout);
	descriptors[0].fd = wl_display_get_fd(program->display);
	descriptors[0].events = POLLIN;
	descriptors[0].revents = 0;
	descriptors[1].fd = main_wake[0];
	descriptors[1].events = POLLIN;
	descriptors[1].revents = 0;
	status = poll(descriptors, 2, timeout);
	if (status < 0 && errno != EINTR)
		return -1;

	/* A stop asked by a signal: the loop ends at its test, without waiting for the compositor. */
	if (main_stopping) {
		if (main_wake[0] >= 0)
			(void)read(main_wake[0], drained, sizeof(drained));
		return 0;
	}

	/* The compositor's events, read and handled. */
	if (status > 0 && (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
		status = wl_display_dispatch(program->display);
		if (status < 0)
			return -1;
	}

	/* The held key's repeat. */
	program_repeat_due(program, program_clock_ms());

	/* The languages' save, once no key has come for a while. */
	program_save_due(program, program_clock_ms());

	/* Succeeded: the connection goes on. */
	return 0;
}

/*
 * Gives the shorter of two poll timeouts, where -1 means no limit.
 */
static int
main_earlier(
	int left,
	int right)
{
	/* A side without a limit leaves the other. */
	if (left < 0)
		return right;
	if (right < 0)
		return left;

	/* Both limited: the left is sooner. */
	if (left < right)
		return left;

	/* The right is sooner, or as soon. */
	return right;
}

/*
 * Notes a signal asking the program to stop (SIGTERM, SIGHUP, SIGINT).
 */
static void
main_stop(
	int signal_number)
{
	UNUSED_PARAMETER(signal_number);

	/* main's loop sees it after the wait, which the byte in the pipe ends. */
	main_stopping = 1;
	if (main_wake[1] >= 0)
		(void)write(main_wake[1], "", 1);
}
