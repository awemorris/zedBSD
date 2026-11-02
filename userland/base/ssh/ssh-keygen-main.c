/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ssh-keygen program: makes ssh-ed25519 keys, prints fingerprints and
 * public keys, makes the host key of a machine, and removes a host from a
 * known_hosts file (the options are listed in keygen_usage; their meaning
 * follows ssh-keygen(1) of OpenSSH).
 *
 * Only ed25519 keys without a passphrase are supported.  A key file is
 * written to a temporary name and renamed into place, so an interrupted
 * run leaves no half-written key, and a private key is created with mode
 * 0600 whatever the umask is.  An existing key is never overwritten and
 * the program never asks a question: it refuses.
 */

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ssh-hostfile.h"

/* The longest path the program builds, with its NUL. */
#define KEYGEN_PATH_MAX		4096U

/* The only key type the program makes (the argument of -t). */
#define KEYGEN_TYPE		"ed25519"

/* The option letters getopt reads. */
#define KEYGEN_OPTIONS		"t:f:N:C:qlyAR:"

/* The directory under the home directory that holds a user's keys and known_hosts, and the file names inside it. */
#define KEYGEN_USER_DIRECTORY	"/.ssh"
#define KEYGEN_USER_KEY		"id_ed25519"
#define KEYGEN_USER_KNOWN_HOSTS	"known_hosts"

/* The host key of the machine that -A makes; the -f prefix of -A is put in front of it. */
#define KEYGEN_HOST_KEY		"/etc/ssh/ssh_host_ed25519_key"

/* The suffix of a public key file next to its private key file, and of the file that keeps the old known_hosts. */
#define KEYGEN_PUBLIC_SUFFIX	".pub"
#define KEYGEN_OLD_SUFFIX	".old"

/* The mode of a private key file and of a public key file before the umask is applied (ssh-keygen(1)). */
#define KEYGEN_PRIVATE_MODE	0600
#define KEYGEN_PUBLIC_MODE	0644

/* The mode of the directory made for a user's keys. */
#define KEYGEN_DIRECTORY_MODE	0700

/* The room for one public key line as ssh_key_put_public_line writes it: the type, the base64 blob, a comment and an LF. */
#define KEYGEN_PUBLIC_LINE_MAX	(SSH_KEY_COMMENT_MAX + 128U)

/* The port a plain host name in known_hosts stands for. */
#define KEYGEN_STANDARD_PORT	22U

/*
 * What the command line asked for.
 *
 * The string fields point into argv and are NULL when the option was not
 * given.  The flags are 1 when the option was given.
 */
struct keygen_options {
	const char *type;
	const char *file;
	const char *passphrase;
	const char *comment;
	const char *remove_host;
	int quiet;
	int list;
	int show;
	int host_keys;
};

static void keygen_usage(void);
static int keygen_parse_options(int argc, char **argv, struct keygen_options *options);
static int keygen_text_equal(const char *left, const char *right);
static void keygen_report(const char *subject, int error);
static int keygen_join(char *out, size_t capacity, const char *first, const char *second);
static int keygen_home_path(const char *file_name, char *directory, size_t directory_size, char *path, size_t path_size);
static int keygen_key_file_path(const struct keygen_options *options, char *path, size_t path_size);
static int keygen_check_absent(const char *path);
static int keygen_read_file(const char *path, struct ssh_buf *out, size_t limit);
static int keygen_write_file(const char *path, const uint8_t *data, size_t size, mode_t mode, int replace);
static void keygen_default_comment(char *comment, size_t capacity);
static int keygen_parse_host(const char *spec, char *host, size_t host_size, uint16_t *port);
static int keygen_load_key(const char *path, int want_private, struct ssh_key *key);
static int keygen_generate_pair(const char *path, const char *comment, int replace_public, int announce);
static int keygen_run_generate(const struct keygen_options *options);
static int keygen_run_list(const struct keygen_options *options);
static int keygen_run_show(const struct keygen_options *options);
static int keygen_run_host_keys(const struct keygen_options *options);
static int keygen_run_remove(const struct keygen_options *options);

/*
 * Runs the ssh-keygen program.
 *
 * Returns 0 when the request was carried out and 1 for a refusal, an
 * error or a command line that is not understood.
 */
