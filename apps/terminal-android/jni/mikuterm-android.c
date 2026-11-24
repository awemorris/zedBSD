/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mikuterm's body on Android: what the Windows program's WinMain does, on the
 * program's native thread.  It unpacks the program's read-only data, reads the
 * settings, finds the fonts, and runs the terminal (run.h) on libkeiland's
 * Android layer until the last tab or the window closes.  Each tab connects
 * over SSH on a thread of its own (connection-ssh.c, ssh-thread.c).
 *
 * A build made with the echo connection (echo-connection.h) opens tabs with it
 * instead, so that the terminal's screen, keys and touch can be tried without
 * a network.
 */

#include "apps/libkeiland/android/keiland-android.h"
#include "apps/terminal-android/jni/mikuterm-android.h"

#include "mikuterm.h"
#include "session.h"
#include "sftp-link.h"
#include "sftp-view.h"
#include "sftp-worker.h"
#include "x11-thread.h"

#include "userland/desktop/terminal/run.h"

#if MIKUTERM_ANDROID_ECHO
#include "apps/terminal-android/jni/echo-connection.h"
#endif

#include "mikuterm-version.h"

#include <android/log.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The user's settings and the View menu's, inside the folder. */
#define MAIN_CONF		"config/mikuterm.conf"

/* The fonts the program's data brings (share/fonts): the signs' and box drawing's. */
#define MAIN_COMPANION		"keiland-fallback-mono.ttf"

/* The fonts the settings' tab draws its words with (share/fonts): the interface's and its fallback. */
#define MAIN_PAGE_FONT		"keiland.ttf"
#define MAIN_PAGE_FALLBACK	"keiland-fallback.ttf"

/* How long the program waits at its end for the connections' threads to say goodbye, in milliseconds. */
#define MAIN_FAREWELL_MS	3000U

/* The capacity of a tab's local notice buffer (network changes). */
#define MAIN_NOTICE_MAX		256U

/* What Help > About writes on the screen. */
#define MAIN_ABOUT		"\r\n\033[1mmikuterm\033[0m " MIKUTERM_VERSION_TEXT \
				" (Micro Unicode Terminal): zedBSD's terminal and SSH client for Android.\r\n"

/*
 * Wraps an opened tab connection on M so that the program tracks how many
 * sessions are active (for the foreground service), can close all tabs on a
 * stop-all request, and can inject local notices on network changes.
 */
struct main_session {
	struct terminal_connection base;
	struct terminal_connection *inner;
	struct main_session *next;
	int stop_requested;
	char notice[MAIN_NOTICE_MAX];
	size_t notice_offset;
	size_t notice_length;
};

/*
 * The run's settings and paths, for the whole program: they are given to the
 * run, which keeps pointers to them.
 */
struct main_state {
	struct mikuterm_conf conf;
	struct mikuterm_connector connector;
	struct terminal_connector wrapped_connector;
	struct terminal_connector *inner_connector;
	char font[MIKUTERM_PATH_MAX];
	char fallback[MIKUTERM_PATH_MAX];
	char companion[MIKUTERM_PATH_MAX];
	char page_font[MIKUTERM_PATH_MAX];
	char page_fallback[MIKUTERM_PATH_MAX];
};

/* The program's state (large, and needed for the whole run). */
static struct main_state main_state;

/* Active wrapped sessions on M, their count, the open kl_app, and whether the default network was lost. */
static struct main_session *main_sessions;
static unsigned main_session_count;
static struct kl_app *main_app;
static int main_network_lost;

