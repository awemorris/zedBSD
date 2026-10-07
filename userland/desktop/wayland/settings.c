/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop's settings in the compositor (WS135, plan/ws135/design.md
 * section 4): the session's store (settings-store.c), the settings put
 * into effect (the wallpaper, the windows' opacity, the pointer, the
 * keyboards' repeat, the sound through volume.c), and Keiland's system
 * extension that clients reach them through (kl_system_manager_v1 and
 * kl_system_settings_v1, libkeiland/system/kl-system-protocol.h).
 *
 * The store reads desktop.conf once, before the look draws the wallpaper,
 * and writes it once, at the session's end: a thread merges it at the Log
 * Out, and the compositor's end merges what changed since.  Nothing here
 * waits on the disk during the session: a wallpaper chosen is read and
 * decoded by glass.c's thread, and its result answers the client later.
 *
 * Each change, whoever made it, comes to every settings object as its
 * value and a done.  Each key put into effect is logged
 * ("KWL PREFERENCES key=... applied"), which the tests read.  No other
 * process reads or writes desktop.conf, and nothing looks at it during
 * the session.
 */

#include "language.h"
#include "kwl.h"
#include "settings-store.h"
#include "ime.h"
#include "../artwork/accent.h"

#include "userland/desktop/libkeiland/system/kl-system-protocol.h"
#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The longest string a request may carry; a longer key or value is refused as invalid. */
#define SETTINGS_WIRE_TEXT_MAX	4096U

/*
 * A client's request waiting for the wallpaper glass.c reads: the client
 * (by its number: the client may go meanwhile) and its settings object, the
 * request's number, and whether the wallpaper goes back to its default.
 * active is zero when nothing waits.
 */
struct settings_waiting {
	uint64_t client;
	uint32_t object;
	uint32_t request;
	unsigned reset;
	unsigned active;
	char path[KL_SETTINGS_VALUE_MAX];
};

/*
 * The compositor's side of the settings that is not the store: which
 * entries are to be told to the settings objects at the next flush, the
 * serial of the last done, and the request waiting for a wallpaper.  It
 * lives for the process (one desktop runs in it); only the event loop's
 * thread touches it.
 */
struct settings_state {
	unsigned announce[KWL_SETTINGS_ENTRIES];
	uint32_t serial;
	struct settings_waiting waiting;
};

/*
 * The one state of the process: zeroed by kwl_settings_open and kept for
 * the process's life; only the event loop's thread touches it.
 */
static struct settings_state settings_state;

static void settings_migrate(struct kwl_settings_store *store, const char *old_name, const char *new_name);
static void settings_apply(struct kwl_server *server, const char *name, int starting);
static void settings_apply_all(struct kwl_server *server, int starting);
static void settings_apply_wallpaper(struct kwl_server *server, int starting);
static void settings_apply_opacity(struct kwl_server *server);
static void settings_apply_number(struct kwl_server *server, const char *name, int32_t *target);
static void settings_apply_repeat(struct kwl_server *server, int starting);
static void settings_apply_appearance(struct kwl_server *server, int starting);
static void settings_apply_accent(struct kwl_server *server, int starting);
static void settings_mark(struct kwl_server *server, const char *name);
static void settings_flush(struct kwl_server *server);
static int settings_emit_value(struct kwl_client *client, uint32_t id, const struct kwl_settings_entry *entry);
static void settings_emit_done(struct kwl_client *client, uint32_t id);
static void settings_result(struct kwl_client *client, uint32_t id, uint32_t request, uint32_t applied, int stored);
static int settings_set(struct kwl_object *object, const unsigned char *bytes, size_t size, int reset);
static uint32_t settings_change(struct kwl_object *object, uint32_t request, const char *name, const char *value, int reset, int *answered);
static uint32_t settings_change_sound(struct kwl_server *server, const struct kl_settings_key *key, const char *value, int reset);
static uint32_t settings_change_wallpaper(struct kwl_object *object, uint32_t request, const char *value, int reset, int *answered);
static void settings_wallpaper_done(struct kwl_server *server, int error);
static void settings_sound(struct kwl_server *server);
static int settings_snapshot(struct kwl_object *settings);
static uint32_t settings_result_of(int error);
static size_t settings_put_string(unsigned char *payload, size_t offset, const char *text);
static int settings_read_string(const unsigned char *bytes, size_t size, size_t offset, char **text, size_t *next);
static uint32_t settings_word(const unsigned char *bytes, size_t offset);

/*
 * Opens the session's settings, before the look draws the wallpaper: the
 * command line's wallpaper and opacity are the defaults, the file's values
 * come over them, and every setting is put into effect.  The login screen
 * has none.
 */
void
kwl_settings_open(
	struct kwl_server *server)
{
	struct kwl_settings_store *store;
	char home[KWL_SETTINGS_PATH_MAX];
	char opacity[16];
	int percent;
	int error;

	/* Keeps what the command line gave, which a setting back at its default returns to. */
	server->window_opacity_started = server->window_opacity;
	server->wallpaper_started = server->wallpaper_path;
	server->wallpaper_chosen[0] = '\0';

	/* Starts with nothing marked and no wallpaper request waiting. */
	memset(&settings_state, 0, sizeof(settings_state));

	/* Allocates the store; without it the session has no settings. */
	store = calloc(1, sizeof(*store));
	if (store == NULL) {
		printf("KWL SETTINGS none errno=%d\n", ENOMEM);
		return;
	}

	/* Finds the user's home; without one the settings live for the session only. */
	error = kwl_settings_home(home, sizeof(home));
	if (error != 0)
		home[0] = '\0';

	/* Opens the store on the home's desktop.conf. */
	error = kwl_settings_store_open(store, home);
	if (error != 0)
		printf("KWL SETTINGS home errno=%d\n", error);

	/* Makes the command line's wallpaper the wallpaper's default. */
	if (server->wallpaper_path != NULL)
		kwl_settings_store_default(store, "wallpaper", server->wallpaper_path);

	/* Makes the command line's opacity, in whole percent, the opacity's default. */
	percent = (int)(server->window_opacity * 100.0f + 0.5f);
	(void)snprintf(opacity, sizeof(opacity), "%d", percent);
	kwl_settings_store_default(store, "window.opacity", opacity);

	/* Reads desktop.conf, once for the session. */
	error = kwl_settings_store_load(store);
	if (error != 0)
		printf("KWL SETTINGS read-failed errno=%d\n", error);

	/* The one pointer setting of before becomes the mouse's (ws089-p024). */
	settings_migrate(store, "pointer.speed", "mouse.speed");
	settings_migrate(store, "pointer.natural", "mouse.natural");

	/* Gives the server the store, and puts every setting into effect before anything is drawn. */
	server->settings = store;
	settings_apply_all(server, 1);

	/* Logs the opening for the tests. */
	printf("KWL SETTINGS open present=%d\n", store->present);
}

/*
 * Looks after the settings once a pass: a wallpaper read meanwhile, the
 * sound audiod reported, and the changes to tell the settings objects.
 */
void
kwl_settings_tick(
	struct kwl_server *server)
{
	int finished;
	int error;

	/* No settings (the login screen). */
	if (server->settings == NULL)
		return;

	/* Takes a wallpaper glass.c finished reading, and answers its request. */
	finished = kwl_glass_wallpaper_poll(server, &error);
	if (finished)
		settings_wallpaper_done(server, error);

	/* Takes the sound as audiod reports it. */
	settings_sound(server);

	/* Tells every settings object what changed. */
	settings_flush(server);
}

/*
 * Starts writing the session's settings at the Log Out, without the event
 * loop waiting (the session goes on showing until its manager ends it).
 */
void
kwl_settings_logout(
	struct kwl_server *server)
{
	int error;

	/* No settings (the login screen). */
	if (server->settings == NULL)
		return;

	/* Starts merging the session's settings into desktop.conf on the store's thread. */
	error = kwl_settings_store_save_later(server->settings);

	/* Logs how the start went, for the tests. */
	printf("KWL SETTINGS logout-save error=%d\n", error);
}

/*
 * Writes what is left of the session's settings and lets them go, at the
 * compositor's end.
 */
void
kwl_settings_close(
	struct kwl_server *server)
{
	int error;

	/* No settings (the login screen). */
	if (server->settings == NULL)
		return;

	/* Waits for the writer, then writes whatever changed after it (or everything, without it). */
	error = kwl_settings_store_finish(server->settings);

	/* Logs how the writing went, for the tests. */
	printf("KWL SETTINGS saved error=%d\n", error);

	/* Lets the store go; the server has no settings from now on. */
	kwl_settings_store_close(server->settings);
	free(server->settings);
	server->settings = NULL;
}

/*
 * Gives the value a setting had in the file at the session's start, as a
 * number (the volume volume.c gives audiod).  Returns 0, or ENOENT when
 * the file did not hold it.
 */
int
kwl_settings_kept(
	struct kwl_server *server,
	const char *name,
	int *number)
{
	struct kwl_settings_entry *entry;
	int error;

	/* Without settings (the login screen) nothing was kept. */
	if (server->settings == NULL)
		return ENOENT;

	/* Finds the setting; one the file did not hold was not kept. */
	entry = kwl_settings_store_find(server->settings, name);
	if (entry == NULL || !entry->start_chosen)
		return ENOENT;

	/* Reads the file's value as a number. */
	error = kl_settings_key_number(entry->key, entry->start, number);
	if (error != 0)
		return ENOENT;

	/* Succeeded: the kept number. */
	return 0;
}

/*
 * Gives a setting's value now, as a number (ws164-p002: welcome.done).
 * Returns 0, or ENOENT without settings (the login screen) or for a
 * setting the store does not hold or whose value is not a number.
 */
int
kwl_settings_number(
	struct kwl_server *server,
	const char *name,
	int *number)
{
	struct kwl_settings_entry *entry;
	int error;

	/* Without settings nothing is known. */
	if (server->settings == NULL)
		return ENOENT;

	/* The setting, its value as a number. */
	entry = kwl_settings_store_find(server->settings, name);
	if (entry == NULL)
		return ENOENT;
	error = kl_settings_key_number(entry->key, entry->value, number);
	if (error != 0)
		return ENOENT;

	/* Succeeded: the number. */
	return 0;
}

/*
 * Tells whether a client sees a global of the system extension: only a
 * session (not the login screen) shows it, and only to a client of the
 * compositor's own user (WS131 D5).  Every other global is everyone's.
 */
int
kwl_settings_global_visible(
	struct kwl_client *client,
	enum kwl_kind kind)
{
	uid_t uid;
	uid_t own;
	int error;

	/* Only the system manager is limited. */
	if (kind != KWL_SYSTEM_MANAGER)
		return 1;

	/* The login screen has no settings. */
	if (client->server->settings == NULL)
		return 0;

	/* Looks at the peer's user once, at the first registry. */
	if (!client->peer_checked) {
		/*
		 * peer_checked keeps the look from being repeated; peer_same stays
		 * zero (another user) unless the users are found to match.
		 */
		client->peer_checked = 1;
		client->peer_same = 0;

		/* Asks which user runs the peer; a failure leaves it another user. */
		error = kl_backend_peer_uid(client->fd, &uid);
		if (error != 0) {
			printf("KWL SETTINGS peer client=%llu errno=%d\n", (unsigned long long)client->number, error);
		} else {
			/* The compositor's own user is the one that sees the extension. */
			own = getuid();
			if (uid == own)
				client->peer_same = 1;
		}
	}

	/* Only the compositor's user. */
	if (!client->peer_same)
		return 0;

	/* Succeeded: the client sees the extension. */
	return 1;
}

/*
 * Carries out a request of a settings object, or the manager's
 * get_settings (system.c hands it on, WS131 p010).
 */
int
kwl_settings_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *created;
	uint32_t id;
	int error;

	/* The manager's get_settings makes a settings object. */
	if (object->kind == KWL_SYSTEM_MANAGER) {
		/* get_settings, with its new ID. */
		if (opcode != KL_SYSTEM_MANAGER_GET_SETTINGS || size != 4U)
			return EPROTO;

		/* Makes the settings object under the ID the client chose. */
		id = settings_word(bytes, 0U);
		created = kwl_create(object->client, id, KWL_SYSTEM_SETTINGS, object->version);
		if (created == NULL)
			return EPROTO;

		/* Tells the new object every setting and a done. */
		error = settings_snapshot(created);
		if (error != 0)
			return error;

		/* Succeeded: the client has a settings object that knows every setting. */
		return 0;
	}

	/* A settings object: it goes, sets, or resets. */
	switch (opcode) {
	case KL_SYSTEM_SETTINGS_DESTROY:
		/* A destroy carries no arguments. */
		if (size != 0U)
			return EPROTO;

		/* Lets the settings object go. */
		kwl_object_destroy(object);

		/* Succeeded: the settings object is gone. */
		return 0;
	case KL_SYSTEM_SETTINGS_SET:
		/* Sets a setting to the value the client gave. */
		error = settings_set(object, bytes, size, 0);
		break;
	case KL_SYSTEM_SETTINGS_RESET:
		/* Puts a setting back at its default. */
		error = settings_set(object, bytes, size, 1);
		break;
	default:
		/* No other request exists. */
		error = EPROTO;
		break;
	}

	/* Reports a malformed request. */
	if (error != 0)
		return error;

	/* Succeeded: the request is answered (now, or once the wallpaper is read). */
	return 0;
}

