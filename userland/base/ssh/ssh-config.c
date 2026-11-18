/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The configuration files of sshd and ssh.
 */

#include <string.h>

#include "ssh-channel.h"
#include "ssh-config.h"
#include "ssh-hostfile.h"

/* The longest line read, in bytes after the trailing blanks are cut; a longer line is an error. */
#define CONFIG_LINE_MAX		8192U

/* The most values (words) one line may hold after its keyword. */
#define CONFIG_TOKENS_MAX	128U

/* The longest keyword text shown in an error message. */
#define CONFIG_KEYWORD_SHOWN	48U

/* The most digits a count or a size may be written with, so that they always fit an unsigned. */
#define CONFIG_DIGITS_MAX	9U

/* The most digits of one term of a time, and the largest time in seconds (the largest int, so a caller may keep it in one). */
#define CONFIG_TIME_DIGITS_MAX	10U
#define CONFIG_TIME_MAX		2147483647U

/* The largest value of the options that count something (ServerAliveCountMax, MaxStartups and the like). */
#define CONFIG_COUNT_MAX	1000000U

/* The path that stands for "no file": a keyword given "none" reads this, which holds no key. */
#define CONFIG_NO_FILE		"/dev/null"

/* The default values the caller expects (ssh-config.h). */
#define CONFIG_DEFAULT_HOST_KEY		"/etc/ssh/ssh_host_ed25519_key"
#define CONFIG_DEFAULT_LISTEN		"0.0.0.0"
#define CONFIG_DEFAULT_AUTHORIZED	".ssh/authorized_keys"
#define CONFIG_DEFAULT_IDENTITY		"~/.ssh/id_ed25519"
#define CONFIG_DEFAULT_USER_KNOWN	"~/.ssh/known_hosts"
#define CONFIG_DEFAULT_GLOBAL_KNOWN	"/etc/ssh/ssh_known_hosts"

/* What the value of a keyword is, for the readers of the keyword tables. */
#define CONFIG_KIND_ACTED	0
#define CONFIG_KIND_MATCH	1
#define CONFIG_KIND_IGNORE_ANY	2
#define CONFIG_KIND_IGNORE_BOOL	3
#define CONFIG_KIND_IGNORE_NO	4
#define CONFIG_KIND_IGNORE_NONE	5

/* Whether a path value may hold the home and user tokens of AuthorizedKeysFile, or no "%" at all. */
#define CONFIG_PATH_PLAIN	0
#define CONFIG_PATH_TOKENS	1

/* Whether a path value reads the word "none" as no file. */
#define CONFIG_PATH_NO_NONE	0
#define CONFIG_PATH_NONE_WORD	1

/* The numbers of sshd_config keywords that are acted on; each is also the bit of the first-value-wins mask. */
enum sshd_keyword_id {
	SSHD_KW_PORT = 0,
	SSHD_KW_LISTEN_ADDRESS = 1,
	SSHD_KW_ADDRESS_FAMILY = 2,
	SSHD_KW_HOST_KEY = 3,
	SSHD_KW_PERMIT_ROOT = 4,
	SSHD_KW_PASSWORD = 5,
	SSHD_KW_PERMIT_EMPTY = 6,
	SSHD_KW_PUBKEY = 7,
	SSHD_KW_AUTHORIZED_KEYS = 8,
	SSHD_KW_MAX_AUTH_TRIES = 9,
	SSHD_KW_LOGIN_GRACE_TIME = 10,
	SSHD_KW_MAX_SESSIONS = 11,
	SSHD_KW_MAX_STARTUPS = 12,
	SSHD_KW_STRICT_MODES = 13,
	SSHD_KW_SUBSYSTEM = 14,
	SSHD_KW_ALIVE_INTERVAL = 15,
	SSHD_KW_ALIVE_COUNT = 16,
	SSHD_KW_NONE = 17
};

/* The numbers of ssh_config keywords that are acted on; each below 15 is also the bit of ssh_config.set_mask. */
enum ssh_keyword_id {
	SSH_KW_USER = 0,
	SSH_KW_HOST_NAME = 1,
	SSH_KW_PORT = 2,
	SSH_KW_IDENTITY = 3,
	SSH_KW_STRICT = 4,
	SSH_KW_USER_KNOWN = 5,
	SSH_KW_GLOBAL_KNOWN = 6,
	SSH_KW_BATCH = 7,
	SSH_KW_CONNECT_TIMEOUT = 8,
	SSH_KW_ALIVE_INTERVAL = 9,
	SSH_KW_ALIVE_COUNT = 10,
	SSH_KW_REQUEST_TTY = 11,
	SSH_KW_PASSWORD = 12,
	SSH_KW_PUBKEY = 13,
	SSH_KW_LOG_LEVEL = 14,
	SSH_KW_HOST = 15,
	SSH_KW_NONE = 16
};

/*
 * One word of a configuration line, pointing into the text it was read
 * from (a word in double quotes is the text between the quotes).
 */
struct config_token {
	const char *text;
	size_t size;
};

/*
 * One configuration line split into its keyword and the words after it.
 * It lives for the handling of that one line and points into the text.
 */
struct config_line {
	const char *keyword;
	size_t keyword_size;
	struct config_token tokens[CONFIG_TOKENS_MAX];
	unsigned token_count;
};

/* One keyword: its name (matched without regard to case), what kind of value it takes and, when acted on, its number. */
struct config_keyword {
	const char *name;
	int kind;
	int id;
};

/* One named value of a keyword that takes one of a few words, and the number it stands for. */
struct config_name {
	const char *name;
	int value;
};

/*
 * The state sshd_config_parse keeps for one call: the configuration being
 * filled and the keywords already given (the first value of a keyword is
 * the one used).
 */
struct sshd_state {
	struct sshd_config *config;
	uint32_t seen;
};

/*
 * The state ssh_config_parse and ssh_config_option keep for one call: the
 * configuration being filled, the host name given, whether the lines now
 * being read apply to it (a Host block that did not match turns this off),
 * and whether the line is a -o option (where Host is not allowed).
 */
struct ssh_state {
	struct ssh_config *config;
	const char *host;
	int active;
	int option;
};

/* The function that handles one split line; it answers 0 or EINVAL and, for EINVAL, says what was wrong in *what. */
typedef int (*config_line_handler)(void *context, const struct config_line *parsed, const char **what);

/*
 * The keywords of sshd_config.
 *
 * Values referenced: sshd_config(5) of OpenSSH 10.5p1, the keyword
 * descriptions.  The acted-on keywords are the ones of ssh-config.h.  A
 * keyword that enables a feature these programs do not have is accepted
 * only with the value that leaves the feature off (kind IGNORE_NO: the
 * value must be "no"; IGNORE_NONE: "none"); UseDNS, PrintMotd and
 * PrintLastLog change only what is logged or printed, so either boolean
 * is accepted; AcceptEnv is accepted with any patterns because the
 * environment passed on is fixed.  Match is refused.
 */
static const struct config_keyword sshd_keywords[] = {
	{ "Port", CONFIG_KIND_ACTED, SSHD_KW_PORT },
	{ "ListenAddress", CONFIG_KIND_ACTED, SSHD_KW_LISTEN_ADDRESS },
	{ "AddressFamily", CONFIG_KIND_ACTED, SSHD_KW_ADDRESS_FAMILY },
	{ "HostKey", CONFIG_KIND_ACTED, SSHD_KW_HOST_KEY },
	{ "PermitRootLogin", CONFIG_KIND_ACTED, SSHD_KW_PERMIT_ROOT },
	{ "PasswordAuthentication", CONFIG_KIND_ACTED, SSHD_KW_PASSWORD },
	{ "PermitEmptyPasswords", CONFIG_KIND_ACTED, SSHD_KW_PERMIT_EMPTY },
	{ "PubkeyAuthentication", CONFIG_KIND_ACTED, SSHD_KW_PUBKEY },
	{ "AuthorizedKeysFile", CONFIG_KIND_ACTED, SSHD_KW_AUTHORIZED_KEYS },
	{ "MaxAuthTries", CONFIG_KIND_ACTED, SSHD_KW_MAX_AUTH_TRIES },
	{ "LoginGraceTime", CONFIG_KIND_ACTED, SSHD_KW_LOGIN_GRACE_TIME },
	{ "MaxSessions", CONFIG_KIND_ACTED, SSHD_KW_MAX_SESSIONS },
	{ "MaxStartups", CONFIG_KIND_ACTED, SSHD_KW_MAX_STARTUPS },
	{ "StrictModes", CONFIG_KIND_ACTED, SSHD_KW_STRICT_MODES },
	{ "Subsystem", CONFIG_KIND_ACTED, SSHD_KW_SUBSYSTEM },
	{ "ClientAliveInterval", CONFIG_KIND_ACTED, SSHD_KW_ALIVE_INTERVAL },
	{ "ClientAliveCountMax", CONFIG_KIND_ACTED, SSHD_KW_ALIVE_COUNT },
	{ "Match", CONFIG_KIND_MATCH, SSHD_KW_NONE },
	{ "UseDNS", CONFIG_KIND_IGNORE_BOOL, SSHD_KW_NONE },
	{ "PrintMotd", CONFIG_KIND_IGNORE_BOOL, SSHD_KW_NONE },
	{ "PrintLastLog", CONFIG_KIND_IGNORE_BOOL, SSHD_KW_NONE },
	{ "UsePAM", CONFIG_KIND_IGNORE_NO, SSHD_KW_NONE },
	{ "X11Forwarding", CONFIG_KIND_IGNORE_NO, SSHD_KW_NONE },
	{ "AllowTcpForwarding", CONFIG_KIND_IGNORE_NO, SSHD_KW_NONE },
	{ "AllowAgentForwarding", CONFIG_KIND_IGNORE_NO, SSHD_KW_NONE },
	{ "GatewayPorts", CONFIG_KIND_IGNORE_NO, SSHD_KW_NONE },
	{ "PermitTunnel", CONFIG_KIND_IGNORE_NO, SSHD_KW_NONE },
	{ "PermitUserEnvironment", CONFIG_KIND_IGNORE_NO, SSHD_KW_NONE },
	{ "Compression", CONFIG_KIND_IGNORE_NO, SSHD_KW_NONE },
	{ "KbdInteractiveAuthentication", CONFIG_KIND_IGNORE_NO, SSHD_KW_NONE },
	{ "ChallengeResponseAuthentication", CONFIG_KIND_IGNORE_NO, SSHD_KW_NONE },
	{ "GSSAPIAuthentication", CONFIG_KIND_IGNORE_NO, SSHD_KW_NONE },
	{ "HostbasedAuthentication", CONFIG_KIND_IGNORE_NO, SSHD_KW_NONE },
	{ "Banner", CONFIG_KIND_IGNORE_NONE, SSHD_KW_NONE },
	{ "AcceptEnv", CONFIG_KIND_IGNORE_ANY, SSHD_KW_NONE }
};