static void main_read_conf(struct mikuterm_conf *conf);
#if !MIKUTERM_ANDROID_ECHO
static void main_connector_init(struct mikuterm_connector *connector, const struct mikuterm_conf *conf);
#endif
static void main_forget_passphrases(void *data);
static void main_ended(void *data, const struct terminal_run_result *result);
static int main_can_open_sftp(void *data, const struct terminal_connection *connection);
static int main_open_sftp(void *data, struct terminal_connection *connection);
static void main_sftp_on_upload(void *data, struct sftp_view *view);
static void main_sftp_on_change_download_dir(void *data, struct sftp_view *view);
static struct sftp_view *main_find_sftp_view(uint32_t view_number);
static void main_load_sftp_download_uri(void);
static void main_save_sftp_download_uri(const char *uri);
static int main_connector_open(void *data, struct kl_app *app, uint32_t id, unsigned columns, unsigned rows, unsigned width, unsigned height, char *title, size_t size, struct terminal_connection **connection);
static size_t main_session_read(struct terminal_connection *connection, unsigned char *buffer, size_t size);
static int main_session_write(struct terminal_connection *connection, const unsigned char *bytes, size_t size, size_t *written);
static int main_session_resize(struct terminal_connection *connection, unsigned columns, unsigned rows, unsigned width, unsigned height);
static int main_session_mode(const struct terminal_connection *connection, unsigned *mode);
static unsigned main_session_event(struct terminal_connection *connection, const struct kl_app_event *event);
static unsigned main_session_pending(const struct terminal_connection *connection);
static void main_session_close(struct terminal_connection *connection);
static struct terminal_connection *main_session_take(struct terminal_connection *connection);
static void main_session_notice(struct main_session *session, const char *text);

/* The wrapped connection operations (picture is NULL; X11 picture tabs come from take). */
static const struct terminal_connection_ops main_session_ops = {
	main_session_read,
	main_session_write,
	main_session_resize,
	main_session_mode,
	main_session_event,
	main_session_pending,
	main_session_close,
	main_session_take,
	NULL,
	NULL
};

/*
 * Runs mikuterm until its last tab or its window closes.
 */
void
mikuterm_android_main(void)
{
	const struct terminal_page_program *page;
	struct terminal_run_options run;
	struct terminal_run_result result;
	int status;
	int error;

	/* The read-only data in place (the fonts); a package that holds none leaves the fonts missing. */
	error = android_places_unpack(keiland_android_activity());
	if (error != 0)
		__android_log_print(ANDROID_LOG_WARN, "mikuterm", "the program's data was not unpacked (%d)", error);

	/* The folder everything of the user's is in. */
	error = mikuterm_folder_init();
	if (error != 0) {
		__android_log_print(ANDROID_LOG_ERROR, "mikuterm", "the folder cannot be found (%d)", error);
		return;
	}

	/* The passphrases kept on the disk are asked of Java, and the answers of Java and of the connections' threads come to the hook. */
	keiland_android_set_post_hook(mikuterm_android_post);
	mikuterm_credential_start();

	/* The user's settings, and the View menu's kept over them. */
	main_read_conf(&main_state.conf);
	mikuterm_settings_defaults(&main_state.conf);
	main_load_sftp_download_uri();

	/* The fonts: the user's choice or the program's own, and the companion the program's. */
	error = mikuterm_font(main_state.font, sizeof(main_state.font), main_state.conf.font);
	if (error == 0)
		error = mikuterm_font(main_state.fallback, sizeof(main_state.fallback), main_state.conf.fallback);
	if (error == 0)
		error = mikuterm_font(main_state.companion, sizeof(main_state.companion), MAIN_COMPANION);
	if (error != 0) {
		__android_log_print(ANDROID_LOG_ERROR, "mikuterm", "the fonts' paths are too long (%d)", error);
		return;
	}

	/* The tabs' connections, wrapped to track active sessions for the foreground service. */
#if MIKUTERM_ANDROID_ECHO
	memset(&main_state.connector, 0, sizeof(main_state.connector));
	main_state.connector.connector.open = echo_connector_open;
	main_state.connector.connector.operation = "echo_connector_open";
	main_state.connector.connector.data = NULL;
#else
	main_connector_init(&main_state.connector, &main_state.conf);
#endif
	main_state.inner_connector = &main_state.connector.connector;
	main_state.wrapped_connector.open = main_connector_open;
	main_state.wrapped_connector.operation = main_state.inner_connector->operation;
	main_state.wrapped_connector.data = &main_state;
	main_sessions = NULL;
	main_session_count = 0U;
	main_app = NULL;
	main_network_lost = 0;

	/* The settings' tab: the folder's ssh/ files and mikuterm.conf (none when the folder's path is too long for them). */
	error = mikuterm_page_init(&main_state.conf, &main_state.connector, &page);
	if (error != 0)
		page = NULL;
	error = mikuterm_font(main_state.page_font, sizeof(main_state.page_font), MAIN_PAGE_FONT);
	if (error == 0)
		error = mikuterm_font(main_state.page_fallback, sizeof(main_state.page_fallback), MAIN_PAGE_FALLBACK);
	if (error != 0)
		page = NULL;

	/*
	 * The window: drawn on the processor at the screen's scale, the grid the
	 * settings give (the window is the screen's, so the grid follows its size),
	 * with no new window and no emoji font yet.
	 */
	memset(&run, 0, sizeof(run));
	run.application = "mikuterm";
	run.display = NULL;
	run.title = "mikuterm";
	run.present = KL_PRESENT_SHM;
	run.font = main_state.font;
	run.companion = main_state.companion;
	run.fallback = main_state.fallback;
	run.emoji = NULL;
	run.pixels = main_state.conf.font_size;
	run.pixels_given = 0;
	run.scale = keiland_android_scale();
	run.columns = main_state.conf.columns;
	run.rows = main_state.conf.rows;
	run.timeout = 0U;
	run.token = "mikuterm";
	run.about = MAIN_ABOUT;
	run.input_method_menu = 0;
	run.open_sftp_menu = 1;
	run.can_open_sftp = main_can_open_sftp;
	run.open_sftp = main_open_sftp;
	run.ambiguous_wide_key = 0;
	run.fullscreen_alt_enter = 0;
	run.forget_passphrases = main_forget_passphrases;
	run.page = page;
	run.page_font = main_state.page_font;
	run.page_fallback = main_state.page_fallback;
	run.connector = &main_state.wrapped_connector;
	run.new_window = NULL;
	run.ended = main_ended;
	run.data = &main_state;

	/* The run, until the last tab or the window closes; then the threads have a while to say goodbye. */
	status = terminal_run(&run, &result);
	(void)mikuterm_connections_wait(MAIN_FAREWELL_MS);
	if (status != 0)
		__android_log_print(ANDROID_LOG_ERROR, "mikuterm", "the run failed: %s (result %d, errno %d)", result.operation, result.result, result.error);

	/* Stops the foreground service and closes the Activity. */
	main_app = NULL;
	main_sessions = NULL;
	main_session_count = 0U;
	main_network_lost = 0;
	(void)mikuterm_java_service_update(0U);
	keiland_android_quit();
}