/* Finds the user's home: $HOME, else the password file's; returns 0 or ENOENT. */
int
kwl_settings_home(
	char *home,
	size_t size)
{
	const struct passwd *user;
	const char *given;
	uid_t uid;
	int written;

	/* $HOME when it is an absolute path, else the password file's. */
	given = getenv("HOME");
	if (given == NULL || given[0] != '/') {
		/* Looks the user up in the password file; a missing or relative home is none. */
		uid = getuid();
		user = getpwuid(uid);
		if (user == NULL ||
		    user->pw_dir == NULL ||
		    user->pw_dir[0] != '/')
			return ENOENT;

		/* Takes the password file's home. */
		given = user->pw_dir;
	}

	/* The home must fit. */
	written = snprintf(home, size, "%s", given);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the home is known. */
	return 0;
}

/*
 * Moves a setting kept under its old name to its new one: a value the file
 * held for the old name, when it held none for the new one, is chosen for
 * the new one (and written under it at the session's end).
 */
static void
settings_migrate(
	struct kwl_settings_store *store,
	const char *old_name,
	const char *new_name)
{
	struct kwl_settings_entry *old_entry;
	struct kwl_settings_entry *new_entry;
	int error;

	/* Both settings, the old one read from the file and the new one not. */
	old_entry = kwl_settings_store_find(store, old_name);
	new_entry = kwl_settings_store_find(store, new_name);
	if (old_entry == NULL || new_entry == NULL)
		return;
	if (!old_entry->start_chosen || new_entry->start_chosen)
		return;

	/* The old value, chosen under the new name. */
	error = kwl_settings_store_choose(store, new_name, old_entry->start);
	printf("KWL SETTINGS migrated %s=%s to %s error=%d\n", old_name, old_entry->start, new_name, error);
}