int
main(
	int argc,
	char **argv)
{
	struct keygen_options options;
	int modes;
	int status;
	int same;

	/* Reads the command line. */
	status = keygen_parse_options(argc, argv, &options);
	if (status != 0) {
		keygen_usage();
		return 1;
	}

	/* Counts the options that each ask for a different job: at most one may be given. */
	modes = 0;
	if (options.list)
		modes++;
	if (options.show)
		modes++;
	if (options.host_keys)
		modes++;
	if (options.remove_host != NULL)
		modes++;

	/* Refuses two jobs at once. */
	if (modes > 1) {
		keygen_usage();
		return 1;
	}

	/* Refuses a key type other than ed25519, whichever job was asked for. */
	if (options.type != NULL) {
		same = keygen_text_equal(options.type, KEYGEN_TYPE);
		if (!same) {
			fprintf(stderr, "ssh-keygen: key type %s is not supported (only %s)\n", options.type, KEYGEN_TYPE);
			return 1;
		}
	}

	/* Refuses a passphrase: private keys are written without encryption. */
	if (options.passphrase != NULL && options.passphrase[0] != '\0') {
		fprintf(stderr, "ssh-keygen: a passphrase is not supported, use -N ''\n");
		return 1;
	}

	/* Runs the job that was asked for. */
	if (options.list) {
		status = keygen_run_list(&options);
	} else if (options.show) {
		status = keygen_run_show(&options);
	} else if (options.host_keys) {
		status = keygen_run_host_keys(&options);
	} else if (options.remove_host != NULL) {
		status = keygen_run_remove(&options);
	} else {
		status = keygen_run_generate(&options);
	}

	/* Reports that the job failed. */
	if (status != 0)
		return 1;

	/* Succeeded: the job was carried out. */
	return 0;
}

/*
 * Prints the forms of the command line the program understands.
 */
static void
keygen_usage(void)
{
	/* Describes every accepted form of the command line. */
	fputs("usage: ssh-keygen [-q] [-t ed25519] [-f file] [-N ''] [-C comment]\n"
	      "       ssh-keygen [-q] -l [-f file]\n"
	      "       ssh-keygen -y [-f file]\n"
	      "       ssh-keygen [-q] -A [-f prefix]\n"
	      "       ssh-keygen [-q] -R host [-f known_hosts]\n", stderr);
}

/*
 * Reads the command line into options.
 *
 * Returns 0, or 1 when an option is unknown, lacks its value, or an
 * operand is left over.
 */
static int
keygen_parse_options(
	int argc,
	char **argv,
	struct keygen_options *options)
{
	int option;

	/* Starts with every option absent. */
	memset(options, 0, sizeof(*options));

	/* Reads each option letter in turn. */
	option = getopt(argc, argv, KEYGEN_OPTIONS);
	while (option != -1) {
		/* Records what the letter asks for. */
		switch (option) {
		case 't':
			options->type = optarg;
			break;
		case 'f':
			options->file = optarg;
			break;
		case 'N':
			options->passphrase = optarg;
			break;
		case 'C':
			options->comment = optarg;
			break;
		case 'q':
			options->quiet = 1;
			break;
		case 'l':
			options->list = 1;
			break;
		case 'y':
			options->show = 1;
			break;
		case 'A':
			options->host_keys = 1;
			break;
		case 'R':
			options->remove_host = optarg;
			break;
		default:
			/* An unknown letter or a missing value. */
			return 1;
		}

		option = getopt(argc, argv, KEYGEN_OPTIONS);
	}

	/* Refuses an operand: the program takes none. */
	if (optind != argc)
		return 1;

	/* Succeeded: the command line was understood. */
	return 0;
}

/*
 * Tells whether two C strings are the same (1) or not (0).
 */
static int
keygen_text_equal(
	const char *left,
	const char *right)
{
	int difference;

	/* Compares the two strings byte for byte. */
	difference = strcmp(left, right);
	if (difference != 0)
		return 0;

	/* Succeeded: the strings are the same. */
	return 1;
}

/*
 * Prints "ssh-keygen: subject: reason" for an error value.
 */
static void
keygen_report(
	const char *subject,
	int error)
{
	/* Names the thing that failed and the reason the system gives. */
	fprintf(stderr, "ssh-keygen: %s: %s\n", subject, strerror(error));
}

/*
 * Joins two strings into out.
 *
 * Returns 0, or ENAMETOOLONG when they do not fit with the NUL.
 */
static int
keygen_join(
	char *out,
	size_t capacity,
	const char *first,
	const char *second)
{
	size_t first_size;
	size_t second_size;

	/* Measures both parts. */
	first_size = strlen(first);
	second_size = strlen(second);

	/* Refuses a result that would not fit with its NUL. */
	if (first_size >= capacity)
		return ENAMETOOLONG;
	if (second_size >= capacity - first_size)
		return ENAMETOOLONG;

	/* Copies the two parts one after the other. */
	memcpy(out, first, first_size);
	memcpy(out + first_size, second, second_size);
	out[first_size + second_size] = '\0';

	/* Succeeded: out holds the joined string. */
	return 0;
}