/*
 * The keywords of ssh_config.
 *
 * Values referenced: ssh_config(5) of OpenSSH 10.5p1, the keyword
 * descriptions.  The acted-on keywords are the ones of ssh-config.h.  As
 * in the sshd table, a keyword that enables a feature ssh does not have is
 * accepted only with the value that leaves it off.  The connection
 * sharing keywords (ControlMaster, ControlPath, ControlPersist) are
 * accepted with any value: ssh makes one connection for each command, so
 * a request to share one only means a new connection is made.  Match is
 * refused.
 */
static const struct config_keyword ssh_keywords[] = {
	{ "Host", CONFIG_KIND_ACTED, SSH_KW_HOST },
	{ "Match", CONFIG_KIND_MATCH, SSH_KW_NONE },
	{ "HostName", CONFIG_KIND_ACTED, SSH_KW_HOST_NAME },
	{ "User", CONFIG_KIND_ACTED, SSH_KW_USER },
	{ "Port", CONFIG_KIND_ACTED, SSH_KW_PORT },
	{ "IdentityFile", CONFIG_KIND_ACTED, SSH_KW_IDENTITY },
	{ "StrictHostKeyChecking", CONFIG_KIND_ACTED, SSH_KW_STRICT },
	{ "UserKnownHostsFile", CONFIG_KIND_ACTED, SSH_KW_USER_KNOWN },
	{ "GlobalKnownHostsFile", CONFIG_KIND_ACTED, SSH_KW_GLOBAL_KNOWN },
	{ "BatchMode", CONFIG_KIND_ACTED, SSH_KW_BATCH },
	{ "ConnectTimeout", CONFIG_KIND_ACTED, SSH_KW_CONNECT_TIMEOUT },
	{ "ServerAliveInterval", CONFIG_KIND_ACTED, SSH_KW_ALIVE_INTERVAL },
	{ "ServerAliveCountMax", CONFIG_KIND_ACTED, SSH_KW_ALIVE_COUNT },
	{ "RequestTTY", CONFIG_KIND_ACTED, SSH_KW_REQUEST_TTY },
	{ "PasswordAuthentication", CONFIG_KIND_ACTED, SSH_KW_PASSWORD },
	{ "PubkeyAuthentication", CONFIG_KIND_ACTED, SSH_KW_PUBKEY },
	{ "LogLevel", CONFIG_KIND_ACTED, SSH_KW_LOG_LEVEL },
	{ "VerifyHostKeyDNS", CONFIG_KIND_IGNORE_NO, SSH_KW_NONE },
	{ "HashKnownHosts", CONFIG_KIND_IGNORE_NO, SSH_KW_NONE },
	{ "ForwardAgent", CONFIG_KIND_IGNORE_NO, SSH_KW_NONE },
	{ "ForwardX11", CONFIG_KIND_IGNORE_NO, SSH_KW_NONE },
	{ "ForwardX11Trusted", CONFIG_KIND_IGNORE_NO, SSH_KW_NONE },
	{ "Compression", CONFIG_KIND_IGNORE_NO, SSH_KW_NONE },
	{ "AddKeysToAgent", CONFIG_KIND_IGNORE_NO, SSH_KW_NONE },
	{ "GSSAPIAuthentication", CONFIG_KIND_IGNORE_NO, SSH_KW_NONE },
	{ "KbdInteractiveAuthentication", CONFIG_KIND_IGNORE_NO, SSH_KW_NONE },
	{ "HostbasedAuthentication", CONFIG_KIND_IGNORE_NO, SSH_KW_NONE },
	{ "PermitLocalCommand", CONFIG_KIND_IGNORE_NO, SSH_KW_NONE },
	{ "IdentitiesOnly", CONFIG_KIND_IGNORE_BOOL, SSH_KW_NONE },
	{ "SendEnv", CONFIG_KIND_IGNORE_ANY, SSH_KW_NONE },
	{ "ControlMaster", CONFIG_KIND_IGNORE_ANY, SSH_KW_NONE },
	{ "ControlPath", CONFIG_KIND_IGNORE_ANY, SSH_KW_NONE },
	{ "ControlPersist", CONFIG_KIND_IGNORE_ANY, SSH_KW_NONE }
};

/*
 * The values of PermitRootLogin.
 *
 * Values referenced: sshd_config(5) of OpenSSH 10.5p1, PermitRootLogin:
 * yes, prohibit-password, forced-commands-only, no.  forced-commands-only
 * is left out because it depends on a command restriction in the
 * authorized_keys file that these programs do not read, so a line that
 * asks for it is an error and not silently a different setting.
 */
static const struct config_name permit_root_names[] = {
	{ "yes", SSHD_PERMIT_ROOT_YES },
	{ "no", SSHD_PERMIT_ROOT_NO },
	{ "prohibit-password", SSHD_PERMIT_ROOT_PROHIBIT_PASSWORD }
};

/*
 * The values of AddressFamily, as the value of the inet6 field: any and
 * inet read as 0 (the listener uses IPv4), inet6 as 1.
 *
 * Values referenced: sshd_config(5) of OpenSSH 10.5p1, AddressFamily: any,
 * inet, inet6.
 */
static const struct config_name address_family_names[] = {
	{ "any", 0 },
	{ "inet", 0 },
	{ "inet6", 1 }
};

/*
 * The values of StrictHostKeyChecking.
 *
 * Values referenced: ssh_config(5) of OpenSSH 10.5p1,
 * StrictHostKeyChecking: yes, accept-new, no (and off, its synonym), ask.
 */
static const struct config_name strict_names[] = {
	{ "yes", SSH_STRICT_YES },
	{ "no", SSH_STRICT_NO },
	{ "off", SSH_STRICT_NO },
	{ "ask", SSH_STRICT_ASK },
	{ "accept-new", SSH_STRICT_ACCEPT_NEW }
};

/*
 * The values of RequestTTY.
 *
 * Values referenced: ssh_config(5) of OpenSSH 10.5p1, RequestTTY: no, yes,
 * force, auto.
 */
static const struct config_name request_tty_names[] = {
	{ "auto", SSH_TTY_AUTO },
	{ "no", SSH_TTY_NO },
	{ "yes", SSH_TTY_YES },
	{ "force", SSH_TTY_FORCE }
};

/*
 * The values of LogLevel and the verbosity each one stands for (0 quiet,
 * 1 INFO, 2 VERBOSE, 3 DEBUG).
 *
 * Values referenced: ssh_config(5) of OpenSSH 10.5p1, LogLevel: QUIET,
 * FATAL, ERROR, INFO, VERBOSE, DEBUG, DEBUG1, DEBUG2, DEBUG3; "DEBUG and
 * DEBUG1 are equivalent", DEBUG2 and DEBUG3 are higher.  QUIET, FATAL and
 * ERROR are all read as 0 and DEBUG to DEBUG3 as 3, as ssh-config.h says.
 */
static const struct config_name log_level_names[] = {
	{ "QUIET", 0 },
	{ "FATAL", 0 },
	{ "ERROR", 0 },
	{ "INFO", 1 },
	{ "VERBOSE", 2 },
	{ "DEBUG", 3 },
	{ "DEBUG1", 3 },
	{ "DEBUG2", 3 },
	{ "DEBUG3", 3 }
};