/* Puts every setting into effect (starting: before the look is made). */
static void
settings_apply_all(
	struct kwl_server *server,
	int starting)
{
	unsigned index;

	/* Puts each compositor setting the store holds into effect. */
	for (index = 0; index < server->settings->count; index++)
		settings_apply(server, server->settings->entries[index].key->name, starting);
}

/* Puts one setting into effect; the sound is audiod's (volume.c) and needs nothing here. */
static void
settings_apply(
	struct kwl_server *server,
	const char *name,
	int starting)
{
	int32_t language;
	int differs;

	/* A mail reader's permission: each reader whose permission changed is told (mail-shell.c, ws177-p005). */
	differs = strncmp(name, KL_SYSTEM_MAIL_SETTING_PREFIX, sizeof(KL_SYSTEM_MAIL_SETTING_PREFIX) - 1U);
	if (differs == 0) {
		if (!starting)
			kwl_mail_settings_changed(server);
		return;
	}

	/* Shows the wallpaper. */
	differs = strcmp(name, "wallpaper");
	if (differs == 0) {
		settings_apply_wallpaper(server, starting);
		return;
	}

	/* Sets the windows' opacity. */
	differs = strcmp(name, "window.opacity");
	if (differs == 0) {
		settings_apply_opacity(server);
		return;
	}

	/* Makes the windows' glass panels frosted or solid (BUG-214). */
	differs = strcmp(name, "window.frosted");
	if (differs == 0) {
		settings_apply_opacity(server);
		return;
	}

	/* Sets a mouse's speed, acceleration and wheel's direction (ws089-p024). */
	differs = strcmp(name, "mouse.speed");
	if (differs == 0) {
		settings_apply_number(server, name, &server->mouse_speed);
		return;
	}

	/* A mouse's acceleration. */
	differs = strcmp(name, "mouse.acceleration");
	if (differs == 0) {
		settings_apply_number(server, name, &server->mouse_acceleration);
		return;
	}

	/* A mouse's wheel's direction. */
	differs = strcmp(name, "mouse.natural");
	if (differs == 0) {
		settings_apply_number(server, name, &server->mouse_natural);
		return;
	}

	/* Sets the touch pads' speed, and their acceleration and scrolling's direction, which their layers take now. */
	differs = strcmp(name, "touchpad.speed");
	if (differs == 0) {
		settings_apply_number(server, name, &server->touchpad_speed);
		return;
	}

	/* The touch pads' acceleration. */
	differs = strcmp(name, "touchpad.acceleration");
	if (differs == 0) {
		settings_apply_number(server, name, &server->touchpad_acceleration);
		kwl_input_touchpads_changed(server);
		return;
	}

	/* The touch pads' scrolling's direction. */
	differs = strcmp(name, "touchpad.natural");
	if (differs == 0) {
		settings_apply_number(server, name, &server->touchpad_natural);
		kwl_input_touchpads_changed(server);
		return;
	}

	/* Sets the keyboards' repeat rate, and tells the keyboards bound already. */
	differs = strcmp(name, "keyboard.repeat.rate");
	if (differs == 0) {
		settings_apply_number(server, name, &server->repeat_rate);
		settings_apply_repeat(server, starting);
		return;
	}

	/* Sets the keyboards' repeat delay, and tells the keyboards bound already. */
	differs = strcmp(name, "keyboard.repeat.delay");
	if (differs == 0) {
		settings_apply_number(server, name, &server->repeat_delay_ms);
		settings_apply_repeat(server, starting);
		return;
	}

	/* Sets the desktop's appearance, light or dark, and tells the clients (ws089-p017). */
	differs = strcmp(name, "appearance.dark");
	if (differs == 0) {
		settings_apply_appearance(server, starting);
		return;
	}

	/* Sets the accent the user chose, and tells the clients (ws179-p001). */
	differs = strcmp(name, "appearance.accent");
	if (differs == 0) {
		settings_apply_accent(server, starting);
		return;
	}

	/* Chooses the language of the desktop's text, the compositor's at once (WS158). */
	differs = strcmp(name, "ui.language");
	if (differs == 0) {
		language = 0;
		settings_apply_number(server, name, &language);
		kwl_language_set(server, (int)language);
		return;
	}

	/* The minutes without input before a sleep on the power adapter (ws052-p012, sleep.c reads them each tick). */
	differs = strcmp(name, "power.sleep.ac");
	if (differs == 0) {
		settings_apply_number(server, name, &server->sleep_ac_minutes);
		return;
	}

	/* And on battery. */
	differs = strcmp(name, "power.sleep.battery");
	if (differs == 0) {
		settings_apply_number(server, name, &server->sleep_battery_minutes);
		return;
	}

	/* Chooses the input method, which is started again with it (WS154). */
	differs = strcmp(name, "ime.method");
	if (differs == 0) {
		settings_apply_number(server, name, &server->ime_method);

		/* At the start the input method has not been started yet; it starts with the choice. */
		if (!starting)
			kwl_ime_method_changed(server);

		/* Applied. */
		return;
	}
}