/*
 * Builds the path of a file in the user's key directory (under HOME, or
 * the account's home directory when HOME is not set) and the path of that
 * directory.
 *
 * Returns 0, ENOENT when no home directory is known, or ENAMETOOLONG.
 */
static int
keygen_home_path(
	const char *file_name,
	char *directory,
	size_t directory_size,
	char *path,
	size_t path_size)
{
	char slashed[KEYGEN_PATH_MAX];
	const char *home;
	struct passwd *account;
	int error;

	/* Takes the home directory from the environment first. */
	home = getenv("HOME");
	if (home == NULL || home[0] == '\0') {
		/* Falls back to the account database. */
		account = getpwuid(getuid());
		if (account == NULL)
			return ENOENT;
		home = account->pw_dir;
		if (home == NULL || home[0] == '\0')
			return ENOENT;
	}

	/* Builds the key directory under the home directory. */
	error = keygen_join(directory, directory_size, home, KEYGEN_USER_DIRECTORY);
	if (error != 0)
		return error;

	/* Builds the file in that directory. */
	error = keygen_join(slashed, sizeof(slashed), directory, "/");
	if (error != 0)
		return error;
	error = keygen_join(path, path_size, slashed, file_name);
	if (error != 0)
		return error;

	/* Succeeded: directory and path are set. */
	return 0;
}

/*
 * Chooses the key file of -l and -y: the -f file, or the default key file
 * in the home directory.
 *
 * Returns 0, or an error value after a message was printed.
 */
static int
keygen_key_file_path(
	const struct keygen_options *options,
	char *path,
	size_t path_size)
{
	char directory[KEYGEN_PATH_MAX];
	int error;

	/* Takes the -f file when it was given. */
	if (options->file != NULL) {
		error = keygen_join(path, path_size, options->file, "");
		if (error != 0) {
			keygen_report(options->file, error);
			return error;
		}
		return 0;
	}

	/* Otherwise takes the default key file. */
	error = keygen_home_path(KEYGEN_USER_KEY, directory, sizeof(directory), path, path_size);
	if (error != 0) {
		fprintf(stderr, "ssh-keygen: cannot find the home directory, use -f\n");
		return error;
	}

	/* Succeeded: path is the key file. */
	return 0;
}

/*
 * Checks that nothing is at path (a link counts, whatever it points to).
 *
 * Returns 0 when nothing is there, EEXIST when something is, or the error
 * that stopped the check.
 */
static int
keygen_check_absent(
	const char *path)
{
	struct stat information;
	int status;

	/* Looks at the path itself, not at what a link points to. */
	status = lstat(path, &information);
	if (status == 0)
		return EEXIST;

	/* Takes a missing path as absent. */
	if (errno == ENOENT)
		return 0;

	/* Reports why the check could not be made. */
	return errno;
}

/*
 * Reads the whole file at path into out.
 *
 * Returns 0, EMSGSIZE when the file is longer than limit bytes, ENOMEM,
 * or the error of opening or reading.  out is made secret by the caller
 * when the file may hold a key.
 */
static int
keygen_read_file(
	const char *path,
	struct ssh_buf *out,
	size_t limit)
{
	uint8_t chunk[4096];
	ssize_t count;
	int descriptor;
	int error;

	/* Opens the file for reading. */
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return errno;

	/* Reads until the end of the file or until a failure. */
	error = 0;
	for (;;) {
		count = read(descriptor, chunk, sizeof(chunk));
		if (count < 0) {
			if (errno == EINTR)
				continue;
			error = errno;
			break;
		}
		if (count == 0)
			break;

		/* Refuses a file longer than the limit. */
		if ((size_t)count > limit - out->size) {
			error = EMSGSIZE;
			break;
		}

		/* Appends what was read. */
		ssh_buf_put_bytes(out, chunk, (size_t)count);
		error = ssh_buf_error(out);
		if (error != 0)
			break;
	}

	/* Closes the file and wipes the copy of what was read on the stack. */
	close(descriptor);
	ssh_wipe(chunk, sizeof(chunk));

	/* Reports how the read ended. */
	if (error != 0)
		return error;

	/* Succeeded: out holds the whole file. */
	return 0;
}