static int config_is_blank(char byte);
static char config_lower(char byte);
static size_t config_skip_blanks(const char *text, size_t size, size_t position);
static size_t config_skip_word(const char *text, size_t size, size_t position);
static size_t config_trim_blanks(const char *text, size_t size);
static int config_text_equal(const char *text, size_t size, const char *expected);
static int config_find_keyword(const struct config_keyword *table, size_t count, const char *name, size_t size);
static int config_lookup_name(const struct config_name *table, size_t count, const struct config_token *token, int *value);
static void config_append(char *error, size_t *used, const char *text, size_t size, int clean);
static void config_error(char *error, unsigned line_number, const char *what, const char *keyword, size_t keyword_size);
static int config_split_line(const char *line, size_t length, struct config_line *parsed, const char **what);
static int config_run(const char *text, size_t size, config_line_handler handler, void *context, char *error);
static int config_one_token(const struct config_line *parsed, const char **what);
static int config_parse_bool(const struct config_token *token, int *value);
static int config_value_bool(const struct config_line *parsed, int *value, const char **what);
static int config_parse_number(const struct config_token *token, unsigned *value);
static int config_value_number(const struct config_line *parsed, unsigned minimum, unsigned maximum, unsigned *value, const char **what);
static int config_parse_time(const struct config_token *token, unsigned *seconds);
static int config_value_time(const struct config_line *parsed, unsigned *seconds, const char **what);
static int config_value_name(const struct config_line *parsed, const struct config_name *table, size_t count, int *value, const char **what);
static int config_copy_path(const struct config_token *token, char *destination, size_t capacity, int tokens, int none_word, const char **what);
static int config_value_path(const struct config_line *parsed, char *destination, size_t capacity, int tokens, int none_word, const char **what);
static int config_value_word(const struct config_line *parsed, char *destination, size_t capacity, size_t maximum, const char **what);
static int config_check_ignored(int kind, const struct config_line *parsed, const char **what);
static void config_set_string(char *destination, size_t capacity, const char *source);
static int config_add_path(char (*list)[SSH_CONFIG_PATH_MAX], unsigned *count, const char *path, const char **what);
static int sshd_value_listen_address(struct sshd_config *target, const struct config_line *parsed, const char **what);
static int sshd_value_max_startups(struct sshd_config *target, const struct config_line *parsed, const char **what);
static int sshd_value_subsystem(struct sshd_config *target, const struct config_line *parsed, const char **what);
static int sshd_value(struct sshd_config *target, int id, const struct config_line *parsed, const char **what);
static int sshd_config_apply(void *context, const struct config_line *parsed, const char **what);
static int ssh_value_identity(struct ssh_config *target, const struct config_line *parsed, const char **what);
static int ssh_value(struct ssh_config *target, int id, const struct config_line *parsed, const char **what);
static int ssh_apply_host(struct ssh_state *state, const struct config_line *parsed, const char **what);
static int ssh_config_apply(void *context, const struct config_line *parsed, const char **what);

/*
 * Fills config with sshd's defaults (host_key_count 0: the default key
 * is added by sshd_config_finish when no HostKey line gave one).
 */
void
sshd_config_defaults(
	struct sshd_config *config)
{
	/* A missing structure has nothing to fill. */
	if (config == NULL)
		return;

	/* Clears every field, so unused host key slots and strings are empty. */
	memset(config, 0, sizeof(*config));

	/* The listening address and port of zedBSD's own sshd_config. */
	config->port = 22;
	config_set_string(config->listen_address, sizeof(config->listen_address), CONFIG_DEFAULT_LISTEN);
	config->inet6 = 0;

	/* OpenSSH's default for root, and both authentication methods on. */
	config->permit_root = SSHD_PERMIT_ROOT_PROHIBIT_PASSWORD;
	config->password = 1;
	config->pubkey = 1;
	config->permit_empty = 0;

	/* The limits of a login: tries, time, sessions and unauthenticated connections. */
	config->max_auth_tries = 6;
	config->login_grace_time = 120;
	config->max_sessions = SSH_CHANNEL_MAX;
	config->max_startups = 10;

	/* Where a user's keys are read from, and the check of the file's permissions. */
	config_set_string(config->authorized_keys_file, sizeof(config->authorized_keys_file), CONFIG_DEFAULT_AUTHORIZED);
	config->strict_modes = 1;

	/* No sftp subsystem until a Subsystem line names one, and no keepalive probes. */
	config->sftp_command[0] = '\0';
	config->client_alive_interval = 0;
	config->client_alive_count = 3;
}

/*
 * Reads size bytes of sshd_config text into config (which holds the
 * defaults or earlier lines).
 */
int
sshd_config_parse(
	struct sshd_config *config,
	const char *text,
	size_t size,
	char *error)
{
	struct sshd_state state;
	int status;

	/* A missing structure cannot be filled. */
	if (config == NULL) {
		config_error(error, 0, "no configuration", NULL, 0);
		return EINVAL;
	}

	/* No keyword has been given yet in this text. */
	state.config = config;
	state.seen = 0;

	/* Reads every line of the text. */
	status = config_run(text, size, sshd_config_apply, &state, error);
	if (status != 0)
		return status;

	/* Succeeded: every line was read. */
	return 0;
}

/*
 * Adds the default host key when none was given.
 */
int
sshd_config_finish(
	struct sshd_config *config)
{
	/* A missing structure has nothing to finish. */
	if (config == NULL)
		return EINVAL;

	/* Adds the default key when no HostKey line gave one. */
	if (config->host_key_count == 0) {
		config_set_string(config->host_key[0], sizeof(config->host_key[0]), CONFIG_DEFAULT_HOST_KEY);
		config->host_key_count = 1;
	}

	/* Succeeded: sshd has at least one host key. */
	return 0;
}

/*
 * Clears config (set_mask 0, every field unset).
 */
void
ssh_config_init(
	struct ssh_config *config)
{
	/* A missing structure has nothing to clear. */
	if (config == NULL)
		return;

	/* Clears every field and the mask of the fields a source has set. */
	memset(config, 0, sizeof(*config));
}

/*
 * Reads size bytes of ssh_config text for destination host (the name the
 * user gave) into config, setting only fields not yet set.
 */
int
ssh_config_parse(
	struct ssh_config *config,
	const char *text,
	size_t size,
	const char *host,
	char *error)
{
	struct ssh_state state;
	int status;

	/* A missing structure or host name cannot be used. */
	if (config == NULL || host == NULL) {
		config_error(error, 0, "no configuration or host", NULL, 0);
		return EINVAL;
	}

	/* Lines before the first Host line apply to every host. */
	state.config = config;
	state.host = host;
	state.active = 1;
	state.option = 0;

	/* Reads every line of the text. */
	status = config_run(text, size, ssh_config_apply, &state, error);
	if (status != 0)
		return status;

	/* Succeeded: every line was read. */
	return 0;
}

/*
 * Applies one -o option ("Keyword=value" or "Keyword value"), as a line
 * outside every Host block.
 */
int
ssh_config_option(
	struct ssh_config *config,
	const char *option,
	char *error)
{
	struct ssh_state state;
	struct config_line parsed;
	const char *what;
	const void *line_feed;
	const void *carriage_return;
	size_t length;
	int status;

	/* A missing structure or option cannot be used. */
	if (config == NULL || option == NULL) {
		config_error(error, 0, "no configuration or option", NULL, 0);
		return EINVAL;
	}

	/* Clears the message of an earlier call. */
	if (error != NULL)
		error[0] = '\0';

	/* An option is one line: it holds no line break and is not longer than a line. */
	length = strlen(option);
	if (length > CONFIG_LINE_MAX) {
		config_error(error, 0, "option too long", NULL, 0);
		return EINVAL;
	}

	/* Refuses an option with a line break in it. */
	line_feed = memchr(option, '\n', length);
	carriage_return = memchr(option, '\r', length);
	if (line_feed != NULL || carriage_return != NULL) {
		config_error(error, 0, "line break in option", NULL, 0);
		return EINVAL;
	}

	/* Splits the option into its keyword and values. */
	what = "bad option";
	status = config_split_line(option, length, &parsed, &what);
	if (status != 0) {
		config_error(error, 0, what, parsed.keyword, parsed.keyword_size);
		return EINVAL;
	}

	/* An option is applied as a line outside every Host block, and cannot start one. */
	state.config = config;
	state.host = "";
	state.active = 1;
	state.option = 1;

	/* Applies the option like a line of a file. */
	status = ssh_config_apply(&state, &parsed, &what);
	if (status != 0) {
		config_error(error, 0, what, parsed.keyword, parsed.keyword_size);
		return EINVAL;
	}

	/* Succeeded: the option is applied. */
	return 0;
}

/*
 * Fills every field not set with its default.
 */
void
ssh_config_defaults(
	struct ssh_config *config)
{
	/* A missing structure has nothing to fill. */
	if (config == NULL)
		return;

	/* The user is left empty for the caller to fill with the login name, and the host name with the name given. */
	if ((config->set_mask & ((uint32_t)1U << SSH_KW_USER)) == 0)
		config->user[0] = '\0';
	if ((config->set_mask & ((uint32_t)1U << SSH_KW_HOST_NAME)) == 0)
		config->host_name[0] = '\0';

	/* The standard port. */
	if ((config->set_mask & ((uint32_t)1U << SSH_KW_PORT)) == 0)
		config->port = 22;

	/* The one default identity file, only when no IdentityFile line gave one. */
	if (config->identity_count == 0) {
		config_set_string(config->identity[0], sizeof(config->identity[0]), CONFIG_DEFAULT_IDENTITY);
		config->identity_count = 1;
	}

