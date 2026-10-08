/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Mail's backend (WS169 p003, plan/ws169/phase001/phase.md section 2): a
 * connection to a mail server, plain or TLS (the OpenSSL package loaded
 * when first needed), IMAP4rev1 to read the folders, SMTP to send, the
 * reading and the writing of a message (RFC 5322 and MIME), and the
 * search for a sign-in code.  Nothing here knows the window or libkeiland,
 * so that the host tests build it alone against their own servers.
 */

#ifndef MAILER_MAIL_H
#define MAILER_MAIL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Marks a parameter a function does not use (a callback's that its kind of reader needs no part of). */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(name) ((void)(name))
#endif

/* The longest name, address, host or line of words kept, with its NUL. */
#define ML_TEXT_MAX		256U

/* The longest list of receivers kept (To, Cc), with its NUL. */
#define ML_LIST_MAX		1024U

/* The most accounts (ws177-p015: 16, from 4). */
#define ML_ACCOUNTS_MAX		16U

/* The longest fingerprint of a certificate the user trusts (SHA-256 in hex), with its NUL. */
#define ML_PIN_MAX		65U

/*
 * The failures of Mail's own beyond the errno values (ws177-p015): the
 * server's certificate does not verify (ml_tls_fingerprint gives it, for
 * the user to trust), and a server on a plain port that does not offer
 * STARTTLS (a password is never sent in the clear).
 */
#define ML_ERROR_UNTRUSTED	0x10001
#define ML_ERROR_NO_TLS		0x10002

/* What an IMAP server can do (its CAPABILITY, ws177-p015). */
#define ML_IMAP_CAN_MOVE	1U	/* MOVE (RFC 6851) */
#define ML_IMAP_CAN_UIDPLUS	2U	/* UID EXPUNGE (RFC 4315) */
#define ML_IMAP_GMAIL		4U	/* Gmail's extensions: it keeps a sent message in Sent by itself */

/* The longest IMAP folder name kept, with its NUL. */
#define ML_MAILBOX_MAX		128U

/* The most bytes a connection buffers from the server. */
#define ML_CONN_BUFFER		16384U

/* The longest line read from a server, with its NUL. */
#define ML_LINE_MAX		8192U

/* The longest sign-in code, with its NUL (keiland.h's KL_MAIL_CODE_MAX). */
#define ML_CODE_MAX		16U

/* How much of a message is fetched (a larger one's words alone are fetched by its structure, ws177-p016). */
#define ML_FETCH_BYTES		1048576U

/* The folders of an account, in the sidebar's order. */
enum ml_folder {
	ML_INBOX,
	ML_SENT,
	ML_DRAFTS,
	ML_ARCHIVE,
	ML_TRASH,
	ML_FOLDERS
};

/* What a message is (bits). */
#define ML_UNREAD		1U	/* not read yet (no \Seen) */
#define ML_ATTACHMENT		2U	/* it carries a file */

/*
 * A mail server: its host, its port, whether the connection is TLS from
 * its start (993, 465) or is upgraded with STARTTLS (143, 587), and the
 * fingerprint of a certificate the user trusts though it does not verify
 * (empty for none; ws177-p015).
 */
struct ml_server {
	char host[ML_TEXT_MAX];
	unsigned port;
	int secure;
	char pin[ML_PIN_MAX];
};

/*
 * An account as the user set it: the name shown, the address mail comes
 * to and is sent from, the user name and password the servers take, and
 * the two servers.
 */
struct ml_account_config {
	char name[64];
	char address[ML_TEXT_MAX];
	char user[ML_TEXT_MAX];
	char password[ML_TEXT_MAX];
	struct ml_server imap;
	struct ml_server smtp;
};

/*
 * One connection to a server: its socket, its TLS state (NULL while
 * plain), the bytes read and not yet taken (from start to end of the
 * buffer), and how long a read waits for the server.
 */
struct ml_conn {
	int fd;
	void *tls;
	unsigned char buffer[ML_CONN_BUFFER];
	size_t start;
	size_t end;
	int timeout_ms;
};

/*
 * A message as read (mime.c): its sender's name and address, to whom and
 * in copy, its subject and ID, its date, its words as UTF-8 text with
 * line feeds (allocated), the name and size of the first file it carries
 * (empty for none), and a sign-in code found in it (empty for none).
 */
struct ml_parsed {
	char from_name[ML_TEXT_MAX];
	char from_address[ML_TEXT_MAX];
	char to[ML_LIST_MAX];
	char cc[ML_LIST_MAX];
	char subject[ML_TEXT_MAX];
	char message_id[ML_TEXT_MAX];
	time_t date;
	char *body;
	char file_name[ML_TEXT_MAX];
	size_t file_size;
	char code[ML_CODE_MAX];
};

/*
 * An IMAP session: the connection, the number of the next command's tag,
 * the tag of an IDLE going on (0 for none), how many messages the folder
 * selected has, what the server can do (ML_IMAP_*), and the server's
 * words of the last failure (or of its goodbye).
 */
struct ml_imap {
	struct ml_conn conn;
	unsigned tag;
	unsigned idle_tag;
	uint32_t exists;
	unsigned capabilities;
	char error[ML_TEXT_MAX];
};

/*
 * One part of a message as its structure names it (structure.c,
 * ws177-p016): its section for BODY[] ("1", "2.1"; empty for none), its
 * type ("text/plain"), its character set and its transfer encoding (small
 * letters).
 */
struct ml_structure_part {
	char section[32];
	char type[64];
	char charset[64];
	char encoding[32];
};