/*
 * Writes size bytes at data to path through a temporary file.
 *
 * The temporary file is made new beside path with mode (the umask can only
 * take bits away), written, flushed to the disk and renamed over path, and
 * is removed when anything fails.  When replace is 0 and something is at
 * path, nothing is written and EEXIST is returned.  Returns 0, EEXIST,
 * ENAMETOOLONG or the error of the system.
 */
static int
keygen_write_file(
	const char *path,
	const uint8_t *data,
	size_t size,
	mode_t mode,
	int replace)
{
	char suffix[32];
	char temp_path[KEYGEN_PATH_MAX];
	size_t done;
	ssize_t count;
	int descriptor;
	int status;
	int error;

	/* Names the temporary file after this process, so another run does not share it. */
	snprintf(suffix, sizeof(suffix), ".tmp.%lu", (unsigned long)getpid());
	error = keygen_join(temp_path, sizeof(temp_path), path, suffix);
	if (error != 0)
		return error;

	/* Makes the temporary file new; a leftover of the same name is an error, never reused. */
	descriptor = open(temp_path, O_WRONLY | O_CREAT | O_EXCL, mode);
	if (descriptor < 0)
		return errno;

	/* Writes every byte, continuing after a partial write or a signal. */
	error = 0;
	done = 0;
	while (done < size) {
		count = write(descriptor, data + done, size - done);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			error = errno;
			break;
		}
		if (count == 0) {
			error = EIO;
			break;
		}
		done += (size_t)count;
	}

	/* Flushes the contents to the disk before the name is switched. */
	if (error == 0) {
		status = fsync(descriptor);
		if (status != 0)
			error = errno;
	}

	/* Closes the temporary file; a failed close can lose written data. */
	status = close(descriptor);
	if (status != 0 && error == 0)
		error = errno;

	/* Refuses to replace something that is already there when replacing is not allowed. */
	if (error == 0 && !replace)
		error = keygen_check_absent(path);

	/* Puts the file in place under its real name. */
	if (error == 0) {
		status = rename(temp_path, path);
		if (status != 0)
			error = errno;
	}

	/* Removes the temporary file after a failure, so nothing half-written stays. */
	if (error != 0) {
		unlink(temp_path);
		return error;
	}

	/* Succeeded: path holds the data. */
	return 0;
}

/*
 * Builds the default comment of a new key, "user@host"; a name that is
 * unknown is "unknown".
 */
static void
keygen_default_comment(
	char *comment,
	size_t capacity)
{
	char host[256];
	const char *user;
	struct passwd *account;
	int status;

	/* Takes the account name of the user that runs the program. */
	user = "unknown";
	account = getpwuid(getuid());
	if (account != NULL && account->pw_name != NULL && account->pw_name[0] != '\0')
		user = account->pw_name;

	/* Takes the machine's name, leaving the last byte for the NUL. */
	memset(host, 0, sizeof(host));
	status = gethostname(host, sizeof(host) - 1U);
	if (status != 0 || host[0] == '\0')
		strcpy(host, "unknown");

	/* Joins them; a name too long for the comment is cut. */
	snprintf(comment, capacity, "%s@%s", user, host);
}

/*
 * Splits the argument of -R into a host name and a port: "host" is port
 * 22, "[host]:port" names the port.
 *
 * Returns 0, or EINVAL for an empty name, a name longer than host_size - 1,
 * a missing "]:" or a port that is not 1 to 65535.
 */
static int
keygen_parse_host(
	const char *spec,
	char *host,
	size_t host_size,
	uint16_t *port)
{
	size_t length;
	size_t name_length;
	size_t position;
	unsigned long value;

	/* Measures the argument. */
	length = strlen(spec);

	/* A bare name is a host on the standard port. */
	if (spec[0] != '[') {
		if (length == 0 || length >= host_size)
			return EINVAL;
		memcpy(host, spec, length + 1U);
		*port = (uint16_t)KEYGEN_STANDARD_PORT;
		return 0;
	}

	/* Finds the "]" that ends the bracketed name. */
	position = 1;
	while (position < length && spec[position] != ']')
		position++;
	if (position >= length)
		return EINVAL;

	/* Copies the name between the brackets. */
	name_length = position - 1U;
	if (name_length == 0 || name_length >= host_size)
		return EINVAL;
	memcpy(host, spec + 1, name_length);
	host[name_length] = '\0';

	/* Requires ":" and at least one digit after the bracket. */
	position++;
	if (position >= length || spec[position] != ':')
		return EINVAL;
	position++;
	if (position >= length)
		return EINVAL;

	/* Reads the port digits, stopping as soon as the value is too large. */
	value = 0;
	while (position < length) {
		if (spec[position] < '0' || spec[position] > '9')
			return EINVAL;
		value = value * 10UL + (unsigned long)(spec[position] - '0');
		if (value > 65535UL)
			return EINVAL;
		position++;
	}
	if (value == 0)
		return EINVAL;

	/* Succeeded: the host and the port are set. */
	*port = (uint16_t)value;
	return 0;
}