	/* Asks before trusting a new host key. */
	if ((config->set_mask & ((uint32_t)1U << SSH_KW_STRICT)) == 0)
		config->strict = SSH_STRICT_ASK;

	/* The user's and the system's known_hosts files. */
	if ((config->set_mask & ((uint32_t)1U << SSH_KW_USER_KNOWN)) == 0)
		config_set_string(config->user_known_hosts, sizeof(config->user_known_hosts), CONFIG_DEFAULT_USER_KNOWN);
	if ((config->set_mask & ((uint32_t)1U << SSH_KW_GLOBAL_KNOWN)) == 0)
		config_set_string(config->global_known_hosts, sizeof(config->global_known_hosts), CONFIG_DEFAULT_GLOBAL_KNOWN);

	/* Questions are allowed, no connection timeout, no keepalive probes (and three misses end the connection). */
	if ((config->set_mask & ((uint32_t)1U << SSH_KW_BATCH)) == 0)
		config->batch_mode = 0;
	if ((config->set_mask & ((uint32_t)1U << SSH_KW_CONNECT_TIMEOUT)) == 0)
		config->connect_timeout = 0;
	if ((config->set_mask & ((uint32_t)1U << SSH_KW_ALIVE_INTERVAL)) == 0)
		config->server_alive_interval = 0;
	if ((config->set_mask & ((uint32_t)1U << SSH_KW_ALIVE_COUNT)) == 0)
		config->server_alive_count = 3;

	/* A terminal is requested for a login session only. */
	if ((config->set_mask & ((uint32_t)1U << SSH_KW_REQUEST_TTY)) == 0)
		config->request_tty = SSH_TTY_AUTO;

	/* Both authentication methods are tried. */
	if ((config->set_mask & ((uint32_t)1U << SSH_KW_PASSWORD)) == 0)
		config->password = 1;
	if ((config->set_mask & ((uint32_t)1U << SSH_KW_PUBKEY)) == 0)
		config->pubkey = 1;

	/* The INFO level. */
	if ((config->set_mask & ((uint32_t)1U << SSH_KW_LOG_LEVEL)) == 0)
		config->log_level = 1;
}

/* Reports whether a byte separates the words of a line (a space, a tab or another white-space character). */
static int
config_is_blank(
	char byte)
{
	/* The characters C calls white space, other than the line feed that ends a line. */
	if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\v' || byte == '\f')
		return 1;

	/* Any other byte belongs to a word. */
	return 0;
}

/* Turns an ASCII capital into its small letter and leaves every other byte alone. */
static char
config_lower(
	char byte)
{
	/* Folds the 26 ASCII capitals only. */
	if (byte >= 'A' && byte <= 'Z')
		return (char)(byte - 'A' + 'a');

	/* Succeeded: not a capital. */
	return byte;
}

/* Returns the position of the first byte at or after position that is not a blank, or size. */
static size_t
config_skip_blanks(
	const char *text,
	size_t size,
	size_t position)
{
	int blank;

	/* Stops at the first byte that belongs to a word. */
	while (position < size) {
		blank = config_is_blank(text[position]);
		if (!blank)
			break;
		position++;
	}

	/* Succeeded: the blanks are skipped. */
	return position;
}

/* Returns the position of the first blank at or after position, or size. */
static size_t
config_skip_word(
	const char *text,
	size_t size,
	size_t position)
{
	int blank;

	/* Stops at the first blank. */
	while (position < size) {
		blank = config_is_blank(text[position]);
		if (blank)
			break;
		position++;
	}

	/* Succeeded: the word is skipped. */
	return position;
}

/* Returns the size of the text without the blanks at its end. */
static size_t
config_trim_blanks(
	const char *text,
	size_t size)
{
	int blank;

	/* Stops at the last byte that belongs to a word. */
	while (size > 0) {
		blank = config_is_blank(text[size - 1U]);
		if (!blank)
			break;
		size--;
	}

	/* Succeeded: the trailing blanks are cut. */
	return size;
}

/* Compares size bytes at text with expected without regard to the case of ASCII letters; 1 when they are equal. */
static int
config_text_equal(
	const char *text,
	size_t size,
	const char *expected)
{
	size_t index;
	size_t expected_size;
	char folded_text;
	char folded_expected;

	/* A length that differs is a difference. */
	expected_size = strlen(expected);
	if (expected_size != size)
		return 0;

	/* Compares the bytes one by one, folding capitals. */
	for (index = 0; index < size; index++) {
		folded_text = config_lower(text[index]);
		folded_expected = config_lower(expected[index]);
		if (folded_text != folded_expected)
			return 0;
	}

	/* Succeeded: equal. */
	return 1;
}

/* Finds a keyword in a table without regard to case; returns its index or -1. */
static int
config_find_keyword(
	const struct config_keyword *table,
	size_t count,
	const char *name,
	size_t size)
{
	size_t index;
	int equal;

	/* Looks through the table in order. */
	for (index = 0; index < count; index++) {
		equal = config_text_equal(name, size, table[index].name);
		if (equal)
			return (int)index;
	}

	/* Reports that the keyword is not in the table. */
	return -1;
}

/* Looks up a word in a table of named values without regard to case; 0 with *value set, or EINVAL. */
static int
config_lookup_name(
	const struct config_name *table,
	size_t count,
	const struct config_token *token,
	int *value)
{
	size_t index;
	int equal;

	/* Looks through the table in order. */
	for (index = 0; index < count; index++) {
		equal = config_text_equal(token->text, token->size, table[index].name);
		if (equal) {
			*value = table[index].value;
			return 0;
		}
	}

	/* Reports that the word is none of the names. */
	return EINVAL;
}

/*
 * Appends size bytes of text to the message at error (used counts the
 * bytes already there, and stays below SSH_CONFIG_ERROR_MAX - 1).  With
 * clean set, a byte that is not a printable character is written as '?'.
 */
static void
config_append(
	char *error,
	size_t *used,
	const char *text,
	size_t size,
	int clean)
{
	size_t index;
	char byte;

	/* Copies as many bytes as fit, leaving room for the NUL. */
	for (index = 0; index < size; index++) {
		if (*used + 1 >= SSH_CONFIG_ERROR_MAX)
			break;
		byte = text[index];
		if (clean && ((unsigned char)byte < 0x21 || (unsigned char)byte > 0x7e))
			byte = '?';
		error[*used] = byte;
		(*used)++;
	}
}

/*
 * Writes the message "line N: what: keyword" at error (line_number 0 and
 * a zero keyword_size leave those parts out).  error may be NULL.
 */
static void
config_error(
	char *error,
	unsigned line_number,
	const char *what,
	const char *keyword,
	size_t keyword_size)
{
	char digits[16];
	size_t used;
	size_t count;
	unsigned remaining;

	/* No message is wanted. */
	if (error == NULL)
		return;

	/* Starts with an empty message. */
	used = 0;
	error[0] = '\0';

	/* Writes "line N: " with N in decimal. */
	if (line_number != 0) {
		count = 0;
		remaining = line_number;
		while (remaining != 0) {
			digits[count] = (char)('0' + remaining % 10);
			count++;
			remaining = remaining / 10;
		}

		/* Writes the word and the line number. */
		config_append(error, &used, "line ", 5, 0);
		while (count != 0) {
			count--;
			config_append(error, &used, &digits[count], 1, 0);
		}

		/* Ends the position prefix. */
		config_append(error, &used, ": ", 2, 0);
	}

	/* Writes what was wrong. */
	config_append(error, &used, what, strlen(what), 0);

	/* Names the keyword, cut to a length that fits and with unprintable bytes replaced. */
	if (keyword != NULL && keyword_size != 0) {
		if (keyword_size > CONFIG_KEYWORD_SHOWN)
			keyword_size = CONFIG_KEYWORD_SHOWN;
		config_append(error, &used, ": ", 2, 0);
		config_append(error, &used, keyword, keyword_size, 1);
	}

	/* Ends the message. */
	error[used] = '\0';
}

/*
 * Splits one line (already without its trailing blanks and without
 * a NUL) into its keyword and words.
 *
 * The keyword ends at a blank or at '='; one '=' with blanks around it is
 * accepted in place of the blanks (ssh_config(5)).  A word is a run of
 * non-blank bytes, or text between double quotes, which may hold blanks
 * and ends at the next double quote; a '#' that starts a word starts a
 * comment to the end of the line (a '#' elsewhere belongs to the word).
 * Returns 0 or EINVAL with *what set.
 */