/*
 * Sets the desktop's appearance the settings hold (0 light, 1 dark): the
 * glass is drawn again in it and the clients that bound kl_theme_v1
 * are told (theme.c).  Nothing is bound at the start.
 */
static void
settings_apply_appearance(
	struct kwl_server *server,
	int starting)
{
	int32_t dark;

	/* The setting's value; anything but 1 is light. */
	dark = server->dark;
	settings_apply_number(server, "appearance.dark", &dark);
	if (dark != 1)
		dark = 0;

	/* The same appearance changes nothing. */
	if (dark == server->dark)
		return;

	/* Draws everything again in the new appearance. */
	server->dark = dark;
	server->dirty = 1;

	/* Tells the clients, once anything can be bound. */
	if (!starting)
		kwl_theme_changed(server);
}

/*
 * Sets the accent the settings hold (0 blue to 7 graphite; anything else
 * is blue): the compositor's controls are drawn again in it and the
 * clients that bound kl_theme_v1 at version 2 are told (theme.c).
 * Nothing is bound at the start.
 */
static void
settings_apply_accent(
	struct kwl_server *server,
	int starting)
{
	int32_t accent;

	/* The setting's value; one out of the table is blue. */
	accent = server->accent;
	settings_apply_number(server, "appearance.accent", &accent);
	if (accent < 0 || accent >= (int32_t)KA_ACCENTS)
		accent = 0;

	/* The same accent changes nothing. */
	if (accent == server->accent)
		return;

	/* Draws everything again in the new accent. */
	server->accent = accent;
	server->dirty = 1;

	/* Tells the clients, once anything can be bound. */
	if (!starting)
		kwl_theme_changed(server);
}

/* Tells the keyboards bound already the repeat again (nothing is bound before the look is made). */
static void
settings_apply_repeat(
	struct kwl_server *server,
	int starting)
{
	/* Before anything is bound there is nobody to tell. */
	if (starting)
		return;

	/* The applications' keyboards, then the input method's grab. */
	kwl_seat_repeat_changed(server);
	kwl_ime_repeat_changed(server);
}

/*
 * Shows the wallpaper the settings hold: at the start (the look reads it
 * when it is made), or the landscape after a reset (no file to read).  A
 * picture a client chose is read by glass.c's thread instead
 * (settings_change_wallpaper).
 */
static void
settings_apply_wallpaper(
	struct kwl_server *server,
	int starting)
{
	struct kwl_settings_entry *entry;
	const char *path;
	const char *chosen;
	int differs;
	int error;

	/* The choice: the setting's path when chosen, else none (the command line's). */
	entry = kwl_settings_store_find(server->settings, "wallpaper");
	chosen = "";
	if (entry != NULL && entry->chosen)
		chosen = entry->value;

	/* The same choice as shown changes nothing. */
	differs = strcmp(chosen, server->wallpaper_chosen);
	if (differs == 0)
		return;

	/* Records the choice shown. */
	(void)snprintf(server->wallpaper_chosen, sizeof(server->wallpaper_chosen), "%s", chosen);

	/* Shows the chosen picture, or the command line's when none is chosen. */
	path = server->wallpaper_started;
	if (server->wallpaper_chosen[0] != '\0')
		path = server->wallpaper_chosen;
	server->wallpaper_path = path;

	/* Before the look is made, it draws the picture itself. */
	if (starting) {
		printf("KWL PREFERENCES key=wallpaper applied\n");
		return;
	}

	/*
	 * Afterwards the landscape is drawn now; a picture is read on glass.c's
	 * thread (ws138-p001 U7) and shown when kwl_glass_wallpaper_poll takes it.
	 */
	if (path == NULL) {
		error = kwl_glass_landscape(server);
	} else {
		error = kwl_glass_wallpaper_begin(server, path);
	}

	/* A refusal leaves the wallpaper shown. */
	if (error != 0) {
		printf("KWL PREFERENCES key=wallpaper failed errno=%d\n", error);
		return;
	}

	/* Logs that the new picture is shown, for the tests. */
	printf("KWL PREFERENCES key=wallpaper applied\n");
}