/*
 * Reads a key from a file: a private key file (only that when
 * want_private is 1) or else a private key file or a one-line public key.
 *
 * Returns 0, or an error value after a message was printed.  key is
 * cleared on an error.
 */
static int
keygen_load_key(
	const char *path,
	int want_private,
	struct ssh_key *key)
{
	struct ssh_buf text;
	const char *content;
	size_t line_size;
	int private_error;
	int public_error;
	int error;

	/* Keeps the file's bytes in a buffer that is wiped when freed, because they may hold a seed. */
	ssh_buf_init(&text, SSH_KEY_FILE_MAX, 1);
	ssh_key_clear(key);

	/* Reads the file. */
	error = keygen_read_file(path, &text, SSH_KEY_FILE_MAX);
	if (error != 0) {
		keygen_report(path, error);
		goto done;
	}

	/* An empty file has no bytes to point at, so an empty string stands for it. */
	content = "";
	if (text.size != 0)
		content = (const char *)text.data;

	/* Reads the file as an OpenSSH private key file. */
	private_error = ssh_key_parse_private(content, text.size, key);
	if (private_error == 0) {
		error = 0;
		goto done;
	}

	/* A private key was required: explains why this file is not one. */
	if (want_private) {
		if (private_error == ENOTSUP) {
			fprintf(stderr, "ssh-keygen: %s: only an unencrypted ssh-ed25519 private key is supported\n", path);
		} else {
			fprintf(stderr, "ssh-keygen: %s: not a valid private key file\n", path);
		}
		error = private_error;
		goto done;
	}

	/* Takes the first line of the file as a public key line. */
	line_size = 0;
	while (line_size < text.size && text.data[line_size] != '\n')
		line_size++;
	public_error = ssh_key_parse_public_line(content, line_size, key);
	if (public_error == 0) {
		error = 0;
		goto done;
	}

	/* Explains that the file is neither kind of key. */
	if (public_error == ENOTSUP || private_error == ENOTSUP) {
		fprintf(stderr, "ssh-keygen: %s: only an unencrypted ssh-ed25519 key is supported\n", path);
	} else {
		fprintf(stderr, "ssh-keygen: %s: not a valid key file\n", path);
	}
	error = public_error;

done:
	/* Wipes the file's bytes, and the key when nothing was read. */
	ssh_buf_free(&text);
	if (error != 0)
		ssh_key_clear(key);

	/* Reports that the file holds no usable key. */
	if (error != 0)
		return error;

	/* Succeeded: key holds the key of the file. */
	return 0;
}

/*
 * Makes a new key and writes it to path (private, mode 0600) and path.pub
 * (public, mode 0644).
 *
 * Nothing at path is replaced.  The public file may be replaced when
 * replace_public is 1 (a left-over of an interrupted host key run).
 * announce is 1 to print what is done and the fingerprint.  Returns 0 or
 * an error value after a message was printed.
 */