static int
config_split_line(
	const char *line,
	size_t length,
	struct config_line *parsed,
	const char **what)
{
	size_t position;
	size_t start;
	int blank;

	/* Starts with no keyword and no word. */
	parsed->keyword = line;
	parsed->keyword_size = 0;
	parsed->token_count = 0;
	position = 0;

	/* Skips the blanks before the keyword. */
	position = config_skip_blanks(line, length, position);

	/* Reads the keyword: up to a blank or an '='. */
	start = position;
	while (position < length) {
		blank = config_is_blank(line[position]);
		if (blank || line[position] == '=')
			break;
		position++;
	}

	/* Records the keyword. */
	parsed->keyword = line + start;
	parsed->keyword_size = position - start;

	/* A line that starts with '=' has no keyword. */
	if (parsed->keyword_size == 0) {
		*what = "missing keyword";
		return EINVAL;
	}

	/* Skips the separator: blanks, at most one '=', blanks. */
	position = config_skip_blanks(line, length, position);
	if (position < length && line[position] == '=')
		position++;
	position = config_skip_blanks(line, length, position);

	/* Reads the words up to the end of the line or a comment. */
	while (position < length && line[position] != '#') {
		/* The words are limited so that a line cannot make the array overflow. */
		if (parsed->token_count >= CONFIG_TOKENS_MAX) {
			*what = "too many values";
			return EINVAL;
		}

		/* Reads one word, quoted or bare. */
		if (line[position] == '"') {
			/* A quoted word runs to the closing quote and must be followed by a blank or the end of the line. */
			position++;
			start = position;
			while (position < length && line[position] != '"')
				position++;
			if (position >= length) {
				*what = "unterminated quote";
				return EINVAL;
			}

			/* Records the quoted word. */
			parsed->tokens[parsed->token_count].text = line + start;
			parsed->tokens[parsed->token_count].size = position - start;
			position++;
			if (position < length) {
				blank = config_is_blank(line[position]);
				if (!blank) {
					*what = "text after a closing quote";
					return EINVAL;
				}
			}
		} else {
			/* A plain word runs to the next blank. */
			start = position;
			position = config_skip_word(line, length, position);
			parsed->tokens[parsed->token_count].text = line + start;
			parsed->tokens[parsed->token_count].size = position - start;
		}

		/* Counts the word. */
		parsed->token_count++;

		/* Skips the blanks before the next word. */
		position = config_skip_blanks(line, length, position);
	}

	/* Succeeded: the line is split. */
	return 0;
}

/*
 * Reads the text line by line and gives each line that holds a keyword to
 * handler.  Lines end at LF (a CR before it is a blank); blank lines and
 * comment lines are skipped; a line with a NUL or a line longer than
 * CONFIG_LINE_MAX is an error.  Returns 0, EINVAL with the message
 * "line N: ..." or EMSGSIZE.
 */
static int
config_run(
	const char *text,
	size_t size,
	config_line_handler handler,
	void *context,
	char *error)
{
	struct config_line parsed;
	const char *what;
	size_t offset;
	size_t start;
	size_t end;
	size_t length;
	size_t blanks;
	const void *nul;
	unsigned line_number;
	int status;

	/* Clears the message of an earlier call. */
	if (error != NULL)
		error[0] = '\0';

	/* A text larger than any file read is refused before it is looked at. */
	if (size > SSH_CONFIG_FILE_MAX) {
		config_error(error, 0, "file too large", NULL, 0);
		return EMSGSIZE;
	}

	/* Text of a size with no bytes behind it is a caller's error. */
	if (text == NULL && size != 0) {
		config_error(error, 0, "no text", NULL, 0);
		return EINVAL;
	}

	/* Handles each line in turn. */
	offset = 0;
	line_number = 0;
	while (offset < size) {
		/* Finds the end of this line. */
		start = offset;
		end = start;
		while (end < size && text[end] != '\n')
			end++;
		offset = end;
		if (offset < size)
			offset++;
		line_number++;

		/* A NUL byte in a line is an error whether or not the line is a comment. */
		nul = memchr(text + start, '\0', end - start);
		if (nul != NULL) {
			config_error(error, line_number, "NUL byte in line", NULL, 0);
			return EINVAL;
		}

		/* Cuts the trailing blanks (and the CR of a CR LF). */
		length = config_trim_blanks(text + start, end - start);

		/* Skips the leading blanks; an empty line and a comment line are ignored. */
		blanks = config_skip_blanks(text + start, length, 0);
		start += blanks;
		length -= blanks;

		/* Skips an empty line and a comment line. */
		if (length == 0 || text[start] == '#')
			continue;

		/* A line longer than the limit is an error. */
		if (length > CONFIG_LINE_MAX) {
			config_error(error, line_number, "line too long", NULL, 0);
			return EINVAL;
		}

		/* Splits the line into its keyword and words. */
		what = "bad line";
		status = config_split_line(text + start, length, &parsed, &what);
		if (status != 0) {
			config_error(error, line_number, what, parsed.keyword, parsed.keyword_size);
			return EINVAL;
		}

		/* Gives the line to the handler of this kind of file. */
		status = handler(context, &parsed, &what);
		if (status != 0) {
			config_error(error, line_number, what, parsed.keyword, parsed.keyword_size);
			return EINVAL;
		}
	}

	/* Succeeded: every line was handled. */
	return 0;
}

/* Checks that the line has exactly one word after its keyword; 0, or EINVAL with *what set. */
static int
config_one_token(
	const struct config_line *parsed,
	const char **what)
{
	/* A keyword with no value is an error. */
	if (parsed->token_count == 0) {
		*what = "missing value";
		return EINVAL;
	}

	/* More than one value is an error for a keyword that takes one. */
	if (parsed->token_count > 1) {
		*what = "too many values";
		return EINVAL;
	}

	/* Succeeded: one word. */
	return 0;
}

/* Reads "yes" or "no" (without regard to case) into *value as 1 or 0; 0 or EINVAL. */
static int
config_parse_bool(
	const struct config_token *token,
	int *value)
{
	int equal;

	/* "yes" turns the feature on. */
	equal = config_text_equal(token->text, token->size, "yes");
	if (equal) {
		*value = 1;
		return 0;
	}

	/* "no" turns it off. */
	equal = config_text_equal(token->text, token->size, "no");
	if (equal) {
		*value = 0;
		return 0;
	}

	/* Reports a word that is neither. */
	return EINVAL;
}

/* Reads the one yes/no value of a line into *value; 0 or EINVAL with *what set. */
static int
config_value_bool(
	const struct config_line *parsed,
	int *value,
	const char **what)
{
	int status;

	/* Needs exactly one word. */
	status = config_one_token(parsed, what);
	if (status != 0)
		return status;

	/* Reads the word. */
	status = config_parse_bool(&parsed->tokens[0], value);
	if (status != 0) {
		*what = "value must be yes or no";
		return EINVAL;
	}

	/* Succeeded: *value is 1 or 0. */
	return 0;
}

/* Reads a decimal number of at most CONFIG_DIGITS_MAX digits (no sign, no space) into *value; 0 or EINVAL. */
static int
config_parse_number(
	const struct config_token *token,
	unsigned *value)
{
	size_t index;
	unsigned number;

	/* An empty word, or one with too many digits for the unsigned, is not a number. */
	if (token->size == 0 || token->size > CONFIG_DIGITS_MAX)
		return EINVAL;

	/* Reads the digits; any other byte makes it not a number. */
	number = 0;
	for (index = 0; index < token->size; index++) {
		if (token->text[index] < '0' || token->text[index] > '9')
			return EINVAL;
		number = number * 10U + (unsigned)(token->text[index] - '0');
	}

	/* Succeeded: the number. */
	*value = number;
	return 0;
}

/* Reads the one number of a line, which must be from minimum to maximum, into *value; 0 or EINVAL with *what set. */
static int
config_value_number(
	const struct config_line *parsed,
	unsigned minimum,
	unsigned maximum,
	unsigned *value,
	const char **what)
{
	unsigned number;
	int status;

	/* Needs exactly one word. */
	status = config_one_token(parsed, what);
	if (status != 0)
		return status;

	/* Reads the digits. */
	status = config_parse_number(&parsed->tokens[0], &number);
	if (status != 0) {
		*what = "value must be a number";
		return EINVAL;
	}

	/* Checks the range. */
	if (number < minimum || number > maximum) {
		*what = "number out of range";
		return EINVAL;
	}

	/* Succeeded: the number is in range. */
	*value = number;
	return 0;
}

/*
 * Reads a time (sshd_config(5) TIME FORMATS: one or more terms, each a
 * number and an optional qualifier s, m, h, d or w, a term without a
 * qualifier being seconds and necessarily the last) into *seconds; 0 or
 * EINVAL when the text is not a time or the total is above
 * CONFIG_TIME_MAX.
 *
 * Values referenced: sshd_config(5) of OpenSSH 10.5p1, TIME FORMATS:
 * s or S seconds, m or M minutes (60), h or H hours (3600), d or D days
 * (86400), w or W weeks (604800).
 */
static int
config_parse_time(
	const struct config_token *token,
	unsigned *seconds)
{
	uint64_t total;
	uint64_t term;
	uint64_t unit;
	size_t position;
	size_t digits;

	/* An empty word is not a time. */
	if (token->size == 0)
		return EINVAL;

	/* Reads the terms one after the other. */
	total = 0;
	position = 0;
	while (position < token->size) {
		/* A term starts with up to ten digits (at least one). */
		term = 0;
		digits = 0;
		while (position < token->size && token->text[position] >= '0' && token->text[position] <= '9') {
			if (digits >= CONFIG_TIME_DIGITS_MAX)
				return EINVAL;
			term = term * 10U + (uint64_t)(token->text[position] - '0');
			digits++;
			position++;
		}

		/* Refuses a number without digits. */
		if (digits == 0)
			return EINVAL;

		/* A qualifier after the digits gives the unit; a term with none is seconds and ends the text. */
		unit = 1;
		if (position < token->size) {
			switch (token->text[position]) {
			case 's':
			case 'S':
				unit = 1;
				break;
			case 'm':
			case 'M':
				unit = 60;
				break;
			case 'h':
			case 'H':
				unit = 3600;
				break;
			case 'd':
			case 'D':
				unit = 86400;
				break;
			case 'w':
			case 'W':
				unit = 604800;
				break;
			default:
				return EINVAL;
			}

			/* Moves on to the next character of the time. */
			position++;
		}

		/* Adds the term; the sum stays far below the range of uint64_t, and the total is checked against the limit. */
		total += term * unit;
		if (total > CONFIG_TIME_MAX)
			return EINVAL;
	}

	/* Succeeded: the total number of seconds. */
	*seconds = (unsigned)total;
	return 0;
}