/*
 * M's hook for the posts of Java and of the connections' threads: a secret's
 * answer, a passphrase to keep or forget on the disk, the network and the ask
 * to close every connection.  Returns 1 when the post was one of these.
 */
int
mikuterm_android_post(
	uint32_t code,
	void *data)
{
	struct mikuterm_post_result *net;
	struct main_session *session;

	/* By code. */
	switch (code) {
	case MIKUTERM_POST_SECRET:
		mikuterm_credential_answer(data);
		return 1;
	case MIKUTERM_POST_KEEP_SECRET:
		mikuterm_credential_keep(data);
		return 1;
	case MIKUTERM_POST_FORGET_SECRET:
		mikuterm_credential_forget(data);
		return 1;
	case MIKUTERM_POST_NETWORK:
		/* Tells open tabs when the default network drops or returns after a drop. */
		net = data;
		if (net != NULL) {
			if (!net->ok) {
				main_network_lost = 1;
				for (session = main_sessions; session != NULL; session = session->next)
					main_session_notice(session, "\r\nThe network connection was lost.\r\n");
				if (main_app != NULL)
					(void)kl_app_post(main_app, 0U, NULL);
			} else if (main_network_lost) {
				main_network_lost = 0;
				for (session = main_sessions; session != NULL; session = session->next)
					main_session_notice(session, "\r\nThe network is available again.\r\n");
				if (main_app != NULL)
					(void)kl_app_post(main_app, 0U, NULL);
			}

			free(net);
		}

		return 1;
	case MIKUTERM_POST_STOP_ALL:
		/* Stops every connection thread and asks every open tab to close. */
		mikuterm_connections_stop();
		for (session = main_sessions; session != NULL; session = session->next)
			session->stop_requested = 1;
		if (main_app != NULL)
			(void)kl_app_post(main_app, 0U, NULL);
		return 1;
	case MIKUTERM_POST_SFTP_UPLOAD: {
		struct mikuterm_post_sftp_upload *up = data;
		struct sftp_view *view;
		size_t i;

		if (up != NULL) {
			view = main_find_sftp_view(up->view_number);
			if (view != NULL && view->worker != NULL) {
				for (i = 0U; i < up->count; i++)
					(void)sftp_worker_enqueue_upload(view->worker, up->paths[i], view->current_path, 0, NULL);
				view->dirty = 1;
				if (main_app != NULL)
					(void)kl_app_post(main_app, 0U, NULL);
			}
			for (i = 0U; i < up->count; i++)
				free(up->paths[i]);
			free(up->paths);
			free(up);
		}
		return 1;
	}
	case MIKUTERM_POST_SFTP_DOWNLOAD_DIR: {
		struct mikuterm_post_sftp_dir *dir = data;
		struct sftp_view *view;

		if (dir != NULL) {
			if (dir->tree_uri != NULL) {
				sftp_local_android_set_download_uri(dir->tree_uri);
				main_save_sftp_download_uri(dir->tree_uri);
			}
			view = main_find_sftp_view(dir->view_number);
			if (view != NULL) {
				view->dirty = 1;
				if (main_app != NULL)
					(void)kl_app_post(main_app, 0U, NULL);
			}
			free(dir->tree_uri);
			free(dir->virtual_dir);
			free(dir);
		}
		return 1;
	}
	default:
		break;
	}

	/* Not one of these. */
	return 0;
}