static int
keygen_generate_pair(
	const char *path,
	const char *comment,
	int replace_public,
	int announce)
{
	char public_path[KEYGEN_PATH_MAX];
	char fingerprint[SSH_KEY_FINGERPRINT_SIZE];
	struct ssh_key key;
	struct ssh_buf private_text;
	struct ssh_buf public_text;
	uint32_t checkint;
	int error;

	/* Prepares the buffers; the private text is secret and is wiped when freed. */
	ssh_buf_init(&private_text, SSH_KEY_FILE_MAX, 1);
	ssh_buf_init(&public_text, KEYGEN_PUBLIC_LINE_MAX, 0);
	ssh_key_clear(&key);

	/* Names the public file. */
	error = keygen_join(public_path, sizeof(public_path), path, KEYGEN_PUBLIC_SUFFIX);
	if (error != 0) {
		keygen_report(path, error);
		goto done;
	}

	/* Refuses to replace a private key that exists. */
	error = keygen_check_absent(path);
	if (error == EEXIST) {
		fprintf(stderr, "ssh-keygen: %s already exists, not overwriting it\n", path);
		goto done;
	}
	if (error != 0) {
		keygen_report(path, error);
		goto done;
	}

	/* Refuses to replace a public key unless that is allowed. */
	if (!replace_public) {
		error = keygen_check_absent(public_path);
		if (error == EEXIST) {
			fprintf(stderr, "ssh-keygen: %s already exists, not overwriting it\n", public_path);
			goto done;
		}
		if (error != 0) {
			keygen_report(public_path, error);
			goto done;
		}
	}

	/* Makes the key from 32 random bytes. */
	error = ssh_key_generate(&key, comment);
	if (error != 0) {
		fprintf(stderr, "ssh-keygen: cannot make a key: %s\n", strerror(error));
		goto done;
	}

	/* Takes a random check value for the private key file. */
	error = ssh_random(&checkint, sizeof(checkint));
	if (error != 0) {
		fprintf(stderr, "ssh-keygen: no random numbers: %s\n", strerror(error));
		goto done;
	}

	/* Encodes the private key file. */
	error = ssh_key_put_private(&key, checkint, &private_text);
	if (error != 0) {
		fprintf(stderr, "ssh-keygen: cannot encode the key: %s\n", strerror(error));
		goto done;
	}

	/* Encodes the public key line. */
	ssh_key_put_public_line(key.public_key, key.comment, &public_text);
	error = ssh_buf_error(&public_text);
	if (error != 0) {
		fprintf(stderr, "ssh-keygen: cannot encode the public key: %s\n", strerror(error));
		goto done;
	}

	/* Writes the private key first, so a public key never exists without it. */
	error = keygen_write_file(path, private_text.data, private_text.size, KEYGEN_PRIVATE_MODE, 0);
	if (error == EEXIST) {
		fprintf(stderr, "ssh-keygen: %s already exists, not overwriting it\n", path);
		goto done;
	}
	if (error != 0) {
		keygen_report(path, error);
		goto done;
	}

	/* Writes the public key. */
	error = keygen_write_file(public_path, public_text.data, public_text.size, KEYGEN_PUBLIC_MODE, replace_public);
	if (error != 0) {
		keygen_report(public_path, error);
		fprintf(stderr, "ssh-keygen: the private key %s was written; ssh-keygen -y -f %s makes the public key\n", path, path);
		goto done;
	}

	/* Tells what was done and the fingerprint of the new key. */
	if (announce) {
		error = ssh_key_fingerprint(key.public_key, fingerprint, sizeof(fingerprint));
		if (error != 0) {
			fprintf(stderr, "ssh-keygen: cannot compute the fingerprint: %s\n", strerror(error));
			goto done;
		}
		printf("Generating public/private ed25519 key pair.\n");
		printf("Your identification has been saved in %s\n", path);
		printf("Your public key has been saved in %s\n", public_path);
		printf("The key fingerprint is:\n");
		if (key.comment[0] != '\0') {
			printf("%s %s\n", fingerprint, key.comment);
		} else {
			printf("%s\n", fingerprint);
		}
	}

done:
	/* Wipes the seed and the private text, and frees the buffers. */
	ssh_key_clear(&key);
	ssh_buf_free(&private_text);
	ssh_buf_free(&public_text);

	/* Reports why the key pair is not in place. */
	if (error != 0)
		return error;

	/* Succeeded: both files are in place. */
	return 0;
}

/*
 * Makes a new key pair at the -f path, or at the default path in the home
 * directory (making that directory when it is missing).
 *
 * Returns 0 or an error value after a message was printed.
 */
static int
keygen_run_generate(
	const struct keygen_options *options)
{
	char comment[SSH_KEY_COMMENT_MAX];
	char directory[KEYGEN_PATH_MAX];
	char default_path[KEYGEN_PATH_MAX];
	const char *path;
	const char *chosen_comment;
	size_t index;
	int status;
	int error;

	/* Refuses to make a key unless the empty passphrase was asked for: there is no prompt. */
	if (options->passphrase == NULL) {
		fprintf(stderr, "ssh-keygen: there is no passphrase prompt, use -N '' for a key without a passphrase\n");
		return EINVAL;
	}

	/* Takes the comment of the command line or builds user@host. */
	chosen_comment = options->comment;
	if (chosen_comment == NULL) {
		keygen_default_comment(comment, sizeof(comment));
		chosen_comment = comment;
	}

	/* Refuses a comment that would break the one-line public key form. */
	for (index = 0;
	     chosen_comment[index] != '\0';
	     index++) {
		if (chosen_comment[index] == '\n' || chosen_comment[index] == '\r') {
			fprintf(stderr, "ssh-keygen: the comment may not contain a line break\n");
			return EINVAL;
		}
	}

	/* Takes the -f path when it was given. */
	path = options->file;
	if (path != NULL) {
		error = keygen_generate_pair(path, chosen_comment, 0, !options->quiet);
		if (error != 0)
			return error;
		return 0;
	}

	/* Otherwise takes the default key file in the user's key directory. */
	error = keygen_home_path(KEYGEN_USER_KEY, directory, sizeof(directory), default_path, sizeof(default_path));
	if (error != 0) {
		fprintf(stderr, "ssh-keygen: cannot find the home directory, use -f\n");
		return error;
	}

	/* Makes the key directory when it is not there yet; an existing one is used as it is. */
	status = mkdir(directory, KEYGEN_DIRECTORY_MODE);
	if (status != 0 && errno != EEXIST) {
		error = errno;
		keygen_report(directory, error);
		return error;
	}

	/* Makes the key and writes it. */
	error = keygen_generate_pair(default_path, chosen_comment, 0, !options->quiet);
	if (error != 0)
		return error;

	/* Succeeded: the key pair was written. */
	return 0;
}