/*
 * Sets the windows' opacity the settings hold, or the command line's
 * exactly while at the default, and whether the windows' glass panels are
 * frosted (window.frosted, the default) or solid (BUG-214, the user's
 * decision of 2026-10-06: the slider is the windows' contents' opacity
 * alone and a switch makes the panels solid; it replaced BUG-171's
 * decision B, where 100 chosen made them solid).  The title bars stay
 * glass either way.
 */
static void
settings_apply_opacity(
	struct kwl_server *server)
{
	struct kwl_settings_entry *entry;
	unsigned panels_opaque;
	const char *panels;
	float opacity;
	int percent;
	int frosted;
	int error;

	/* The command line's opacity while the setting is at its default. */
	entry = kwl_settings_store_find(server->settings, "window.opacity");
	opacity = server->window_opacity_started;
	percent = (int)(opacity * 100.0f + 0.5f);
	if (entry != NULL && entry->chosen) {
		/* The chosen percentage, when it reads as a number. */
		error = kl_settings_key_number(entry->key, entry->value, &percent);
		if (error == 0)
			opacity = (float)percent / 100.0f;
	}

	/* The panels frosted unless window.frosted is chosen off. */
	panels_opaque = 0U;
	entry = kwl_settings_store_find(server->settings, "window.frosted");
	if (entry != NULL && entry->chosen) {
		error = kl_settings_key_number(entry->key, entry->value, &frosted);
		if (error == 0 && frosted == 0)
			panels_opaque = 1U;
	}

	/* The same opacity and panels change nothing. */
	if (opacity == server->window_opacity && panels_opaque == server->panels_opaque)
		return;

	/* Draws every window again at the new opacity. */
	server->window_opacity = opacity;
	server->panels_opaque = panels_opaque;
	server->dirty = 1;

	/* Logs the opacity put into effect, for the tests. */
	panels = "glass";
	if (panels_opaque != 0U)
		panels = "opaque";
	printf("KWL PREFERENCES key=window.opacity applied value=%d panels=%s\n", percent, panels);
}

/* Sets a number the desktop uses to the setting's value when it differs, and logs it. */
static void
settings_apply_number(
	struct kwl_server *server,
	const char *name,
	int32_t *target)
{
	struct kwl_settings_entry *entry;
	int number;
	int error;

	/* Finds the setting; one the store does not hold changes nothing. */
	entry = kwl_settings_store_find(server->settings, name);
	if (entry == NULL)
		return;

	/* Reads its value as a number; one that does not read changes nothing. */
	error = kl_settings_key_number(entry->key, entry->value, &number);
	if (error != 0)
		return;

	/* The same value changes nothing. */
	if (*target == (int32_t)number)
		return;

	/* Uses the new value from the next input on. */
	*target = (int32_t)number;

	/* Logs the value put into effect, for the tests. */
	printf("KWL PREFERENCES key=%s applied value=%d\n", name, number);
}

/* Marks a setting to be told to every settings object at the next flush. */
static void
settings_mark(
	struct kwl_server *server,
	const char *name)
{
	unsigned index;
	int differs;

	/* Finds the setting's entry and marks it. */
	for (index = 0; index < server->settings->count; index++) {
		/* Only the entry whose key has the name is marked. */
		differs = strcmp(server->settings->entries[index].key->name, name);
		if (differs == 0) {
			/* announce makes the next flush tell every settings object the entry's value. */
			settings_state.announce[index] = 1;
			return;
		}
	}
}

/* Tells every settings object the marked settings' values and a done. */
static void
settings_flush(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *object;
	unsigned marked;
	unsigned index;

	/* Finds whether any setting is marked. */
	marked = 0;
	for (index = 0; index < server->settings->count; index++)
		marked |= settings_state.announce[index];

	/* Nothing marked, nothing told. */
	if (!marked)
		return;

	/*
	 * The serial moves once for each state told, so that the done closing
	 * this state is told apart from every earlier one.
	 */
	settings_state.serial++;

	/* Tells each live settings object of every client each marked value, then one done. */
	for (client = server->clients;
	     client != NULL;
	     client = client->next) {
		/* A client already failed is not written to again. */
		if (client->fatal)
			continue;

		/* Visits each of the client's objects. */
		for (object = client->objects;
		     object != NULL;
		     object = object->next) {
			/* Only a live settings object is told. */
			if (object->kind != KWL_SYSTEM_SETTINGS || object->dead)
				continue;

			/* Sends each marked value. */
			for (index = 0; index < server->settings->count; index++) {
				if (settings_state.announce[index])
					(void)settings_emit_value(client, object->id, &server->settings->entries[index]);
			}

			/* Sends the done that makes the values one state. */
			settings_emit_done(client, object->id);
		}
	}

	/* Clears the marks: everything marked was told. */
	memset(settings_state.announce, 0, sizeof(settings_state.announce));
}

