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
 * on a thread of its own, after the screen of launch.c chose the host
 * (or a WSL distribution, which wsl.c asks wsl.exe for).
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

/* The most other names of a host's key shown, and the longest names field of one kept. */
#define MIKUTERM_KNOWN_OTHERS	8U
#define MIKUTERM_KNOWN_NAMES_MAX	128U

struct kl_address;

/*
 * What config\mikuterm.conf says (conf.c), each value its default when
 * the file does not say it.  The *_given fields say which of the View
 * menu's and the settings' tab's settings the file gave (state.conf wins
 * over them).  problems
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
	unsigned wsl_port;
	int font_size_given;
	int theme_given;
	int ambiguous_given;
	int copy_given;
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
	unsigned wsl_port;
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

/*
 * One line of a known_hosts file that holds a host's key under another
 * name: the file (as shown to the user), the line's number and its names
 * field (cut to fit).
 */
struct mikuterm_known_other {
	char path[MIKUTERM_PATH_MAX];
	unsigned line;
	char names[MIKUTERM_KNOWN_NAMES_MAX];
};

/*
 * What the known_hosts files say of a host's key besides the library's
 * answer, for the user (known-hosts.c): the first line naming the host
 * with another key and its file, the @revoked line of the key and its
 * file (line 0: none), and the lines holding the key under other names
 * (other_count of them, the first MIKUTERM_KNOWN_OTHERS kept).  Zeroed
 * before the first file is looked through.
 */
struct mikuterm_known_scan {
	unsigned changed_line;
	char changed_path[MIKUTERM_PATH_MAX];
	unsigned revoked_line;
	char revoked_path[MIKUTERM_PATH_MAX];
	unsigned other_count;
	struct mikuterm_known_other others[MIKUTERM_KNOWN_OTHERS];
};

/* The folder (folder.c). */
int mikuterm_folder_init(void);
const char *mikuterm_folder(void);
int mikuterm_path(char *path, size_t size, const char *inside);
int mikuterm_ssh_path(char *path, size_t size, const char *written);
int mikuterm_file_read(const char *path, size_t limit, char **text, size_t *size);
int mikuterm_file_append(const char *path, const void *bytes, size_t size);
int mikuterm_file_replace(const char *path, const void *bytes, size_t size);
int mikuterm_file_create(const char *path, const void *bytes, size_t size, int secret);
int mikuterm_file_exists(const char *path);
const char *mikuterm_program(void);

/* The settings files (conf.c). */
void mikuterm_conf_defaults(struct mikuterm_conf *conf);
int mikuterm_conf_read(struct mikuterm_conf *conf, const char *text, size_t size, int state);
int mikuterm_conf_value(const char *keyword, const char *value, const char **why);
int mikuterm_conf_text(const struct mikuterm_conf *conf, const char *keyword, char *value, size_t size);
int mikuterm_conf_set(const char *text, size_t size, const char *keyword, const char *value, char **out, size_t *out_size);
int mikuterm_theme_name(const char *name, unsigned *theme);
const char *mikuterm_theme_text(unsigned theme);

/* The hosts' keys and the paths as the user is told of them (known-hosts.c). */
int mikuterm_host_port_name(const char *host, uint16_t port, char *name, size_t size);
int mikuterm_address_name(const struct kl_address *address, uint16_t port, char *name, size_t size);
void mikuterm_known_hosts_scan(const char *text, size_t size, const char *path, const char *name, const uint8_t *key, struct mikuterm_known_scan *scan);
int mikuterm_path_absolute(const char *path);
void mikuterm_path_shown(const char *path, char *shown, size_t size);

/* The passphrases kept for the Windows session (credential.c). */
int mikuterm_passphrase_recall(const char *path, char *passphrase, size_t size);
int mikuterm_passphrase_keep(const char *path, const char *passphrase);
void mikuterm_passphrase_forget(const char *path);
unsigned mikuterm_passphrases_forget_all(void);

/* The settings' tab's files and settings (page.c), and where a font named in the settings is (main.c). */
struct terminal_page_program;
int mikuterm_page_init(struct mikuterm_conf *conf, struct mikuterm_connector *connector, const struct terminal_page_program **program);
int mikuterm_font(char *path, size_t size, const char *name);

/* The View menu's settings kept in state.conf (settings-file.c). */
void mikuterm_settings_defaults(const struct mikuterm_conf *conf);

/* wsl.exe, for WSL's distributions (wsl.c). */
int mikuterm_wsl_run(const char *arguments, unsigned milliseconds, int (*stop)(void *data), void *data, unsigned char *output, size_t capacity, size_t *size, unsigned long *code);

/* The tabs' connections (connection-ssh.c, ssh-thread.c). */
void mikuterm_connector_init(struct mikuterm_connector *connector, const struct mikuterm_connect_options *options);
void mikuterm_connections_stop(void);
int mikuterm_connections_wait(unsigned milliseconds);

#endif