/*
 * Prints the fingerprint of the key in the -f file (the default key file
 * when -f is not given): its bit size, fingerprint, comment and type.
 *
 * Returns 0 or an error value after a message was printed.
 */
static int
keygen_run_list(
	const struct keygen_options *options)
{
	char fingerprint[SSH_KEY_FINGERPRINT_SIZE];
	char path[KEYGEN_PATH_MAX];
	struct ssh_key key;
	const char *comment;
	int error;

	/* Chooses the key file. */
	error = keygen_key_file_path(options, path, sizeof(path));
	if (error != 0)
		return error;

	/* Reads the key, from a public key file or a private key file. */
	error = keygen_load_key(path, 0, &key);
	if (error != 0)
		return error;

	/* Computes the fingerprint of the public key. */
	error = ssh_key_fingerprint(key.public_key, fingerprint, sizeof(fingerprint));
	if (error != 0) {
		fprintf(stderr, "ssh-keygen: cannot compute the fingerprint: %s\n", strerror(error));
		ssh_key_clear(&key);
		return error;
	}

	/* Prints the line in the form ssh-keygen -l has: size, fingerprint, comment, type. */
	comment = key.comment;
	if (comment[0] == '\0')
		comment = "no comment";
	printf("256 %s %s (ED25519)\n", fingerprint, comment);

	/* Wipes the key, which may hold a seed. */
	ssh_key_clear(&key);

	/* Succeeded: the fingerprint was printed. */
	return 0;
}

/*
 * Prints the public key of the private key in the -f file (the default key
 * file when -f is not given) in the one-line form.
 *
 * Returns 0 or an error value after a message was printed.
 */
static int
keygen_run_show(
	const struct keygen_options *options)
{
	char path[KEYGEN_PATH_MAX];
	struct ssh_key key;
	struct ssh_buf line;
	int error;

	/* Chooses the key file. */
	error = keygen_key_file_path(options, path, sizeof(path));
	if (error != 0)
		return error;

	/* Reads the private key. */
	error = keygen_load_key(path, 1, &key);
	if (error != 0)
		return error;

	/* Encodes the public half as one line, then wipes the seed. */
	ssh_buf_init(&line, KEYGEN_PUBLIC_LINE_MAX, 0);
	ssh_key_put_public_line(key.public_key, key.comment, &line);
	ssh_key_clear(&key);
	error = ssh_buf_error(&line);
	if (error != 0) {
		fprintf(stderr, "ssh-keygen: cannot encode the public key: %s\n", strerror(error));
		ssh_buf_free(&line);
		return error;
	}

	/* Prints the line, which already ends with a line feed. */
	fwrite(line.data, 1, line.size, stdout);
	ssh_buf_free(&line);

	/* Succeeded: the public key was printed. */
	return 0;
}

/*
 * Makes the host key of the machine when it does not exist: the file
 * KEYGEN_HOST_KEY under the -f prefix (the root directory when -f is not
 * given).
 *
 * A host key that exists is kept as it is.  Returns 0 or an error value
 * after a message was printed.
 */