/* Sends a setting's value event: its key, its value (empty while not known) and its flags. */
static int
settings_emit_value(
	struct kwl_client *client,
	uint32_t id,
	const struct kwl_settings_entry *entry)
{
	unsigned char payload[16U + KL_SETTINGS_KEY_MAX + KL_SETTINGS_VALUE_MAX];
	const char *value;
	uint32_t flags;
	size_t offset;
	int error;

	/* Sends the value in effect, flagged when it is at its default. */
	flags = 0;
	value = entry->value;
	if (!entry->chosen)
		flags |= KL_SYSTEM_SETTINGS_DEFAULT;

	/* A value nothing reported yet is sent empty and flagged unknown. */
	if (!entry->known) {
		flags |= KL_SYSTEM_SETTINGS_UNKNOWN;
		value = "";
	}

	/* Builds the event's arguments in the protocol's order. */
	offset = settings_put_string(payload, 0, entry->key->name);
	offset = settings_put_string(payload, offset, value);
	memcpy(payload + offset, &flags, sizeof(flags));
	offset += sizeof(flags);

	/* Queues the event to the client. */
	error = kwl_emit(client, id, KL_SYSTEM_SETTINGS_EVENT_VALUE, payload, offset);
	if (error != 0)
		return error;

	/* Succeeded: the value is queued. */
	return 0;
}

/* Sends a done with the serial of the state it closes. */
static void
settings_emit_done(
	struct kwl_client *client,
	uint32_t id)
{
	uint32_t serial;

	/* Queues the done with the serial of the state told last. */
	serial = settings_state.serial;
	(void)kwl_emit(client, id, KL_SYSTEM_SETTINGS_EVENT_DONE, &serial, sizeof(serial));
}

/* Answers a request: whether it was applied, and whether it will be kept (stored: the session's end writes the file). */
static void
settings_result(
	struct kwl_client *client,
	uint32_t id,
	uint32_t request,
	uint32_t applied,
	int stored)
{
	uint32_t words[3];

	/* Answers the request by its number, with whether it was applied. */
	words[0] = request;
	words[1] = applied;

	/*
	 * saved: a setting applied is kept when the store has a file to write
	 * at the session's end; without a home it lives for the session only.
	 */
	words[2] = applied;
	if (applied == KL_SYSTEM_RESULT_OK && !stored)
		words[2] = KL_SYSTEM_RESULT_NOT_SAVED;

	/* Queues the answer to the client. */
	(void)kwl_emit(client, id, KL_SYSTEM_SETTINGS_EVENT_RESULT, words, sizeof(words));

	/* Logs the answer, for the tests. */
	printf("KWL SETTINGS result client=%llu request=%u applied=%u saved=%u\n", (unsigned long long)client->number, request, words[1], words[2]);
}

/* Reads a set (request, key, value) or a reset (request, key) and answers it; returns 0 or EPROTO for a malformed request. */
static int
settings_set(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size,
	int reset)
{
	const char *shown;
	uint32_t request;
	uint32_t applied;
	char *name;
	char *value;
	size_t next;
	size_t end;
	int answered;
	int valid;
	int error;

	/* A request too short for its number is malformed. */
	if (size < 4U)
		return EPROTO;

	/* Reads the request's number and the key. */
	request = settings_word(bytes, 0U);
	error = settings_read_string(bytes, size, 4U, &name, &next);
	if (error != 0)
		return EPROTO;

	/* Reads a set's value; a reset has none. */
	value = NULL;
	end = next;
	if (!reset) {
		error = settings_read_string(bytes, size, next, &value, &end);
		if (error != 0) {
			free(name);
			return EPROTO;
		}
	}

	/* Refuses bytes that trail the last argument. */
	if (end != size) {
		free(value);
		free(name);
		return EPROTO;
	}

	/* Makes the change, and answers it now unless the wallpaper is being read. */
	answered = 0;
	applied = settings_change(object, request, name, value, reset, &answered);
	if (!answered)
		settings_result(object->client, object->id, request, applied, object->client->server->settings->present);

	/* The log names a well-formed key only (a client's bytes are not written as they are). */
	shown = "-";
	valid = kl_settings_name_valid(name);
	if (valid)
		shown = name;

	/* Logs the request, for the tests. */
	printf("KWL SETTINGS %s key=%s client=%llu applied=%u\n", reset ? "reset" : "set", shown, (unsigned long long)object->client->number, applied);

	/* Lets the request's copies go. */
	free(value);
	free(name);

	/* Succeeded: the request was read. */
	return 0;
}

/*
 * Makes a client's change of a setting, puts it into effect and marks it to
 * be told.  Returns the result to answer with; *answered is set when the
 * answer comes later (the wallpaper).
 */
static uint32_t
settings_change(
	struct kwl_object *object,
	uint32_t request,
	const char *name,
	const char *value,
	int reset,
	int *answered)
{
	struct kwl_server *server;
	const struct kl_settings_key *key;
	uint32_t applied;
	int differs;
	int valid;
	int error;

	/* Finds the compositor the request reached. */
	server = object->client->server;

	/* Refuses a malformed name. */
	valid = kl_settings_name_valid(name);
	if (!valid)
		return KL_SYSTEM_RESULT_INVALID;

	/* Refuses a key the table lacks or that the compositor does not resolve. */
	key = kl_settings_key_find(name);
	if (key == NULL || key->resolver != KL_SETTINGS_RESOLVER_COMPOSITOR)
		return KL_SYSTEM_RESULT_UNSUPPORTED;

	/* Refuses a change to a key nobody may change. */
	if ((key->flags & KL_SETTINGS_KEY_READ_ONLY) != 0U)
		return KL_SYSTEM_RESULT_DENIED;

	/* A value of its type and within its range. */
	if (!reset) {
		error = kl_settings_key_check(key, value);
		if (error != 0)
			return KL_SYSTEM_RESULT_INVALID;
	}

	/* The sound is audiod's: it goes there through volume.c. */
	differs = strncmp(name, "sound.", 6U);
	if (differs == 0) {
		applied = settings_change_sound(server, key, value, reset);

		/* Reports audiod's answer. */
		return applied;
	}

	/* The wallpaper is read away from the event loop. */
	differs = strcmp(name, "wallpaper");
	if (differs == 0) {
		applied = settings_change_wallpaper(object, request, value, reset, answered);

		/* Reports whether the reading started. */
		return applied;
	}

	/* Puts any other setting in the store. */
	if (reset) {
		error = kwl_settings_store_reset(server->settings, name);
	} else {
		error = kwl_settings_store_choose(server->settings, name, value);
	}

	/* Answers a change the store refused with its reason. */
	if (error != 0) {
		applied = settings_result_of(error);
		return applied;
	}

	/* Puts the setting into effect, and tells every settings object. */
	settings_apply(server, name, 0);
	settings_mark(server, name);
	settings_flush(server);

	/* Succeeded: the setting is in effect. */
	return KL_SYSTEM_RESULT_OK;
}