/*
 * A message's structure: its first text/plain and text/html parts, and
 * the name and size (as decoded) of the first file it carries (empty and
 * 0 for none).
 */
struct ml_structure {
	struct ml_structure_part text;
	struct ml_structure_part html;
	char file_name[ML_TEXT_MAX];
	size_t file_size;
};

/* What a FETCH gives for each message: its UID, its ML_* flags, its size, and its bytes (up to ML_FETCH_BYTES). */
typedef void (*ml_imap_fetched_fn)(void *data, uint32_t uid, unsigned flags, size_t size, const char *raw, size_t length);

/* The TLS of the OpenSSL package (tls.c). */
int ml_tls_add_ca_file(const char *path);
int ml_tls_open(int fd, const char *host, const char *pin, void **tls);
int ml_tls_read(void *tls, unsigned char *bytes, size_t length, size_t *received);
int ml_tls_write(void *tls, const unsigned char *bytes, size_t length);
int ml_tls_pending(void *tls);
void ml_tls_close(void *tls);
const char *ml_tls_error(void);
const char *ml_tls_fingerprint(void);

/* A connection (conn.c). */
int ml_server_parse(const char *text, unsigned fallback_port, struct ml_server *server);
int ml_conn_open(struct ml_conn *conn, const struct ml_server *server);
int ml_conn_start_tls(struct ml_conn *conn, const struct ml_server *server);
int ml_conn_write(struct ml_conn *conn, const char *bytes, size_t length);
int ml_conn_line(struct ml_conn *conn, char *line, size_t size);
int ml_conn_bytes(struct ml_conn *conn, char *bytes, size_t count);
int ml_conn_ready(const struct ml_conn *conn);
void ml_conn_close(struct ml_conn *conn);

/* IMAP (imap.c). */
int ml_imap_open(struct ml_imap *imap, const struct ml_account_config *account);
int ml_imap_folders(struct ml_imap *imap, char names[ML_FOLDERS][ML_MAILBOX_MAX]);
int ml_imap_select(struct ml_imap *imap, const char *mailbox, uint32_t *exists);
int ml_imap_fetch(struct ml_imap *imap, uint32_t first_uid, unsigned most, ml_imap_fetched_fn fetched, void *data);
int ml_imap_flag(struct ml_imap *imap, uint32_t uid, const char *flag, int add);
int ml_imap_move(struct ml_imap *imap, uint32_t uid, const char *mailbox);
int ml_imap_delete(struct ml_imap *imap, uint32_t uid);
int ml_imap_append(struct ml_imap *imap, const char *mailbox, const char *raw, size_t length);
int ml_imap_section(struct ml_imap *imap, uint32_t uid, const char *section, char **bytes, size_t *length);
int ml_imap_structure(struct ml_imap *imap, uint32_t uid, struct ml_structure *structure);
int ml_imap_fetch_large(struct ml_imap *imap, uint32_t uid, struct ml_parsed *parsed);
int ml_imap_idle_start(struct ml_imap *imap);
int ml_imap_idle_take(struct ml_imap *imap, int *arrived);
int ml_imap_idle_stop(struct ml_imap *imap);
void ml_imap_close(struct ml_imap *imap);

/* SMTP (smtp.c). */
int ml_smtp_send(const struct ml_account_config *account, const char *const *receivers, size_t count, const char *raw, size_t length, char *error, size_t size);

/* Reading and writing a message (mime.c, compose.c). */
int ml_mime_parse(const char *raw, size_t length, struct ml_parsed *parsed);
int ml_mime_parse_large(const char *header, size_t header_length, const struct ml_structure *structure, const struct ml_structure_part *part, const char *body, size_t body_length, struct ml_parsed *parsed);
int ml_structure_parse(const char *text, struct ml_structure *structure);
void ml_mime_release(struct ml_parsed *parsed);
int ml_mime_address_list(const char *list, char (*addresses)[ML_TEXT_MAX], size_t capacity, size_t *count);
int ml_compose(const struct ml_account_config *account, const char *to, const char *cc, const char *subject, const char *body, const char *reply_to_id, time_t now, char **raw, size_t *length);

/*
 * A file attached to a message being written (ws189-p004): its name (UTF-8),
 * its MIME type, and its bytes.  At most ML_ATTACH_MAX go with a message,
 * ML_ATTACH_TOTAL_MAX bytes in all.
 */
#define ML_ATTACH_MAX		16U
#define ML_ATTACH_TOTAL_MAX	((size_t)25 * 1024 * 1024)
#define ML_ATTACH_TYPE_MAX	64U
struct ml_attachment {
	char name[ML_TEXT_MAX];
	char type[ML_ATTACH_TYPE_MAX];
	unsigned char *data;
	size_t length;
};

int ml_compose_with(const struct ml_account_config *account, const char *to, const char *cc, const char *subject, const char *body, const char *reply_to_id,
    const struct ml_attachment *attachments, size_t count, time_t now, char **raw, size_t *length);

/* The Japanese character sets (jis.c, ws177-p016): which one a charset name is, and its next character. */
#define ML_JIS_NONE		0
#define ML_JIS_ISO2022		1
#define ML_JIS_SHIFT		2
#define ML_JIS_EUC		3
int ml_jis_charset(const char *name);
int ml_jis_next(int charset, const unsigned char *bytes, size_t length, size_t *at, int *state, unsigned long *code_point);

/* The sign-in code (code.c). */
int ml_code_find(const char *subject, const char *body, char *code, size_t size);

/* Base64 (mime.c), for SMTP's AUTH PLAIN too. */
size_t ml_base64_encode(const unsigned char *bytes, size_t length, char *text, size_t size);

#endif