/*
 * Finds a font named in the settings: a path from the root as it is, else the
 * user's fonts/ folder, else the program's own (share/fonts of the program's
 * data).  Returns 0 or ENAMETOOLONG.
 */
int
mikuterm_font(
	char *path,
	size_t size,
	const char *name)
{
	char inside[MIKUTERM_PATH_MAX];
	char directory[MIKUTERM_PATH_MAX];
	size_t length;
	int written;
	int exists;
	int error;

	/* A path from the root. */
	length = strlen(name);
	if (name[0] == '/') {
		if (length + 1U > size)
			return ENAMETOOLONG;
		memcpy(path, name, length + 1U);
		return 0;
	}

	/* The user's fonts first. */
	written = snprintf(inside, sizeof(inside), "fonts/%s", name);
	if (written < 0 || (size_t)written >= sizeof(inside))
		return ENAMETOOLONG;
	error = mikuterm_path(path, size, inside);
	if (error != 0)
		return error;
	exists = mikuterm_file_exists(path);
	if (exists)
		return 0;

	/* The program's own. */
	error = kl_data_directory(directory, sizeof(directory));
	if (error != 0)
		return error;
	written = snprintf(path, size, "%s/fonts/%s", directory, name);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the path (the run says so when the font is not there). */
	return 0;
}

#if !MIKUTERM_ANDROID_ECHO
/* Makes the tabs' connector from the settings: SSH connections, the first to the Connect host when the settings name one. */
static void
main_connector_init(
	struct mikuterm_connector *connector,
	const struct mikuterm_conf *conf)
{
	struct mikuterm_connect_options connect;

	/* What the settings say of the connections. */
	memset(&connect, 0, sizeof(connect));
	connect.term = conf->term;
	connect.connect = conf->connect;
	connect.connect_timeout = conf->connect_timeout;
	connect.wsl_port = conf->wsl_port;
	connect.x11_scale = conf->x11_scale;
	connect.x11_render_cpu = conf->x11_render_cpu;
	connect.x11_primary = conf->x11_primary;
	connect.sftp = conf->sftp;
	connect.notice = conf->problems;
	mikuterm_connector_init(connector, &connect);
}
#endif

/* Reads config/mikuterm.conf into the settings (its defaults when it is not there). */
static void
main_read_conf(
	struct mikuterm_conf *conf)
{
	char path[MIKUTERM_PATH_MAX];
	char shown[sizeof(MAIN_CONF)];
	char *text;
	size_t size;
	int error;

	/* The defaults. */
	mikuterm_conf_defaults(conf);