/* Reads the one time of a line into *seconds; 0 or EINVAL with *what set. */
static int
config_value_time(
	const struct config_line *parsed,
	unsigned *seconds,
	const char **what)
{
	int status;

	/* Needs exactly one word. */
	status = config_one_token(parsed, what);
	if (status != 0)
		return status;

	/* Reads the time. */
	status = config_parse_time(&parsed->tokens[0], seconds);
	if (status != 0) {
		*what = "bad time";
		return EINVAL;
	}

	/* Succeeded: the time in seconds. */
	return 0;
}

/* Reads the one word of a line that must be one of the table's names into *value; 0 or EINVAL with *what set. */
static int
config_value_name(
	const struct config_line *parsed,
	const struct config_name *table,
	size_t count,
	int *value,
	const char **what)
{
	int status;

	/* Needs exactly one word. */
	status = config_one_token(parsed, what);
	if (status != 0)
		return status;

	/* Looks the word up. */
	status = config_lookup_name(table, count, &parsed->tokens[0], value);
	if (status != 0) {
		*what = "unknown value";
		return EINVAL;
	}

	/* Succeeded: *value is the number of the name. */
	return 0;
}

/*
 * Copies a path word into destination (capacity bytes): 1 to capacity - 1
 * bytes, no control character; a '%' is an error unless tokens is
 * CONFIG_PATH_TOKENS, where %h, %u and %% are accepted (the caller
 * expands them).  none_word CONFIG_PATH_NONE_WORD reads the word "none"
 * as CONFIG_NO_FILE.  0 or EINVAL with *what set.
 */
static int
config_copy_path(
	const struct config_token *token,
	char *destination,
	size_t capacity,
	int tokens,
	int none_word,
	const char **what)
{
	size_t index;
	char next;
	int different;

	/* The word "none" stands for no file at all. */
	different = 1;
	if (token->size == 4)
		different = memcmp(token->text, "none", 4);
	if (none_word == CONFIG_PATH_NONE_WORD && different == 0) {
		config_set_string(destination, capacity, CONFIG_NO_FILE);
		return 0;
	}

	/* A path is not empty and fits with its NUL. */
	if (token->size == 0) {
		*what = "empty path";
		return EINVAL;
	}

	/* Refuses a path that does not fit the buffer. */
	if (token->size >= capacity) {
		*what = "path too long";
		return EINVAL;
	}

	/* Checks each byte: no control character, and '%' only as a token the caller expands. */
	for (index = 0; index < token->size; index++) {
		if ((unsigned char)token->text[index] < 0x20 || (unsigned char)token->text[index] == 0x7f) {
			*what = "control character in path";
			return EINVAL;
		}

		/* Looks at the expansion tokens of the path. */
		if (token->text[index] != '%')
			continue;
		if (tokens != CONFIG_PATH_TOKENS) {
			*what = "tokens are not supported in a path";
			return EINVAL;
		}

		/* Refuses a token that is cut at the end. */
		if (index + 1 >= token->size) {
			*what = "bad token in path";
			return EINVAL;
		}

		/* Checks the token letter. */
		next = token->text[index + 1];
		if (next != 'h' && next != 'u' && next != '%') {
			*what = "bad token in path";
			return EINVAL;
		}

		/* Skips the letter of the token. */
		index++;
	}

	/* Copies the path and ends it. */
	memcpy(destination, token->text, token->size);
	destination[token->size] = '\0';

	/* Succeeded: the path is copied. */
	return 0;
}

/* Reads the one path word of a line into destination; 0 or EINVAL with *what set. */
static int
config_value_path(
	const struct config_line *parsed,
	char *destination,
	size_t capacity,
	int tokens,
	int none_word,
	const char **what)
{
	int status;

	/* Needs exactly one word. */
	status = config_one_token(parsed, what);
	if (status != 0)
		return status;

	/* Checks and copies the path. */
	status = config_copy_path(&parsed->tokens[0], destination, capacity, tokens, none_word, what);
	if (status != 0)
		return status;

	/* Succeeded: the path is copied. */
	return 0;
}

/*
 * Reads the one word of a line that names a user or a host into
 * destination (capacity bytes): 1 to maximum bytes of printable
 * characters (0x21 to 0x7e) other than '%'.  0 or EINVAL with *what set.
 */
static int
config_value_word(
	const struct config_line *parsed,
	char *destination,
	size_t capacity,
	size_t maximum,
	const char **what)
{
	const struct config_token *token;
	size_t index;
	int status;

	/* Needs exactly one word. */
	status = config_one_token(parsed, what);
	if (status != 0)
		return status;

	/* The word is not empty and not longer than the field holds. */
	token = &parsed->tokens[0];
	if (token->size == 0 || token->size > maximum || token->size >= capacity) {
		*what = "value has a bad length";
		return EINVAL;
	}

	/* Checks each byte: printable, no blank, no '%' (tokens are not expanded). */
	for (index = 0; index < token->size; index++) {
		if ((unsigned char)token->text[index] < 0x21 || (unsigned char)token->text[index] > 0x7e || token->text[index] == '%') {
			*what = "bad character in value";
			return EINVAL;
		}
	}

	/* Copies the word and ends it. */
	memcpy(destination, token->text, token->size);
	destination[token->size] = '\0';

	/* Succeeded: the word is copied. */
	return 0;
}

/*
 * Accepts the line of a keyword that is read and ignored: its kind says
 * what the value must be.  0 or EINVAL with *what set.
 */
static int
config_check_ignored(
	int kind,
	const struct config_line *parsed,
	const char **what)
{
	int unused;
	int equal;
	int status;

	/* A keyword with no value is an error whatever it is. */
	if (parsed->token_count == 0) {
		*what = "missing value";
		return EINVAL;
	}

	/* A keyword whose value is ignored takes any words. */
	if (kind == CONFIG_KIND_IGNORE_ANY)
		return 0;

	/* The remaining kinds take exactly one word. */
	status = config_one_token(parsed, what);
	if (status != 0)
		return status;

	/* A yes/no keyword takes either. */
	if (kind == CONFIG_KIND_IGNORE_BOOL) {
		status = config_parse_bool(&parsed->tokens[0], &unused);
		if (status != 0) {
			*what = "value must be yes or no";
			return EINVAL;
		}

		/* Reports success. */
		return 0;
	}

	/* A keyword that enables a feature not provided takes the value that leaves it off. */
	if (kind == CONFIG_KIND_IGNORE_NO) {
		equal = config_text_equal(parsed->tokens[0].text, parsed->tokens[0].size, "no");
		if (!equal) {
			*what = "feature not supported: value must be no";
			return EINVAL;
		}

		/* Reports success. */
		return 0;
	}

	/* The last kind takes "none". */
	equal = config_text_equal(parsed->tokens[0].text, parsed->tokens[0].size, "none");
	if (!equal) {
		*what = "feature not supported: value must be none";
		return EINVAL;
	}

	/* Succeeded: the value leaves the feature off. */
	return 0;
}

/* Copies the NUL-terminated source into destination (capacity bytes); the caller has checked that it fits. */
static void
config_set_string(
	char *destination,
	size_t capacity,
	const char *source)
{
	size_t length;

	/* Keeps the copy inside the field even if the caller was wrong. */
	length = strlen(source);
	if (length >= capacity)
		length = capacity - 1;

	/* Copies the bytes and ends the string. */
	memcpy(destination, source, length);
	destination[length] = '\0';
}

/* Adds path to a list of at most SSH_CONFIG_KEYS_MAX paths, unless it is there already; 0 or EINVAL with *what set. */
static int
config_add_path(
	char (*list)[SSH_CONFIG_PATH_MAX],
	unsigned *count,
	const char *path,
	const char **what)
{
	unsigned index;
	int same;

	/* A path already in the list is not added again. */
	for (index = 0; index < *count; index++) {
		same = strcmp(list[index], path);
		if (same == 0)
			return 0;
	}

	/* A full list takes no more. */
	if (*count >= SSH_CONFIG_KEYS_MAX) {
		*what = "too many entries";
		return EINVAL;
	}

	/* Appends the path. */
	config_set_string(list[*count], SSH_CONFIG_PATH_MAX, path);
	(*count)++;

	/* Succeeded: the path is in the list. */
	return 0;
}

/*
 * Reads ListenAddress: one host name or address (no port, no brackets, no
 * rdomain) of at most 63 characters from letters, digits and ".-_:"; a
 * word with exactly one colon (host:port) is refused, a word with more is
 * an IPv6 address.
 */
