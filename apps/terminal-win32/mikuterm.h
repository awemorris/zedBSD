/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of mikuterm, the terminal of userland/desktop/terminal as a
 * portable Windows program whose tabs are SSH connections.
 *
 * Everything it reads and writes is in the folder mikuterm.exe is in
 * (folder.c): its settings (conf.c reads config\mikuterm.conf and
 * config\state.conf; settings-file.c keeps what the View menu chose in
 * state.conf), the hosts it connects to (ssh\config, in ssh_config's
 * form), the keys and known_hosts (ssh\), and the fonts (fonts\ and
 * share\fonts\).  connection-ssh.c is the tabs' connection on the
 * application's thread; ssh-thread.c runs each connection's SSH engine
 * on a thread of its own.
 */

#ifndef MIKUTERM_H
#define MIKUTERM_H

#include "userland/desktop/terminal/connection.h"

#include <stddef.h>
#include <stdint.h>

/* The longest path mikuterm keeps, in UTF-8 bytes with its NUL. */
#define MIKUTERM_PATH_MAX	1024U

/* The largest settings or ssh_config file mikuterm reads, and the largest known_hosts or key file. */
#define MIKUTERM_CONF_MAX	(256U * 1024U)
#define MIKUTERM_HOSTS_MAX	(1024U * 1024U)

/* The longest text of the problems found in mikuterm.conf, shown in the first tab. */
#define MIKUTERM_PROBLEMS_MAX	2048U

/* The longest TERM name and host given in the settings. */
#define MIKUTERM_TERM_MAX	64U
#define MIKUTERM_HOST_MAX	320U

/*
 * What config\mikuterm.conf says (conf.c), each value its default when
 * the file does not say it.  The *_given fields say which of the View
 * menu's settings the file gave (state.conf wins over them).  problems
 * holds a line for each line the file could not use.
 */
struct mikuterm_conf {
	char font[MIKUTERM_PATH_MAX];
	char fallback[MIKUTERM_PATH_MAX];
	unsigned font_size;
	unsigned columns;
	unsigned rows;
	unsigned theme;
	int ambiguous_wide;
	char term[MIKUTERM_TERM_MAX];
	char connect[MIKUTERM_HOST_MAX];
	int copy_on_select;
	unsigned connect_timeout;
	int font_size_given;
	int theme_given;
	int ambiguous_given;
	char problems[MIKUTERM_PROBLEMS_MAX];
};

/*
 * What a tab's connection needs to know besides the host: the folder's
 * settings, read once at the start and kept for the whole run.
 */
struct mikuterm_connect_options {
	const char *term;
	const char *connect;
	unsigned connect_timeout;
	const char *notice;
};

/*
 * The tabs' connector: each tab connects on a thread of its own, the
 * first one to the Connect host when the settings name one.
 */
struct mikuterm_connector {
	struct terminal_connector connector;
	struct mikuterm_connect_options options;
	int first_opened;
};

/* The folder (folder.c). */
int mikuterm_folder_init(void);
const char *mikuterm_folder(void);
int mikuterm_path(char *path, size_t size, const char *inside);
int mikuterm_ssh_path(char *path, size_t size, const char *written);
int mikuterm_file_read(const char *path, size_t limit, char **text, size_t *size);
int mikuterm_file_append(const char *path, const void *bytes, size_t size);
int mikuterm_file_replace(const char *path, const void *bytes, size_t size);
int mikuterm_file_exists(const char *path);
const char *mikuterm_program(void);

/* The settings files (conf.c). */
void mikuterm_conf_defaults(struct mikuterm_conf *conf);
int mikuterm_conf_read(struct mikuterm_conf *conf, const char *text, size_t size, int state);
int mikuterm_theme_name(const char *name, unsigned *theme);
const char *mikuterm_theme_text(unsigned theme);

/* The View menu's settings kept in state.conf (settings-file.c). */
void mikuterm_settings_defaults(const struct mikuterm_conf *conf);

/* The tabs' connections (connection-ssh.c, ssh-thread.c). */
void mikuterm_connector_init(struct mikuterm_connector *connector, const struct mikuterm_connect_options *options);
void mikuterm_connections_stop(void);
int mikuterm_connections_wait(unsigned milliseconds);

#endif