	/* The file, when it is there; one that cannot be read is a problem to show. */
	error = mikuterm_path(path, sizeof(path), MAIN_CONF);
	if (error != 0)
		return;
	error = mikuterm_file_read(path, MIKUTERM_CONF_MAX, &text, &size);
	if (error == ENOENT)
		return;
	if (error != 0) {
		mikuterm_path_shown(MAIN_CONF, shown, sizeof(shown));
		(void)snprintf(conf->problems, sizeof(conf->problems), "%s could not be read (errno %d).\r\n", shown, error);
		return;
	}

	/* What it says. */
	(void)mikuterm_conf_read(conf, text, size, 0);
	free(text);
}

/*
 * Hears how the run ended, before the application closes: the connections'
 * threads post no more from now on.
 */
static void
main_ended(
	void *data,
	const struct terminal_run_result *result)
{
	/* The application is closing. */
	(void)data;
	(void)result;
	mikuterm_connections_stop();
}

/* Forgets every passphrase mikuterm keeps (Session > Forget Remembered Passphrases). */
static void
main_forget_passphrases(
	void *data)
{
	/* Nothing of the program's state is needed. */
	(void)data;

	/* Every passphrase kept goes. */
	(void)mikuterm_passphrases_forget_all();
}

/* Unwraps a main_session connection to its inner mikuterm_session connection. */
static struct terminal_connection *
main_unwrap_connection(
	const struct terminal_connection *connection)
{
	if (connection != NULL && connection->ops == &main_session_ops)
		return ((const struct main_session *)connection)->inner;
	return (struct terminal_connection *)(uintptr_t)connection;
}

/* Reports whether Shell > Open SFTP is available for connection. */
static int
main_can_open_sftp(
	void *data,
	const struct terminal_connection *connection)
{
	(void)data;
	return mikuterm_session_can_open_sftp(main_unwrap_connection(connection));
}

/* Opens an SFTP browser tab on the active SSH connection. */
static int
main_open_sftp(
	void *data,
	struct terminal_connection *connection)
{
	struct main_state *st;
	struct terminal_connection *inner;
	struct mikuterm_session *session;
	struct sftp_link *link;
	struct terminal_connection *view_conn;
	struct sftp_view *view;
	char host[MIKUTERM_HOST_MAX];
	int error;

	st = data;
	inner = main_unwrap_connection(connection);
	if (!mikuterm_session_can_open_sftp(inner))
		return 0;
	session = (struct mikuterm_session *)inner;

	error = sftp_link_create(session->waker, &link);
	if (error != 0)
		return 0;

	mikuterm_session_host(inner, host, sizeof(host));
	view_conn = NULL;
	error = sftp_view_create(
		session->app,
		session,
		link,
		host,
		st->conf.sftp_preserve_times,
		st->conf.sftp_show_hidden,
		st->page_font,
		st->page_fallback,
		&view_conn);
	if (error != 0) {
		sftp_link_release(link);
		return 0;
	}

	view = (struct sftp_view *)view_conn;
	view->on_upload = main_sftp_on_upload;
	view->on_change_download_dir = main_sftp_on_change_download_dir;
	view->platform_data = view;

	error = mikuterm_session_open_sftp(inner, view_conn, link);
	sftp_link_release(link);
	if (error != 0) {
		view_conn->ops->close(view_conn);
		return 0;
	}
	return 0;
}

static void
main_sftp_on_upload(
	void *data,
	struct sftp_view *view)
{
	(void)data;
	if (view != NULL)
		(void)mikuterm_java_sftp_pick_upload(view->number);
}

static void
main_sftp_on_change_download_dir(
	void *data,
	struct sftp_view *view)
{
	(void)data;
	if (view != NULL)
		(void)mikuterm_java_sftp_pick_download_dir(view->number);
}

static struct sftp_view *
main_find_sftp_view(
	uint32_t view_number)
{
	struct main_session *s;
	struct terminal_connection *vc;
	struct sftp_view *v;

	for (s = main_sessions; s != NULL; s = s->next) {
		if (s->inner == NULL)
			continue;
		vc = mikuterm_session_sftp_view(s->inner);
		if (vc != NULL) {
			v = (struct sftp_view *)vc;
			if (v->number == view_number)
				return v;
		}
	}
	return NULL;
}