static int
sshd_value_listen_address(
	struct sshd_config *target,
	const struct config_line *parsed,
	const char **what)
{
	const struct config_token *token;
	size_t index;
	unsigned colons;
	char byte;
	int status;

	/* Needs exactly one word (an rdomain qualifier would be a second). */
	status = config_one_token(parsed, what);
	if (status != 0)
		return status;

	/* The word is not empty and fits the field with its NUL. */
	token = &parsed->tokens[0];
	if (token->size == 0 || token->size >= sizeof(target->listen_address)) {
		*what = "value has a bad length";
		return EINVAL;
	}

	/* Checks each byte and counts the colons. */
	colons = 0;
	for (index = 0; index < token->size; index++) {
		byte = token->text[index];
		if (byte == ':') {
			colons++;
			continue;
		}

		/* Accepts the characters of a host name. */
		if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
		    byte == '.' || byte == '-' || byte == '_')
			continue;
		*what = "bad character in address";
		return EINVAL;
	}

	/* One colon would be an address with a port, which the single Port setting cannot express. */
	if (colons == 1) {
		*what = "a port in the address is not supported";
		return EINVAL;
	}

	/* Copies the address and ends it. */
	memcpy(target->listen_address, token->text, token->size);
	target->listen_address[token->size] = '\0';

	/* Succeeded: the address is copied. */
	return 0;
}

/*
 * Reads MaxStartups: a number from 1 to CONFIG_COUNT_MAX, or
 * "start:rate:full" (start at least 1, rate from 0 to 100, full at least
 * start); the first number is the one kept.
 *
 * Values referenced: sshd_config(5) of OpenSSH 10.5p1, MaxStartups: a
 * number, or three colon-separated values start:rate:full, where rate is
 * a percentage and the connections are all refused at full.
 */
static int
sshd_value_max_startups(
	struct sshd_config *target,
	const struct config_line *parsed,
	const char **what)
{
	struct config_token part;
	const struct config_token *token;
	unsigned numbers[3];
	size_t start;
	size_t index;
	unsigned count;
	int status;

	/* Needs exactly one word. */
	status = config_one_token(parsed, what);
	if (status != 0)
		return status;

	/* Splits the word at its colons into at most three numbers. */
	token = &parsed->tokens[0];
	count = 0;
	start = 0;
	for (index = 0; index <= token->size; index++) {
		if (index < token->size && token->text[index] != ':')
			continue;
		if (count >= 3) {
			*what = "bad MaxStartups";
			return EINVAL;
		}

		/* Takes the number between the colons. */
		part.text = token->text + start;
		part.size = index - start;
		status = config_parse_number(&part, &numbers[count]);
		if (status != 0) {
			*what = "bad MaxStartups";
			return EINVAL;
		}

		/* Counts the number and moves to the next one. */
		count++;
		start = index + 1;
	}

	/* One number, or all three, nothing in between. */
	if (count != 1 && count != 3) {
		*what = "bad MaxStartups";
		return EINVAL;
	}

	/* The first number is the limit of unauthenticated connections. */
	if (numbers[0] < 1 || numbers[0] > CONFIG_COUNT_MAX) {
		*what = "number out of range";
		return EINVAL;
	}

	/* The three-number form needs a rate that is a percentage and a full that is not below start. */
	if (count == 3) {
		if (numbers[1] > 100 || numbers[2] < numbers[0] || numbers[2] > CONFIG_COUNT_MAX) {
			*what = "number out of range";
			return EINVAL;
		}
	}

	/* Keeps the first number. */
	target->max_startups = numbers[0];

	/* Succeeded: the limit is set. */
	return 0;
}

/*
 * Reads Subsystem: "sftp <command>" (the command a single word); another
 * subsystem name is an error.
 */
static int
sshd_value_subsystem(
	struct sshd_config *target,
	const struct config_line *parsed,
	const char **what)
{
	int different;
	int status;

	/* Needs the name and the command. */
	if (parsed->token_count < 2) {
		*what = "missing value";
		return EINVAL;
	}

	/* Refuses extra arguments. */
	if (parsed->token_count > 2) {
		*what = "arguments to the command are not supported";
		return EINVAL;
	}

	/* Only the sftp subsystem is provided. */
	different = 1;
	if (parsed->tokens[0].size == 4)
		different = memcmp(parsed->tokens[0].text, "sftp", 4);
	if (different != 0) {
		*what = "subsystem not supported";
		return EINVAL;
	}

	/* Checks and copies the command. */
	status = config_copy_path(&parsed->tokens[1], target->sftp_command, sizeof(target->sftp_command), CONFIG_PATH_PLAIN, CONFIG_PATH_NO_NONE, what);
	if (status != 0)
		return status;

	/* Succeeded: the command is set. */
	return 0;
}

/* Reads the value of one acted-on sshd_config keyword into target; 0 or EINVAL with *what set. */
static int
sshd_value(
	struct sshd_config *target,
	int id,
	const struct config_line *parsed,
	const char **what)
{
	unsigned number;
	int value;
	int status;
	char path[SSH_CONFIG_PATH_MAX];

	/* Reads the value by keyword. */
	switch (id) {
	case SSHD_KW_PORT:
		status = config_value_number(parsed, 1, 65535, &number, what);
		if (status == 0)
			target->port = (uint16_t)number;
		return status;
	case SSHD_KW_LISTEN_ADDRESS:
		return sshd_value_listen_address(target, parsed, what);
	case SSHD_KW_ADDRESS_FAMILY:
		status = config_value_name(parsed, address_family_names, sizeof(address_family_names) / sizeof(address_family_names[0]), &value, what);
		if (status == 0)
			target->inet6 = value;
		return status;
	case SSHD_KW_HOST_KEY:
		status = config_value_path(parsed, path, sizeof(path), CONFIG_PATH_PLAIN, CONFIG_PATH_NO_NONE, what);
		if (status != 0)
			return status;
		return config_add_path(target->host_key, &target->host_key_count, path, what);
	case SSHD_KW_PERMIT_ROOT:
		status = config_value_name(parsed, permit_root_names, sizeof(permit_root_names) / sizeof(permit_root_names[0]), &value, what);
		if (status == 0)
			target->permit_root = (enum sshd_permit_root)value;
		return status;
	case SSHD_KW_PASSWORD:
		return config_value_bool(parsed, &target->password, what);
	case SSHD_KW_PERMIT_EMPTY:
		return config_value_bool(parsed, &target->permit_empty, what);
	case SSHD_KW_PUBKEY:
		return config_value_bool(parsed, &target->pubkey, what);
	case SSHD_KW_AUTHORIZED_KEYS:
		return config_value_path(parsed, target->authorized_keys_file, sizeof(target->authorized_keys_file), CONFIG_PATH_TOKENS, CONFIG_PATH_NONE_WORD, what);
	case SSHD_KW_MAX_AUTH_TRIES:
		return config_value_number(parsed, 1, 1000, &target->max_auth_tries, what);
	case SSHD_KW_LOGIN_GRACE_TIME:
		return config_value_time(parsed, &target->login_grace_time, what);
	case SSHD_KW_MAX_SESSIONS:
		/* A limit above what one connection can hold is the largest the connection holds. */
		status = config_value_number(parsed, 0, 65535, &number, what);
		if (status == 0) {
			if (number > SSH_CHANNEL_MAX)
				number = SSH_CHANNEL_MAX;
			target->max_sessions = number;
		}

		/* Returns the result of the value. */
		return status;
	case SSHD_KW_MAX_STARTUPS:
		return sshd_value_max_startups(target, parsed, what);
	case SSHD_KW_STRICT_MODES:
		return config_value_bool(parsed, &target->strict_modes, what);
	case SSHD_KW_SUBSYSTEM:
		return sshd_value_subsystem(target, parsed, what);
	case SSHD_KW_ALIVE_INTERVAL:
		return config_value_time(parsed, &target->client_alive_interval, what);
	case SSHD_KW_ALIVE_COUNT:
		return config_value_number(parsed, 0, CONFIG_COUNT_MAX, &target->client_alive_count, what);
	default:
		break;
	}

	/* A number that is not one of the acted-on keywords cannot happen; it is refused. */
	*what = "unknown keyword";
	return EINVAL;
}

/*
 * Handles one line of sshd_config: finds the keyword, reads its value
 * and, when no earlier line gave the keyword, keeps the value.  A second
 * value goes through the same checks into a scratch copy that is thrown
 * away, so that a bad line is an error wherever it stands; a second Port
 * or ListenAddress that differs from the first is an error because only
 * one of each is kept; HostKey lines all add to the list.
 */