/* Sends a client's volume or mute to audiod (through volume.c); the store takes audiod's report. */
static uint32_t
settings_change_sound(
	struct kwl_server *server,
	const struct kl_settings_key *key,
	const char *value,
	int reset)
{
	unsigned restored;
	unsigned available;
	unsigned volume;
	unsigned muted;
	uint32_t applied;
	int differs;
	int number;
	int error;

	/* Takes the volume and mute as they are. */
	kwl_volume_report(&restored, &available, &volume, &muted);

	/* Reads the number asked for; a reset asks for the table's default. */
	number = key->fallback;
	if (!reset)
		(void)kl_settings_key_number(key, value, &number);

	/* The volume's key changes the volume, the mute's key the mute. */
	differs = strcmp(key->name, "sound.volume");
	if (differs == 0) {
		volume = (unsigned)number;
	} else {
		muted = (unsigned)number;
	}

	/* Sends the volume and mute to audiod; a refusal is answered with its reason. */
	error = kwl_volume_request(server, volume, muted);
	if (error != 0) {
		applied = settings_result_of(error);
		return applied;
	}

	/* Succeeded: audiod has it; its report is told when it comes. */
	return KL_SYSTEM_RESULT_OK;
}

/* Starts reading a client's wallpaper (or the default's) on glass.c's thread; the answer waits for it. */
static uint32_t
settings_change_wallpaper(
	struct kwl_object *object,
	uint32_t request,
	const char *value,
	int reset,
	int *answered)
{
	struct kwl_server *server;
	struct kwl_settings_entry *entry;
	const char *path;
	uint32_t applied;
	int error;

	/* Finds the compositor the request reached. */
	server = object->client->server;

	/* One wallpaper at a time. */
	if (settings_state.waiting.active)
		return KL_SYSTEM_RESULT_BUSY;

	/* The path: the client's, or the command line's for a reset. */
	path = value;
	if (reset)
		path = server->wallpaper_started;

	/* A reset to the landscape needs no file: in effect now. */
	if (path == NULL) {
		/* Puts the setting back at its default; a refusal is answered with its reason. */
		error = kwl_settings_store_reset(server->settings, "wallpaper");
		if (error != 0) {
			applied = settings_result_of(error);
			return applied;
		}

		/* Shows the landscape when the store holds the setting. */
		entry = kwl_settings_store_find(server->settings, "wallpaper");
		if (entry != NULL)
			settings_apply_wallpaper(server, 0);

		/* Tells every settings object. */
		settings_mark(server, "wallpaper");
		settings_flush(server);

		/* Succeeded: the landscape is in effect. */
		return KL_SYSTEM_RESULT_OK;
	}

	/* Starts glass.c's thread; a path that is not an ordinary file is refused now. */
	error = kwl_glass_wallpaper_begin(server, path);
	if (error != 0) {
		applied = settings_result_of(error);
		return applied;
	}

	/*
	 * Keeps the request until the picture is read: active makes the tick
	 * answer it, and refuses another wallpaper until then.
	 */
	settings_state.waiting.client = object->client->number;
	settings_state.waiting.object = object->id;
	settings_state.waiting.request = request;
	settings_state.waiting.reset = (unsigned)reset;
	settings_state.waiting.active = 1;
	(void)snprintf(settings_state.waiting.path, sizeof(settings_state.waiting.path), "%s", path);

	/* Tells the caller that the answer comes later. */
	*answered = 1;

	/* Succeeded: the answer comes once the picture is read. */
	return KL_SYSTEM_RESULT_OK;
}

/* Takes the wallpaper glass.c read: in the store and told when it is shown, and the waiting client answered either way. */
static void
settings_wallpaper_done(
	struct kwl_server *server,
	int error)
{
	struct settings_waiting *waiting;
	struct kwl_client *client;
	struct kwl_object *object;
	uint32_t applied;
	int stored;

	/* Nothing waits (the look went, or the reader was another's). */
	waiting = &settings_state.waiting;
	if (!waiting->active)
		return;

	/* The request is answered here, so another wallpaper may be asked for from now on. */
	waiting->active = 0;

	/* A picture that was not shown fails the request. */
	applied = KL_SYSTEM_RESULT_FAILED;
	if (error == 0) {
		/* The picture is shown: the request succeeded. */
		applied = KL_SYSTEM_RESULT_OK;

		/* The setting holds the choice now: its default after a reset, else the path. */
		if (waiting->reset) {
			(void)kwl_settings_store_reset(server->settings, "wallpaper");
			server->wallpaper_chosen[0] = '\0';
		} else {
			(void)kwl_settings_store_choose(server->settings, "wallpaper", waiting->path);
			(void)snprintf(server->wallpaper_chosen, sizeof(server->wallpaper_chosen), "%s", waiting->path);
		}

		/* The picture shown is the setting's. */
		server->wallpaper_path = server->wallpaper_started;
		if (server->wallpaper_chosen[0] != '\0')
			server->wallpaper_path = server->wallpaper_chosen;

		/* Logs the wallpaper put into effect, for the tests. */
		printf("KWL PREFERENCES key=wallpaper applied\n");

		/* Tells every settings object. */
		settings_mark(server, "wallpaper");
		settings_flush(server);
	}

	/* Answers the client, if it is still there with its settings object. */
	stored = server->settings->present;
	for (client = server->clients;
	     client != NULL;
	     client = client->next) {
		/* Only the waiting client, while it is still served. */
		if (client->number != waiting->client || client->fatal)
			continue;

		/* Its settings object must still be alive to hear the answer. */
		object = kwl_find(client, waiting->object);
		if (object == NULL ||
		    object->dead ||
		    object->kind != KWL_SYSTEM_SETTINGS)
			break;

		/* Sends the answer. */
		settings_result(client, object->id, waiting->request, applied, stored);
		break;
	}
}