static void
main_load_sftp_download_uri(void)
{
	char path[MIKUTERM_PATH_MAX];
	char *text;
	size_t text_size;
	const char *p;
	const char *end;
	const char *eol;
	size_t klen;
	size_t vlen;
	char uri[MIKUTERM_PATH_MAX];
	int error;

	error = mikuterm_path(path, sizeof(path), "config/state.conf");
	if (error != 0)
		return;
	text = NULL;
	text_size = 0U;
	if (mikuterm_file_read(path, MIKUTERM_CONF_MAX, &text, &text_size) != 0 || text == NULL)
		return;

	p = text;
	end = text + text_size;
	klen = strlen("SftpDownloadUri=");
	while (p < end) {
		eol = memchr(p, '\n', (size_t)(end - p));
		if (eol == NULL)
			eol = end;
		if ((size_t)(eol - p) > klen && strncmp(p, "SftpDownloadUri=", klen) == 0) {
			vlen = (size_t)(eol - (p + klen));
			if (vlen > 0U && p[klen + vlen - 1U] == '\r')
				vlen--;
			if (vlen < sizeof(uri)) {
				memcpy(uri, p + klen, vlen);
				uri[vlen] = '\0';
				sftp_local_android_set_download_uri(uri);
			}
		}
		p = (eol < end) ? eol + 1 : end;
	}
	free(text);
}

static void
main_save_sftp_download_uri(
	const char *uri)
{
	char path[MIKUTERM_PATH_MAX];
	char *text;
	char *made;
	size_t text_size;
	size_t made_size;
	int error;

	if (uri == NULL)
		return;
	if (mikuterm_path(path, sizeof(path), "config/state.conf") != 0)
		return;
	text = NULL;
	text_size = 0U;
	error = mikuterm_file_read(path, MIKUTERM_CONF_MAX, &text, &text_size);
	if (error != 0 && error != ENOENT)
		return;
	error = mikuterm_conf_set(text != NULL ? text : "", text_size, "SftpDownloadUri", uri, &made, &made_size);
	free(text);
	if (error == 0) {
		(void)mikuterm_file_replace(path, made, made_size);
		free(made);
	}
}

/* Android closes SFTP tabs without blocking the native thread on a synchronous dialog. */
int
sftp_view_platform_confirm_close(
	size_t active_transfers)
{
	(void)active_transfers;
	return 1;
}

/*
 * Returns the logical window size and X scale percent for the X server thread.
 */
void
mikuterm_x11_screen(
	unsigned *width,
	unsigned *height,
	unsigned *percent)
{
	unsigned scale;

	scale = 0U;
	keiland_android_screen(width, height, &scale);
	if (percent != NULL)
		*percent = (scale != 0U) ? (scale * 100U / KL_SCALE_BASE) : 0U;
}

/* Opens a tab's connection through the inner connector and tracks it for the foreground service. */
static int
main_connector_open(
	void *data,
	struct kl_app *app,
	uint32_t id,
	unsigned columns,
	unsigned rows,
	unsigned width,
	unsigned height,
	char *title,
	size_t size,
	struct terminal_connection **connection)
{
	struct main_state *state;
	struct terminal_connection *inner;
	struct main_session *session;
	int error;

	/* Opens the underlying connection. */
	state = data;
	inner = NULL;
	error = state->inner_connector->open(
		state->inner_connector->data,
		app,
		id,
		columns,
		rows,
		width,
		height,
		title,
		size,
		&inner);
	if (error != 0)
		return error;

	/* Wraps it so the program can count active sessions, close all on request, and inject notices. */
	session = calloc(1U, sizeof(*session));
	if (session == NULL) {
		inner->ops->close(inner);
		return ENOMEM;
	}

	session->base.ops = &main_session_ops;
	session->inner = inner;
	session->next = main_sessions;
	main_sessions = session;
	main_app = app;
	main_session_count++;
	(void)mikuterm_java_service_update(main_session_count);

	/* Succeeded. */
	*connection = &session->base;
	return 0;
}