static int
keygen_run_host_keys(
	const struct keygen_options *options)
{
	char path[KEYGEN_PATH_MAX];
	char comment[SSH_KEY_COMMENT_MAX];
	const char *prefix;
	int error;

	/* Takes the prefix of the command line, or none. */
	prefix = options->file;
	if (prefix == NULL)
		prefix = "";

	/* Names the host key file under the prefix. */
	error = keygen_join(path, sizeof(path), prefix, KEYGEN_HOST_KEY);
	if (error != 0) {
		keygen_report(prefix, error);
		return error;
	}

	/* Keeps a host key that is already there. */
	error = keygen_check_absent(path);
	if (error == EEXIST)
		return 0;
	if (error != 0) {
		keygen_report(path, error);
		return error;
	}

	/* Makes the host key, replacing a public key left from an interrupted run. */
	keygen_default_comment(comment, sizeof(comment));
	error = keygen_generate_pair(path, comment, 1, 0);
	if (error != 0)
		return error;

	/* Says which host key was made. */
	if (!options->quiet)
		printf("ssh-keygen: generating new host keys: ED25519\n");

	/* Succeeded: the host key was made. */
	return 0;
}

/*
 * Removes every line that names the -R host from the known_hosts file (the
 * -f file, or the default file in the home directory), keeping the old
 * contents in the same name with ".old" added.
 *
 * A host that no line names leaves both files alone and is not an error.
 * Returns 0 or an error value after a message was printed.
 */
static int
keygen_run_remove(
	const struct keygen_options *options)
{
	char host[SSH_HOST_MAX + 1U];
	char directory[KEYGEN_PATH_MAX];
	char default_path[KEYGEN_PATH_MAX];
	char old_path[KEYGEN_PATH_MAX];
	struct stat information;
	struct ssh_buf text;
	struct ssh_buf kept;
	const char *path;
	const char *content;
	uint16_t port;
	unsigned removed;
	mode_t mode;
	int status;
	int error;

	/* Splits the host argument into a name and a port. */
	error = keygen_parse_host(options->remove_host, host, sizeof(host), &port);
	if (error != 0) {
		fprintf(stderr, "ssh-keygen: %s is not a host name or [host]:port\n", options->remove_host);
		return error;
	}

	/* Takes the -f file or the default known_hosts file. */
	path = options->file;
	if (path == NULL) {
		error = keygen_home_path(KEYGEN_USER_KNOWN_HOSTS, directory, sizeof(directory), default_path, sizeof(default_path));
		if (error != 0) {
			fprintf(stderr, "ssh-keygen: cannot find the home directory, use -f\n");
			return error;
		}
		path = default_path;
	}

	/* Names the file that keeps the old contents. */
	error = keygen_join(old_path, sizeof(old_path), path, KEYGEN_OLD_SUFFIX);
	if (error != 0) {
		keygen_report(path, error);
		return error;
	}

	/* Prepares the buffers for the file and for what is kept of it. */
	ssh_buf_init(&text, SSH_HOSTFILE_MAX, 0);
	ssh_buf_init(&kept, SSH_HOSTFILE_MAX, 0);
	removed = 0;

	/* Reads the known_hosts file. */
	error = keygen_read_file(path, &text, SSH_HOSTFILE_MAX);
	if (error != 0) {
		keygen_report(path, error);
		goto done;
	}

	/* Makes the text without the lines of the host. */
	content = "";
	if (text.size != 0)
		content = (const char *)text.data;
	error = ssh_known_hosts_remove(content, text.size, host, port, &kept, &removed);
	if (error != 0) {
		fprintf(stderr, "ssh-keygen: cannot remove %s: %s\n", options->remove_host, strerror(error));
		goto done;
	}

	/* Leaves everything as it is when no line names the host. */
	if (removed == 0) {
		if (!options->quiet)
			printf("Host %s not found in %s\n", options->remove_host, path);
		goto done;
	}

	/* Takes the permission bits of the file to give them to the new one. */
	mode = KEYGEN_PUBLIC_MODE;
	status = stat(path, &information);
	if (status == 0)
		mode = information.st_mode & 0777;

	/* Keeps the old contents first, so the file is replaced only when they are safe. */
	error = keygen_write_file(old_path, text.data, text.size, mode, 1);
	if (error != 0) {
		keygen_report(old_path, error);
		goto done;
	}

	/* Puts the new contents in place of the file. */
	error = keygen_write_file(path, kept.data, kept.size, mode, 1);
	if (error != 0) {
		keygen_report(path, error);
		goto done;
	}

	/* Tells how many lines went and where the old contents are. */
	if (!options->quiet) {
		printf("Removed %u line(s) of %s from %s\n", removed, options->remove_host, path);
		printf("Original contents retained as %s\n", old_path);
	}

done:
	/* Frees the buffers. */
	ssh_buf_free(&text);
	ssh_buf_free(&kept);

	/* Reports why the file was not rewritten. */
	if (error != 0)
		return error;

	/* Succeeded: the host was removed, or no line named it. */
	return 0;
}