/* Takes the sound as audiod reports it into the store (once audiod took the kept volume), and marks what changed. */
static void
settings_sound(
	struct kwl_server *server)
{
	struct kwl_settings_entry *entry;
	unsigned restored;
	unsigned available;
	unsigned value;
	unsigned muted;
	char text[16];
	int differs;

	/* Takes what volume.c knows of the sound. */
	kwl_volume_report(&restored, &available, &value, &muted);

	/* Whether there is sound, told when it changes. */
	(void)snprintf(text, sizeof(text), "%u", available);
	entry = kwl_settings_store_find(server->settings, "sound.available");
	if (entry != NULL) {
		/* Reports a value that changed, or that was not known yet. */
		differs = strcmp(entry->value, text);
		if (differs != 0 || !entry->known) {
			kwl_settings_store_report(server->settings, "sound.available", text);
			settings_mark(server, "sound.available");
		}
	}

	/* The volume is the session's only once audiod took the kept one. */
	if (!restored || !available)
		return;

	/* Takes the volume, told when it changes. */
	(void)snprintf(text, sizeof(text), "%u", value);
	entry = kwl_settings_store_find(server->settings, "sound.volume");
	if (entry != NULL) {
		/* Reports a value that changed, or that was not known yet. */
		differs = strcmp(entry->value, text);
		if (differs != 0 || !entry->known) {
			kwl_settings_store_report(server->settings, "sound.volume", text);
			settings_mark(server, "sound.volume");
		}
	}

	/* Takes the mute, told when it changes. */
	(void)snprintf(text, sizeof(text), "%u", muted);
	entry = kwl_settings_store_find(server->settings, "sound.muted");
	if (entry != NULL) {
		/* Reports a value that changed, or that was not known yet. */
		differs = strcmp(entry->value, text);
		if (differs != 0 || !entry->known) {
			kwl_settings_store_report(server->settings, "sound.muted", text);
			settings_mark(server, "sound.muted");
		}
	}
}

/* Tells a new settings object every setting and a done. */
static int
settings_snapshot(
	struct kwl_object *settings)
{
	struct kwl_settings_store *store;
	unsigned index;
	int error;

	/* Tells every compositor setting the store holds, when there is a store. */
	store = settings->client->server->settings;
	if (store != NULL) {
		for (index = 0; index < store->count; index++) {
			error = settings_emit_value(settings->client, settings->id, &store->entries[index]);
			if (error != 0)
				return error;
		}
	}

	/* Sends the done that makes them one state. */
	settings_emit_done(settings->client, settings->id);

	/* Logs the snapshot, for the tests. */
	printf("KWL SETTINGS snapshot client=%llu object=%u\n", (unsigned long long)settings->client->number, settings->id);

	/* Succeeded: the object knows every setting. */
	return 0;
}

/* Gives the result an errno value of a change means. */
static uint32_t
settings_result_of(
	int error)
{
	/* Each errno value the changes give. */
	switch (error) {
	case 0:
		return KL_SYSTEM_RESULT_OK;
	case EBUSY:
		return KL_SYSTEM_RESULT_BUSY;
	case EINVAL:
	case ENAMETOOLONG:
		return KL_SYSTEM_RESULT_INVALID;
	case EPERM:
		return KL_SYSTEM_RESULT_DENIED;
	case ENOENT:
		return KL_SYSTEM_RESULT_UNSUPPORTED;
	case ENODEV:
		return KL_SYSTEM_RESULT_UNAVAILABLE;
	default:
		break;
	}

	/* Anything else failed. */
	return KL_SYSTEM_RESULT_FAILED;
}

/* Writes a string argument and gives the offset after it. */
static size_t
settings_put_string(
	unsigned char *payload,
	size_t offset,
	const char *text)
{
	uint32_t length;
	size_t padded;

	/* The length with the NUL, the bytes, and zeros to a four-byte boundary. */
	length = (uint32_t)strlen(text) + 1U;
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	memcpy(payload + offset, &length, sizeof(length));
	memset(payload + offset + 4U, 0, padded);
	memcpy(payload + offset + 4U, text, length - 1U);

	/* The offset after the string. */
	return offset + 4U + padded;
}

/* Reads a string argument into an allocated copy; *next is the offset after it.  Returns 0, EPROTO or ENOMEM. */
static int
settings_read_string(
	const unsigned char *bytes,
	size_t size,
	size_t offset,
	char **text,
	size_t *next)
{
	uint32_t length;
	size_t padded;
	char *copy;

	/* The length word must be within the request. */
	if (offset + 4U > size)
		return EPROTO;

	/* Reads the length, which counts the NUL; an empty or too long string is malformed. */
	length = settings_word(bytes, offset);
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	if (length == 0U || length > SETTINGS_WIRE_TEXT_MAX)
		return EPROTO;

	/* The padded text must be within the request. */
	if (offset + 4U + padded > size)
		return EPROTO;

	/* The text must end with its NUL. */
	if (bytes[offset + 4U + length - 1U] != '\0')
		return EPROTO;

	/* Copies the text for the caller, who frees it. */
	copy = malloc(length);
	if (copy == NULL)
		return ENOMEM;
	memcpy(copy, bytes + offset + 4U, length);

	/* Gives the caller the copy and where the next argument starts. */
	*text = copy;
	*next = offset + 4U + padded;

	/* Succeeded: the string is read. */
	return 0;
}

/* Reads a 32-bit word of a request in the wire's native byte order. */
static uint32_t
settings_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* Copies the word out, since a request's bytes need not be aligned. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Reports the word read. */
	return word;
}