static int
sshd_config_apply(
	void *context,
	const struct config_line *parsed,
	const char **what)
{
	struct sshd_state *state;
	struct sshd_config scratch;
	struct sshd_config *target;
	const struct config_keyword *entry;
	uint32_t bit;
	int index;
	int repeated;
	int different;
	int status;

	/* Takes the state from the callback context. */
	state = context;

	/* Finds the keyword. */
	index = config_find_keyword(sshd_keywords, sizeof(sshd_keywords) / sizeof(sshd_keywords[0]), parsed->keyword, parsed->keyword_size);
	if (index < 0) {
		*what = "unknown keyword";
		return EINVAL;
	}

	/* Takes the keyword entry. */
	entry = &sshd_keywords[index];

	/* Match blocks are not supported. */
	if (entry->kind == CONFIG_KIND_MATCH) {
		*what = "Match is not supported";
		return EINVAL;
	}

	/* A keyword that is read and ignored needs only its value checked. */
	if (entry->kind != CONFIG_KIND_ACTED) {
		status = config_check_ignored(entry->kind, parsed, what);
		if (status != 0)
			return status;
		return 0;
	}

	/* A keyword already given is checked into a copy that is thrown away; HostKey always adds. */
	bit = (uint32_t)1U << entry->id;
	repeated = 0;
	if (entry->id != SSHD_KW_HOST_KEY && (state->seen & bit) != 0)
		repeated = 1;
	target = state->config;
	if (repeated) {
		memcpy(&scratch, state->config, sizeof(scratch));
		target = &scratch;
	}

	/* Reads the value. */
	status = sshd_value(target, entry->id, parsed, what);
	if (status != 0)
		return status;

	/* A second Port or ListenAddress must say the same as the first, since only one is kept. */
	if (repeated && entry->id == SSHD_KW_PORT && target->port != state->config->port) {
		*what = "only one Port is supported";
		return EINVAL;
	}

	/* Refuses a second ListenAddress that differs from the first. */
	different = strcmp(target->listen_address, state->config->listen_address);
	if (repeated && entry->id == SSHD_KW_LISTEN_ADDRESS && different != 0) {
		*what = "only one ListenAddress is supported";
		return EINVAL;
	}

	/* Remembers that the keyword has been given. */
	state->seen |= bit;

	/* Succeeded: the line is handled. */
	return 0;
}

/* Reads IdentityFile: a path, added to the list of identity files (the list takes all the sources, in order). */
static int
ssh_value_identity(
	struct ssh_config *target,
	const struct config_line *parsed,
	const char **what)
{
	char path[SSH_CONFIG_PATH_MAX];
	int status;

	/* Checks and copies the path. */
	status = config_value_path(parsed, path, sizeof(path), CONFIG_PATH_PLAIN, CONFIG_PATH_NO_NONE, what);
	if (status != 0)
		return status;

	/* Adds the path to the list. */
	status = config_add_path(target->identity, &target->identity_count, path, what);
	if (status != 0)
		return status;

	/* Succeeded: the path is in the list. */
	return 0;
}

/* Reads the value of one acted-on ssh_config keyword (not Host) into target; 0 or EINVAL with *what set. */
static int
ssh_value(
	struct ssh_config *target,
	int id,
	const struct config_line *parsed,
	const char **what)
{
	unsigned number;
	int value;
	int status;

	/* Reads the value by keyword. */
	switch (id) {
	case SSH_KW_USER:
		return config_value_word(parsed, target->user, sizeof(target->user), SSH_USER_MAX, what);
	case SSH_KW_HOST_NAME:
		return config_value_word(parsed, target->host_name, sizeof(target->host_name), sizeof(target->host_name) - 1, what);
	case SSH_KW_PORT:
		status = config_value_number(parsed, 1, 65535, &number, what);
		if (status == 0)
			target->port = (uint16_t)number;
		return status;
	case SSH_KW_IDENTITY:
		return ssh_value_identity(target, parsed, what);
	case SSH_KW_STRICT:
		status = config_value_name(parsed, strict_names, sizeof(strict_names) / sizeof(strict_names[0]), &value, what);
		if (status == 0)
			target->strict = (enum ssh_strict_host)value;
		return status;
	case SSH_KW_USER_KNOWN:
		/* Several files may be listed; the first is the one used. */
		if (parsed->token_count == 0) {
			*what = "missing value";
			return EINVAL;
		}

		/* Copies the path into the settings. */
		return config_copy_path(&parsed->tokens[0], target->user_known_hosts, sizeof(target->user_known_hosts), CONFIG_PATH_PLAIN, CONFIG_PATH_NONE_WORD, what);
	case SSH_KW_GLOBAL_KNOWN:
		if (parsed->token_count == 0) {
			*what = "missing value";
			return EINVAL;
		}

		/* Copies the path into the settings. */
		return config_copy_path(&parsed->tokens[0], target->global_known_hosts, sizeof(target->global_known_hosts), CONFIG_PATH_PLAIN, CONFIG_PATH_NONE_WORD, what);
	case SSH_KW_BATCH:
		return config_value_bool(parsed, &target->batch_mode, what);
	case SSH_KW_CONNECT_TIMEOUT:
		return config_value_time(parsed, &target->connect_timeout, what);
	case SSH_KW_ALIVE_INTERVAL:
		return config_value_time(parsed, &target->server_alive_interval, what);
	case SSH_KW_ALIVE_COUNT:
		return config_value_number(parsed, 0, CONFIG_COUNT_MAX, &target->server_alive_count, what);
	case SSH_KW_REQUEST_TTY:
		status = config_value_name(parsed, request_tty_names, sizeof(request_tty_names) / sizeof(request_tty_names[0]), &value, what);
		if (status == 0)
			target->request_tty = (enum ssh_request_tty)value;
		return status;
	case SSH_KW_PASSWORD:
		return config_value_bool(parsed, &target->password, what);
	case SSH_KW_PUBKEY:
		return config_value_bool(parsed, &target->pubkey, what);
	case SSH_KW_LOG_LEVEL:
		status = config_value_name(parsed, log_level_names, sizeof(log_level_names) / sizeof(log_level_names[0]), &value, what);
		if (status == 0)
			target->log_level = value;
		return status;
	default:
		break;
	}

	/* A number that is not one of the acted-on keywords cannot happen; it is refused. */
	*what = "unknown keyword";
	return EINVAL;
}

/*
 * Handles a Host line: the lines after it apply when some pattern
 * matches the host name given and no negated pattern does (ssh_config(5)
 * Host and PATTERNS; the patterns are separated by white space).
 */
static int
ssh_apply_host(
	struct ssh_state *state,
	const struct config_line *parsed,
	const char **what)
{
	const struct config_token *token;
	unsigned index;
	int matched;
	int negated;
	int negation;
	int hit;

	/* A Host line cannot be a -o option, which has no block to start. */
	if (state->option) {
		*what = "Host cannot be given as an option";
		return EINVAL;
	}

	/* A Host line names at least one pattern. */
	if (parsed->token_count == 0) {
		*what = "missing value";
		return EINVAL;
	}

	/* Matches the host name against each pattern; a negated pattern that matches rules the block out. */
	matched = 0;
	negated = 0;
	for (index = 0; index < parsed->token_count; index++) {
		token = &parsed->tokens[index];
		negation = 0;
		if (token->size > 0 && token->text[0] == '!')
			negation = 1;
		hit = ssh_pattern_match(token->text + negation, token->size - (size_t)negation, state->host, 1);
		if (hit == 0)
			continue;
		if (negation)
			negated = 1;
		else
			matched = 1;
	}

	/* The block applies when a pattern matched and no negated one did. */
	state->active = 0;
	if (matched && !negated)
		state->active = 1;

	/* Succeeded: the block is open. */
	return 0;
}

/*
 * Handles one line of ssh_config or one -o option: finds the keyword,
 * reads its value and keeps it when the line is in a block that applies
 * and no source has set the field before (IdentityFile adds to its list
 * instead).  A value that is not kept goes through the same checks into
 * a scratch copy that is thrown away, so a bad line is an error whether
 * or not its Host block matched.
 */
static int
ssh_config_apply(
	void *context,
	const struct config_line *parsed,
	const char **what)
{
	struct ssh_state *state;
	struct ssh_config scratch;
	struct ssh_config *target;
	const struct config_keyword *entry;
	uint32_t bit;
	int index;
	int keep;
	int status;

	/* Takes the state from the callback context. */
	state = context;

	/* Finds the keyword. */
	index = config_find_keyword(ssh_keywords, sizeof(ssh_keywords) / sizeof(ssh_keywords[0]), parsed->keyword, parsed->keyword_size);
	if (index < 0) {
		*what = "unknown keyword";
		return EINVAL;
	}

	/* Takes the keyword entry. */
	entry = &ssh_keywords[index];

	/* Match blocks are not supported. */
	if (entry->kind == CONFIG_KIND_MATCH) {
		*what = "Match is not supported";
		return EINVAL;
	}

	/* A keyword that is read and ignored needs only its value checked. */
	if (entry->kind != CONFIG_KIND_ACTED) {
		status = config_check_ignored(entry->kind, parsed, what);
		if (status != 0)
			return status;
		return 0;
	}

	/* Host starts a block. */
	if (entry->id == SSH_KW_HOST) {
		status = ssh_apply_host(state, parsed, what);
		if (status != 0)
			return status;
		return 0;
	}

	/* The value is kept when the block applies and no source has set the field; IdentityFile always adds. */
	bit = (uint32_t)1U << entry->id;
	keep = 1;
	if (!state->active)
		keep = 0;
	if (entry->id != SSH_KW_IDENTITY && (state->config->set_mask & bit) != 0)
		keep = 0;
	target = state->config;
	if (!keep) {
		memcpy(&scratch, state->config, sizeof(scratch));
		target = &scratch;
	}

	/* Reads the value. */
	status = ssh_value(target, entry->id, parsed, what);
	if (status != 0)
		return status;

	/* Records that a source has set the field, so a later source does not override it. */
	if (keep)
		state->config->set_mask |= bit;

	/* Succeeded: the line is handled. */
	return 0;
}