/* Reads any pending local notice first, then delegates to the underlying connection. */
static size_t
main_session_read(
	struct terminal_connection *connection,
	unsigned char *buffer,
	size_t size)
{
	struct main_session *session;
	size_t avail;
	size_t take;

	/* Emits a queued network notice before reading from the connection. */
	session = (struct main_session *)connection;
	if (session->notice_offset < session->notice_length) {
		avail = session->notice_length - session->notice_offset;
		take = avail < size ? avail : size;
		memcpy(buffer, session->notice + session->notice_offset, take);
		session->notice_offset += take;
		if (session->notice_offset == session->notice_length) {
			session->notice_offset = 0U;
			session->notice_length = 0U;
		}

		return take;
	}

	/* Stops reading once a global disconnect was requested. */
	if (session->stop_requested)
		return 0U;

	/* Reads from the underlying connection. */
	take = session->inner->ops->read(session->inner, buffer, size);
	return take;
}

/* Forwards typed bytes to the underlying connection. */
static int
main_session_write(
	struct terminal_connection *connection,
	const unsigned char *bytes,
	size_t size,
	size_t *written)
{
	struct main_session *session;
	int error;

	/* Writes through the wrapped connection. */
	session = (struct main_session *)connection;
	error = session->inner->ops->write(session->inner, bytes, size, written);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Forwards grid size changes to the underlying connection. */
static int
main_session_resize(
	struct terminal_connection *connection,
	unsigned columns,
	unsigned rows,
	unsigned width,
	unsigned height)
{
	struct main_session *session;
	int error;

	/* Resizes the wrapped connection. */
	session = (struct main_session *)connection;
	error = session->inner->ops->resize(session->inner, columns, rows, width, height);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Forwards mode queries to the underlying connection. */
static int
main_session_mode(
	const struct terminal_connection *connection,
	unsigned *mode)
{
	const struct main_session *session;
	int error;

	/* Queries the wrapped connection's mode. */
	session = (const struct main_session *)connection;
	error = session->inner->ops->mode(session->inner, mode);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Forwards application events and adds local output/exit readiness when needed. */
static unsigned
main_session_event(
	struct terminal_connection *connection,
	const struct kl_app_event *event)
{
	struct main_session *session;
	unsigned ready;

	session = (struct main_session *)connection;
	ready = session->inner->ops->event(session->inner, event);
	if (session->notice_offset < session->notice_length)
		ready |= TERMINAL_READY_OUTPUT;
	if (session->stop_requested)
		ready |= TERMINAL_READY_EXIT;
	return ready;
}

/* Reports pending readiness from the underlying connection plus any local notice or stop request. */
static unsigned
main_session_pending(
	const struct terminal_connection *connection)
{
	const struct main_session *session;
	unsigned ready;

	session = (const struct main_session *)connection;
	ready = session->inner->ops->pending(session->inner);
	if (session->notice_offset < session->notice_length)
		ready |= TERMINAL_READY_OUTPUT;
	if (session->stop_requested)
		ready |= TERMINAL_READY_EXIT;
	return ready;
}

/* Closes the underlying connection and updates the foreground service session count. */
static void
main_session_close(
	struct terminal_connection *connection)
{
	struct main_session *session;
	struct main_session **link;

	/* Unlinks from the active list and updates the foreground service. */
	session = (struct main_session *)connection;
	for (link = &main_sessions; *link != NULL; link = &(*link)->next) {
		if (*link == session) {
			*link = session->next;
			break;
		}
	}

	if (main_session_count > 0U)
		main_session_count--;
	(void)mikuterm_java_service_update(main_session_count);

	/* Closes the underlying connection and frees the wrapper. */
	session->inner->ops->close(session->inner);
	free(session);
}

/* Forwards X11 picture tab requests to the underlying connection. */
static struct terminal_connection *
main_session_take(
	struct terminal_connection *connection)
{
	struct main_session *session;
	struct terminal_connection *taken;

	/* Takes an X11 picture connection when available. */
	session = (struct main_session *)connection;
	if (session->inner->ops->take == NULL)
		return NULL;
	taken = session->inner->ops->take(session->inner);

	/* Succeeded: returns the taken connection or NULL. */
	return taken;
}

/* Queues a local notice line into a session's notice buffer. */
static void
main_session_notice(
	struct main_session *session,
	const char *text)
{
	size_t length;

	length = strlen(text);
	if (length >= sizeof(session->notice))
		length = sizeof(session->notice) - 1U;
	memcpy(session->notice, text, length);
	session->notice[length] = '\0';
	session->notice_offset = 0U;
	session->notice_length = length;
}
