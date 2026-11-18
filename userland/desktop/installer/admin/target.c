/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The installed system's settings (target.h).  The base unit carries one
 * default user, uid 1000, with a known password, and root with another:
 * neither may be left as it is, so the user is replaced by the plan's
 * (renamed when the names differ, with its group of gid 1000, its
 * memberships and its home), and both hashes are made anew and checked
 * to differ from the ones the unit had.
 */

#include "target.h"

#include "userland/base/common/account.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Marks an argument a function must take but does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/* The longest path component the helper opens under the root. */
#define TARGET_COMPONENT_MAX	128U

/* The suffix of the file a replacement is written to before its rename. */
#define TARGET_TEMPORARY	".install-admin"

/* The room an edit may add to the text it reads. */
#define TARGET_EDIT_SLACK	1024U

/* One line of a text, without its newline, and the bytes it takes with it. */
struct target_line {
	const char *text;
	size_t length;
	size_t piece;
};

static int target_next_line(const char *text, size_t length, size_t *offset, struct target_line *line);
static int target_field(const struct target_line *line, unsigned index, const char **field, size_t *field_length);
static int target_field_is(const char *field, size_t field_length, const char *word);
static int target_put(struct target_edit *edit, const char *bytes, size_t count);
static int target_puts(struct target_edit *edit, const char *text);
static int target_put_members(struct target_edit *edit, const char *members, size_t length, const char *old_name, const char *name);
static int target_sshd_keyword(const struct target_line *line);
static int target_open_parent(int root, const char *path, int *directory, char *leaf, size_t capacity);
static int target_write_all(int fd, const char *text, size_t length);
static int target_edit_file(int root, const char *path, int (*change)(struct target_edit *, const void *), const void *argument);
static int target_change_hostname(struct target_edit *edit, const void *argument);
static int target_change_sshd(struct target_edit *edit, const void *argument);
static int target_change_fstab(struct target_edit *edit, const void *argument);
static int target_shadow_passwords(const struct install_plan *plan, const char *old_name, struct target_edit *edit, char *spare, size_t spare_capacity);
static int target_home_rename(int root, const char *old_name, const char *name);
static int target_autologin(int root, const struct install_plan *plan);

/*
 * Replaces the default user's line of /etc/passwd (the one of uid 1000)
 * by the plan's user: its name, display name and home, the password
 * field, the ids and the shell kept.  The old name goes to old_name.
 * Returns 0, ENOENT when no line has uid 1000, EINVAL for two of them or
 * one without its seven fields, EEXIST when another account already has
 * the name, ERANGE when a result does not fit.
 */
int
target_passwd_user(
	struct target_edit *edit,
	const char *name,
	const char *fullname,
	char *old_name,
	size_t old_capacity)
{
	struct target_line line;
	const char *field[7];
	size_t field_length[7];
	size_t offset;
	unsigned i;
	int found;
	int is_user;
	int named;
	int error;

	/* Each line, copied but for the default user's. */
	found = 0;
	offset = 0;
	edit->written = 0;
	for (;;) {
		error = target_next_line(edit->text, edit->length, &offset, &line);
		if (error == ENOENT)
			break;

		/* The name, and the uid when the line has one. */
		error = target_field(&line, 0, &field[0], &field_length[0]);
		if (error != 0)
			return error;
		is_user = 0;
		error = target_field(&line, 2, &field[2], &field_length[2]);
		if (error == 0)
			is_user = target_field_is(field[2], field_length[2], "1000");

		/* Another account: copied, unless it has the new name. */
		if (!is_user) {
			named = target_field_is(field[0], field_length[0], name);
			if (named)
				return EEXIST;
			error = target_put(edit, line.text, line.piece);
			if (error != 0)
				return error;
			continue;
		}

		/* The default user, once, with all of its fields. */
		if (found)
			return EINVAL;
		for (i = 0; i < 7U; i++) {
			error = target_field(&line, i, &field[i], &field_length[i]);
			if (error != 0)
				return EINVAL;
		}

		/* Its name, which the shadow, the group and the home go by. */
		if (field_length[0] + 1U > old_capacity)
			return ERANGE;
		memcpy(old_name, field[0], field_length[0]);
		old_name[field_length[0]] = '\0';

		/* The new line: name:password:uid:gid:fullname:/home/name:shell. */
		error = target_puts(edit, name);
		if (error == 0)
			error = target_put(edit, field[1] - 1, field_length[1] + 1U);
		if (error == 0)
			error = target_put(edit, field[2] - 1, field_length[2] + 1U);
		if (error == 0)
			error = target_put(edit, field[3] - 1, field_length[3] + 2U);
		if (error == 0)
			error = target_puts(edit, fullname);
		if (error == 0)
			error = target_puts(edit, ":/home/");
		if (error == 0)
			error = target_puts(edit, name);
		if (error == 0)
			error = target_put(edit, field[6] - 1, field_length[6] + 1U);
		if (error == 0)
			error = target_puts(edit, "\n");
		if (error != 0)
			return error;
		found = 1;
	}

	/* No default user to replace. */
	if (!found)
		return ENOENT;

	/* Succeeded: the new text. */
	return 0;
}

/*
 * Renames the record whose first field is old_name (a line of
 * /etc/shadow) to name, copying every other byte.  Returns 0, ENOENT when
 * no line has the old name, EEXIST when another line has the new one,
 * ERANGE when the result does not fit.
 */
int
target_record_rename(
	struct target_edit *edit,
	const char *old_name,
	const char *name)
{
	struct target_line line;
	const char *field;
	size_t field_length;
	size_t offset;
	int found;
	int same;
	int named;
	int old_record;
	int error;

	/* Each line, copied but for its name. */
	same = strcmp(old_name, name);
	found = 0;
	offset = 0;
	edit->written = 0;
	for (;;) {
		error = target_next_line(edit->text, edit->length, &offset, &line);
		if (error == ENOENT)
			break;
		error = target_field(&line, 0, &field, &field_length);
		if (error != 0)
			return error;

		/* The record renamed: the new name, then the line from its first colon. */
		old_record = 0;
		if (!found && field_length < line.length)
			old_record = target_field_is(field, field_length, old_name);
		if (old_record) {
			error = target_puts(edit, name);
			if (error == 0)
				error = target_put(edit, line.text + field_length, line.piece - field_length);
			if (error != 0)
				return error;
			found = 1;
			continue;
		}

		/* Another record with the new name. */
		named = 0;
		if (same != 0)
			named = target_field_is(field, field_length, name);
		if (named)
			return EEXIST;

		/* Copied as it is. */
		error = target_put(edit, line.text, line.piece);
		if (error != 0)
			return error;
	}

	/* No record to rename. */
	if (!found)
		return ENOENT;

	/* Succeeded: the new text. */
	return 0;
}

/*
 * Renames the default user's group (gid 1000, named after the user) and
 * the user in every group's member list (wheel and network among them) to
 * name.  Returns 0, ENOENT when no group of gid 1000 has the old name,
 * EEXIST when another group already has the new one, ERANGE when the
 * result does not fit.
 */
int
target_group_user(
	struct target_edit *edit,
	const char *old_name,
	const char *name)
{
	struct target_line line;
	const char *field[4];
	size_t field_length[4];
	size_t offset;
	unsigned i;
	int found;
	int complete;
	int own;
	int same;
	int old_group;
	int named;
	int error;

	/* Each line. */
	same = strcmp(old_name, name);
	found = 0;
	offset = 0;
	edit->written = 0;
	for (;;) {
		error = target_next_line(edit->text, edit->length, &offset, &line);
		if (error == ENOENT)
			break;

		/* A group's four fields; a line without them is copied. */
		complete = 1;
		for (i = 0; i < 4U; i++) {
			error = target_field(&line, i, &field[i], &field_length[i]);
			if (error != 0)
				complete = 0;
		}

		/* Copies a line without the four fields unchanged. */
		if (!complete) {
			error = target_put(edit, line.text, line.piece);
			if (error != 0)
				return error;
			continue;
		}

		/* The user's own group, or another with the new name. */
		own = 0;
		old_group = target_field_is(field[2], field_length[2], "1000");
		if (old_group)
			old_group = target_field_is(field[0], field_length[0], old_name);
		if (old_group)
			own = 1;
		named = 0;
		if (!own && same != 0)
			named = target_field_is(field[0], field_length[0], name);
		if (named)
			return EEXIST;

		/* The name, renamed for the user's own group. */
		if (own) {
			error = target_puts(edit, name);
			found = 1;
		} else {
			error = target_put(edit, field[0], field_length[0]);
		}

		/* Reports a name that could not be written. */
		if (error != 0)
			return error;

		/* The password and the gid, then the members with the user renamed. */
		error = target_put(edit, field[1] - 1, (size_t)(field[3] - field[1]) + 1U);
		if (error == 0)
			error = target_put_members(edit, field[3], field_length[3], old_name, name);
		if (error == 0)
			error = target_puts(edit, "\n");
		if (error != 0)
			return error;
	}

	/* No group of the default user. */
	if (!found)
		return ENOENT;

	/* Succeeded: the new text. */
	return 0;
}

/*
 * Sets the hostname of /etc/rc.conf: every line starting "hostname:"
 * becomes "hostname: NAME".  Returns 0, ENOENT when no line sets it,
 * EINVAL for a name that is not one, ERANGE when the result does not fit.
 */
int
target_hostname(
	struct target_edit *edit,
	const char *hostname)
{
	struct target_line line;
	size_t offset;
	int found;
	int differs;
	int valid;
	int error;

	/* A name /etc/rc.conf may hold as it is. */
	valid = plan_hostname_valid(hostname);
	if (!valid)
		return EINVAL;

	/* Each line, the hostname's replaced. */
	found = 0;
	offset = 0;
	edit->written = 0;
	for (;;) {
		error = target_next_line(edit->text, edit->length, &offset, &line);
		if (error == ENOENT)
			break;

		/* Another line, copied. */
		differs = 1;
		if (line.length >= 9U)
			differs = memcmp(line.text, "hostname:", 9U);
		if (differs != 0) {
			error = target_put(edit, line.text, line.piece);
			if (error != 0)
				return error;
			continue;
		}

		/* The hostname's line. */
		error = target_puts(edit, "hostname: ");
		if (error == 0)
			error = target_puts(edit, hostname);
		if (error == 0)
			error = target_puts(edit, "\n");
		if (error != 0)
			return error;
		found = 1;
	}

	/* No hostname to set. */
	if (!found)
		return ENOENT;

	/* Succeeded: the new text. */
	return 0;
}

/*
 * Keeps sshd from taking root's logins: every PermitRootLogin line that
 * is not a comment becomes "PermitRootLogin no", and a configuration
 * without one gets it first (sshd keeps the first value it reads, so a
 * Match block cannot come before it).  Returns 0 or ERANGE.
 */
int
target_sshd_no_root(
	struct target_edit *edit)
{
	struct target_line line;
	size_t offset;
	int found;
	int keyword;
	int error;

	/* Whether a line sets it already. */
	found = 0;
	offset = 0;
	for (;;) {
		error = target_next_line(edit->text, edit->length, &offset, &line);
		if (error == ENOENT)
			break;
		keyword = target_sshd_keyword(&line);
		if (keyword)
			found = 1;
	}

	/* Without one, the setting goes first. */
	edit->written = 0;
	if (!found) {
		error = target_puts(edit, "PermitRootLogin no\n");
		if (error != 0)
			return error;
	}

	/* Each line, the setting's replaced. */
	offset = 0;
	for (;;) {
		error = target_next_line(edit->text, edit->length, &offset, &line);
		if (error == ENOENT)
			break;
		keyword = target_sshd_keyword(&line);
		if (keyword)
			error = target_puts(edit, "PermitRootLogin no\n");
		else
			error = target_put(edit, line.text, line.piece);
		if (error != 0)
			return error;
	}

	/* Succeeded: the new text. */
	return 0;
}

/*
 * Adds the swap file's line to /etc/fstab, unless a line that is not a
 * comment names /swapfile already.  Returns 0 or ERANGE.
 */
int
target_fstab_swapfile(
	struct target_edit *edit)
{
	struct target_line line;
	size_t offset;
	size_t skip;
	size_t word;
	int found;
	int differs;
	int error;

	/* Each line, copied, noting one of the swap file. */
	found = 0;
	offset = 0;
	edit->written = 0;
	for (;;) {
		error = target_next_line(edit->text, edit->length, &offset, &line);
		if (error == ENOENT)
			break;
		error = target_put(edit, line.text, line.piece);
		if (error != 0)
			return error;

		/* The line's first word, after its blanks. */
		skip = 0;
		while (skip < line.length && (line.text[skip] == ' ' || line.text[skip] == '\t'))
			skip++;
		word = 0;
		while (skip + word < line.length && line.text[skip + word] != ' ' && line.text[skip + word] != '\t')
			word++;

		/* The swap file's own line. */
		if (word == 9U) {
			differs = memcmp(line.text + skip, "/swapfile", 9U);
			if (differs == 0)
				found = 1;
		}
	}

	/* Already there. */
	if (found)
		return 0;

	/* A last line without its newline gets one before the new line. */
	if (edit->written != 0U && edit->output[edit->written - 1U] != '\n') {
		error = target_puts(edit, "\n");
		if (error != 0)
			return error;
	}

	/* The swap file's line. */
	error = target_puts(edit, TARGET_SWAPFILE_LINE);
	if (error != 0)
		return error;

	/* Succeeded: the new text. */
	return 0;
}

/*
 * Copies a user's hash (the second field of its line of /etc/shadow).
 * Returns 0, ENOENT when no line has the name, EINVAL for a line without
 * the field, ERANGE when it does not fit.
 */
int
target_shadow_hash(
	const char *text,
	size_t length,
	const char *name,
	char *hash,
	size_t capacity)
{
	struct target_line line;
	const char *field;
	size_t field_length;
	size_t offset;
	int named;
	int error;

	/* Each line, until the user's. */
	offset = 0;
	for (;;) {
		error = target_next_line(text, length, &offset, &line);
		if (error == ENOENT)
			return ENOENT;
		error = target_field(&line, 0, &field, &field_length);
		if (error != 0)
			return error;
		named = target_field_is(field, field_length, name);
		if (named)
			break;
	}

	/* The hash. */
	error = target_field(&line, 1, &field, &field_length);
	if (error != 0)
		return EINVAL;
	if (field_length + 1U > capacity)
		return ERANGE;
	memcpy(hash, field, field_length);
	hash[field_length] = '\0';

	/* Succeeded: the hash is copied. */
	return 0;
}

/*
 * Reads a whole file of the installed root (a regular file, reached
 * without following a symbolic link).  Returns 0 with its length, an
 * errno value from the open or the read, EINVAL for one that is not a
 * regular file, EFBIG for one larger than the capacity.
 */
int
target_file_read(
	int root,
	const char *path,
	char *text,
	size_t capacity,
	size_t *length)
{
	char leaf[TARGET_COMPONENT_MAX];
	struct stat status;
	size_t used;
	ssize_t count;
	int directory;
	int regular;
	int fd;
	int error;

	/* The file, in its directory. */
	error = target_open_parent(root, path, &directory, leaf, sizeof(leaf));
	if (error != 0)
		return error;
	fd = openat(directory, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	error = errno;
	close(directory);
	if (fd < 0)
		return error;

	/* A regular file that fits. */
	error = fstat(fd, &status);
	if (error != 0) {
		error = errno;
		close(fd);
		return error;
	}

	/* Refuses anything but a regular file. */
	regular = S_ISREG(status.st_mode);
	if (!regular) {
		close(fd);
		return EINVAL;
	}

	/* Refuses a file larger than the room given. */
	if ((uint64_t)status.st_size > capacity) {
		close(fd);
		return EFBIG;
	}

	/* Its bytes, to the end. */
	used = 0;
	for (;;) {
		count = read(fd, text + used, capacity - used);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			error = errno;
			close(fd);
			return error;
		}

		/* Ends the read at the end of the file. */
		if (count == 0)
			break;
		used += (size_t)count;
		if (used == capacity) {
			close(fd);
			return EFBIG;
		}
	}

	/* Releases the file once its text is read. */
	close(fd);

	/* Succeeded: the file's text. */
	*length = used;
	return 0;
}

/*
 * Replaces a file of the installed root: the new text is written to a
 * file beside it with the old one's mode and owner, synced and renamed
 * over it, and the directory is synced.  The file must be there (a
 * regular file).  Returns 0 or an errno value; a failure leaves the old
 * file as it was.
 */
int
target_file_replace(
	int root,
	const char *path,
	const char *text,
	size_t length)
{
	char leaf[TARGET_COMPONENT_MAX];
	char temporary[TARGET_COMPONENT_MAX + sizeof(TARGET_TEMPORARY)];
	struct stat status;
	int directory;
	int regular;
	int reported;
	int fd;
	int error;

	/* The file's directory, and the file as it is. */
	error = target_open_parent(root, path, &directory, leaf, sizeof(leaf));
	if (error != 0)
		return error;
	error = fstatat(directory, leaf, &status, AT_SYMLINK_NOFOLLOW);
	if (error != 0) {
		error = errno;
		close(directory);
		return error;
	}

	/* Refuses anything but a regular file. */
	regular = S_ISREG(status.st_mode);
	if (!regular) {
		close(directory);
		return EINVAL;
	}

	/* The new file beside it, a stale one of an earlier try removed first. */
	snprintf(temporary, sizeof(temporary), "%s%s", leaf, TARGET_TEMPORARY);
	(void)unlinkat(directory, temporary, 0);
	fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		error = errno;
		close(directory);
		return error;
	}

	/* The text, the old file's owner and mode, on the disk. */
	error = target_write_all(fd, text, length);
	if (error == 0) {
		reported = fchown(fd, status.st_uid, status.st_gid);
		if (reported != 0)
			error = errno;
	}

	/* Gives the new file the old file's mode. */
	if (error == 0) {
		reported = fchmod(fd, status.st_mode & 07777);
		if (reported != 0)
			error = errno;
	}

	/* Puts the new file's bytes on the disk. */
	if (error == 0) {
		reported = fsync(fd);
		if (reported != 0)
			error = errno;
	}

	/* Closes the new file, and reports a close that failed. */
	reported = close(fd);
	if (reported != 0 && error == 0)
		error = errno;
	if (error != 0) {
		(void)unlinkat(directory, temporary, 0);
		close(directory);
		return error;
	}

	/* The new file in the old one's place, and the directory on the disk. */
	error = renameat(directory, temporary, directory, leaf);
	if (error != 0) {
		error = errno;
		(void)unlinkat(directory, temporary, 0);
		close(directory);
		return error;
	}

	/* Puts the directory entry of the new file on the disk. */
	error = fsync(directory);
	if (error != 0)
		error = errno;
	close(directory);
	if (error != 0)
		return error;

	/* Succeeded: the file is replaced. */
	return 0;
}

/*
 * Makes the plan's user the installed system's: /etc/passwd, /etc/shadow
 * and /etc/group edited together (all three made before any is written),
 * both passwords hashed anew and checked to differ from the base unit's,
 * the home renamed with the user, and /etc/keiland/autologin set when the
 * desktop is installed.  Returns 0 or an errno value (EEXIST for a name
 * another account or group has, ENOENT for an automatic login without
 * the desktop).
 */
int
target_accounts(
	int root,
	const struct install_plan *plan)
{
	char old_name[PLAN_USER_MAX + 1U];
	struct target_edit passwd;
	struct target_edit shadow;
	struct target_edit group;
	char *buffer[7];
	size_t length;
	unsigned i;
	int error;

	/* The buffers: each file's text and its edit, and one spare for the shadow's two passes. */
	for (i = 0; i < 7U; i++)
		buffer[i] = NULL;
	error = 0;
	for (i = 0; i < 7U; i++) {
		buffer[i] = malloc(TARGET_FILE_MAX + TARGET_EDIT_SLACK);
		if (buffer[i] == NULL) {
			error = ENOMEM;
			goto done;
		}
	}

	/* The user's line of /etc/passwd, and the name it had. */
	error = target_file_read(root, TARGET_PASSWD, buffer[0], TARGET_FILE_MAX, &length);
	if (error != 0)
		goto done;
	passwd.text = buffer[0];
	passwd.length = length;
	passwd.output = buffer[1];
	passwd.capacity = TARGET_FILE_MAX + TARGET_EDIT_SLACK;
	error = target_passwd_user(&passwd, plan->user, plan->fullname, old_name, sizeof(old_name));
	if (error != 0)
		goto done;

	/* The user's group and memberships. */
	error = target_file_read(root, TARGET_GROUP, buffer[2], TARGET_FILE_MAX, &length);
	if (error != 0)
		goto done;
	group.text = buffer[2];
	group.length = length;
	group.output = buffer[3];
	group.capacity = TARGET_FILE_MAX + TARGET_EDIT_SLACK;
	error = target_group_user(&group, old_name, plan->user);
	if (error != 0)
		goto done;

	/* The shadow: the user renamed, both hashes made anew. */
	error = target_file_read(root, TARGET_SHADOW, buffer[4], TARGET_FILE_MAX, &length);
	if (error != 0)
		goto done;
	shadow.text = buffer[4];
	shadow.length = length;
	shadow.output = buffer[5];
	shadow.capacity = TARGET_FILE_MAX + TARGET_EDIT_SLACK;
	error = target_shadow_passwords(plan, old_name, &shadow, buffer[6], TARGET_FILE_MAX + TARGET_EDIT_SLACK);
	if (error != 0)
		goto done;

	/* The three files, the shadow first so no new name is ever without its own hash. */
	error = target_file_replace(root, TARGET_SHADOW, shadow.output, shadow.written);
	if (error != 0)
		goto done;
	error = target_file_replace(root, TARGET_GROUP, group.output, group.written);
	if (error != 0)
		goto done;
	error = target_file_replace(root, TARGET_PASSWD, passwd.output, passwd.written);
	if (error != 0)
		goto done;

	/* The home, and the automatic login. */
	error = target_home_rename(root, old_name, plan->user);
	if (error != 0)
		goto done;
	error = target_autologin(root, plan);

done:
	for (i = 0; i < 7U; i++)
		free(buffer[i]);

	/* Reports why the accounts could not be set. */
	if (error != 0)
		return error;

	/* Succeeded: the installed system is the plan's user's. */
	return 0;
}

/* Sets the installed system's hostname in /etc/rc.conf. */
int
target_hostname_set(
	int root,
	const char *hostname)
{
	int error;

	/* The file, edited. */
	error = target_edit_file(root, TARGET_RC_CONF, target_change_hostname, hostname);
	if (error != 0)
		return error;

	/* Succeeded: the hostname is set. */
	return 0;
}

/*
 * Keeps the installed sshd from root's logins.  Without OpenSSH (no
 * /etc/ssh/sshd_config) there is nothing to do.
 */
int
target_sshd_set(
	int root)
{
	int error;

	/* The file, edited when it is there. */
	error = target_edit_file(root, TARGET_SSHD_CONFIG, target_change_sshd, NULL);
	if (error == ENOENT)
		return 0;
	if (error != 0)
		return error;

	/* Succeeded: sshd refuses root. */
	return 0;
}

/* Adds the swap file to the installed /etc/fstab. */
int
target_fstab_set(
	int root)
{
	int error;

	/* The file, edited. */
	error = target_edit_file(root, TARGET_FSTAB, target_change_fstab, NULL);
	if (error != 0)
		return error;

	/* Succeeded: init turns the swap file on at boot. */
	return 0;
}

/*
 * Takes the next line of a text.  Returns ENOENT at the end of the text.
 * The last line need not end with a newline.
 */
static int
target_next_line(
	const char *text,
	size_t length,
	size_t *offset,
	struct target_line *line)
{
	const char *end;

	/* The end of the text. */
	if (*offset >= length)
		return ENOENT;

	/* The line, up to its newline or the end. */
	line->text = text + *offset;
	end = memchr(line->text, '\n', length - *offset);
	if (end == NULL) {
		line->length = length - *offset;
		line->piece = line->length;
	} else {
		line->length = (size_t)(end - line->text);
		line->piece = line->length + 1U;
	}

	/* Succeeded: the line, the offset past it. */
	*offset += line->piece;
	return 0;
}

/*
 * Finds a field of a colon-separated line (the last runs to the line's
 * end).  Returns ENOENT for a line with fewer fields.
 */
static int
target_field(
	const struct target_line *line,
	unsigned index,
	const char **field,
	size_t *field_length)
{
	const char *start;
	const char *colon;
	const char *end;
	unsigned i;

	/* Past the colons before the field. */
	start = line->text;
	end = line->text + line->length;
	for (i = 0; i < index; i++) {
		colon = memchr(start, ':', (size_t)(end - start));
		if (colon == NULL)
			return ENOENT;
		start = colon + 1;
	}

	/* The field, up to the next colon or the end. */
	colon = memchr(start, ':', (size_t)(end - start));
	if (colon == NULL)
		colon = end;

	/* Succeeded: the field. */
	*field = start;
	*field_length = (size_t)(colon - start);
	return 0;
}

/* Reports whether a field is the word given. */
static int
target_field_is(
	const char *field,
	size_t field_length,
	const char *word)
{
	size_t length;
	int differs;

	/* Another length. */
	length = strlen(word);
	if (length != field_length)
		return 0;

	/* The bytes. */
	differs = memcmp(field, word, length);
	if (differs != 0)
		return 0;

	/* The same. */
	return 1;
}

/* Appends bytes to an edit's output. */
static int
target_put(
	struct target_edit *edit,
	const char *bytes,
	size_t count)
{
	/* The output's room. */
	if (count > edit->capacity - edit->written)
		return ERANGE;

	/* Succeeded: the bytes are added. */
	memcpy(edit->output + edit->written, bytes, count);
	edit->written += count;
	return 0;
}

/* Appends a string to an edit's output. */
static int
target_puts(
	struct target_edit *edit,
	const char *text)
{
	int error;

	/* The string's bytes. */
	error = target_put(edit, text, strlen(text));
	if (error != 0)
		return error;

	/* Succeeded: the string is added. */
	return 0;
}

/* Appends a group's member list with the old name replaced by the new one. */
static int
target_put_members(
	struct target_edit *edit,
	const char *members,
	size_t length,
	const char *old_name,
	const char *name)
{
	const char *member;
	const char *comma;
	const char *end;
	size_t member_length;
	int is_old;
	int error;

	/* An empty list. */
	if (length == 0U)
		return 0;

	/* Each member, separated by commas. */
	member = members;
	end = members + length;
	for (;;) {
		comma = memchr(member, ',', (size_t)(end - member));
		if (comma == NULL)
			comma = end;
		member_length = (size_t)(comma - member);

		/* The member, renamed when it is the user. */
		is_old = target_field_is(member, member_length, old_name);
		if (is_old)
			error = target_puts(edit, name);
		else
			error = target_put(edit, member, member_length);
		if (error != 0)
			return error;

		/* The last member. */
		if (comma == end)
			break;

		/* The comma between two. */
		error = target_puts(edit, ",");
		if (error != 0)
			return error;
		member = comma + 1;
	}

	/* Succeeded: the list is added. */
	return 0;
}

/*
 * Reports whether a line of sshd_config sets PermitRootLogin (the keyword
 * in any case after blanks, followed by a blank or '=').
 */
static int
target_sshd_keyword(
	const struct target_line *line)
{
	size_t skip;
	int differs;
	char after;

	/* The blanks before the keyword. */
	skip = 0;
	while (skip < line->length && (line->text[skip] == ' ' || line->text[skip] == '\t'))
		skip++;

	/* The keyword, and what follows it. */
	if (line->length - skip <= 15U)
		return 0;
	differs = strncasecmp(line->text + skip, "PermitRootLogin", 15U);
	if (differs != 0)
		return 0;
	after = line->text[skip + 15U];
	if (after != ' ' && after != '\t' && after != '=')
		return 0;

	/* The line sets it. */
	return 1;
}

/*
 * Opens the directory a path of the root names its last component in,
 * one component at a time without following a symbolic link, and copies
 * that last component.  Returns 0 with the directory's descriptor (the
 * caller closes it), EINVAL for a path with an empty, "." or ".."
 * component or none before its last, ENAMETOOLONG for a component too
 * long, or the open's errno value.
 */
static int
target_open_parent(
	int root,
	const char *path,
	int *directory,
	char *leaf,
	size_t capacity)
{
	char component[TARGET_COMPONENT_MAX];
	const char *start;
	const char *slash;
	size_t length;
	int current;
	int next;
	int dot;
	int dotdot;
	int error;

	/* Each component before the last, opened under the one before. */
	current = root;
	start = path;
	for (;;) {
		slash = strchr(start, '/');
		if (slash == NULL)
			length = strlen(start);
		else
			length = (size_t)(slash - start);

		/* A component the walk may take. */
		if (length == 0U || length >= sizeof(component) || length >= capacity) {
			error = EINVAL;
			if (length != 0U)
				error = ENAMETOOLONG;
			break;
		}

		/* Copies the component out of the path, and refuses "." and "..". */
		memcpy(component, start, length);
		component[length] = '\0';
		dot = strcmp(component, ".");
		dotdot = strcmp(component, "..");
		if (dot == 0 || dotdot == 0) {
			error = EINVAL;
			break;
		}

		/* The last component, under a directory opened here. */
		if (slash == NULL) {
			if (current == root) {
				error = EINVAL;
				break;
			}

			/* Hands back the last component as the leaf. */
			memcpy(leaf, component, length + 1U);
			*directory = current;
			return 0;
		}

		/* The next directory. */
		next = openat(current, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		error = errno;
		if (current != root)
			close(current);
		if (next < 0)
			return error;
		current = next;
		start = slash + 1;
	}

	/* Reports a path the walk does not take. */
	if (current != root)
		close(current);
	return error;
}

/* Writes all of a text to a descriptor. */
static int
target_write_all(
	int fd,
	const char *text,
	size_t length)
{
	size_t done;
	ssize_t count;

	/* Each write, until the text is written. */
	done = 0;
	while (done < length) {
		count = write(fd, text + done, length - done);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			return errno;
		if (count == 0)
			return EIO;
		done += (size_t)count;
	}

	/* Succeeded: the text is written. */
	return 0;
}

/* Reads a file of the root, changes its text and replaces it. */
static int
target_edit_file(
	int root,
	const char *path,
	int (*change)(struct target_edit *, const void *),
	const void *argument)
{
	struct target_edit edit;
	char *text;
	char *output;
	size_t length;
	int error;

	/* The text and the room for its change. */
	text = malloc(TARGET_FILE_MAX);
	if (text == NULL)
		return ENOMEM;
	output = malloc(TARGET_FILE_MAX + TARGET_EDIT_SLACK);
	if (output == NULL) {
		free(text);
		return ENOMEM;
	}

	/* The file, changed and written back. */
	error = target_file_read(root, path, text, TARGET_FILE_MAX, &length);
	if (error == 0) {
		edit.text = text;
		edit.length = length;
		edit.output = output;
		edit.capacity = TARGET_FILE_MAX + TARGET_EDIT_SLACK;
		edit.written = 0;
		error = change(&edit, argument);
	}

	/* Replaces the file with the edited text. */
	if (error == 0)
		error = target_file_replace(root, path, edit.output, edit.written);
	free(output);
	free(text);

	/* Reports why the file could not be changed. */
	if (error != 0)
		return error;

	/* Succeeded: the file is changed. */
	return 0;
}

/* Changes /etc/rc.conf's hostname (the argument). */
static int
target_change_hostname(
	struct target_edit *edit,
	const void *argument)
{
	int error;

	/* The hostname's line. */
	error = target_hostname(edit, argument);
	if (error != 0)
		return error;

	/* Succeeded: the text is changed. */
	return 0;
}

/* Changes sshd_config's PermitRootLogin. */
static int
target_change_sshd(
	struct target_edit *edit,
	const void *argument)
{
	int error;

	UNUSED_PARAMETER(argument);

	/* The setting. */
	error = target_sshd_no_root(edit);
	if (error != 0)
		return error;

	/* Succeeded: the text is changed. */
	return 0;
}

/* Adds the swap file to /etc/fstab. */
static int
target_change_fstab(
	struct target_edit *edit,
	const void *argument)
{
	int error;

	UNUSED_PARAMETER(argument);

	/* The swap file's line. */
	error = target_fstab_swapfile(edit);
	if (error != 0)
		return error;

	/* Succeeded: the text is changed. */
	return 0;
}

/*
 * Makes the shadow's new text: the default user renamed to the plan's,
 * its hash and root's made from the plan's passwords, each checked to
 * differ from the hash the base unit had.  The edit's output holds the
 * result; the spare buffer holds the text between the passes.
 */
static int
target_shadow_passwords(
	const struct install_plan *plan,
	const char *old_name,
	struct target_edit *edit,
	char *spare,
	size_t spare_capacity)
{
	char old_user_hash[ACCOUNT_HASH_MAX];
	char old_root_hash[ACCOUNT_HASH_MAX];
	char hash[ACCOUNT_HASH_MAX];
	struct target_edit renamed;
	size_t written;
	long day;
	int same;
	int error;

	/* The hashes the base unit had. */
	error = target_shadow_hash(edit->text, edit->length, old_name, old_user_hash, sizeof(old_user_hash));
	if (error != 0)
		return error;
	error = target_shadow_hash(edit->text, edit->length, "root", old_root_hash, sizeof(old_root_hash));
	if (error != 0)
		return error;

	/* The user renamed, into the spare buffer. */
	renamed.text = edit->text;
	renamed.length = edit->length;
	renamed.output = spare;
	renamed.capacity = spare_capacity;
	renamed.written = 0;
	error = target_record_rename(&renamed, old_name, plan->user);
	if (error != 0)
		return error;

	/* The user's new hash, into the edit's output. */
	day = (long)(time(NULL) / 86400);
	error = account_password_hash(plan->password, hash, sizeof(hash));
	if (error != 0)
		return error;
	same = strcmp(hash, old_user_hash);
	if (same == 0)
		return EIO;
	error = account_shadow_replace(spare, renamed.written, plan->user, hash, day, edit->output, edit->capacity, &written);
	if (error != 0)
		return error;

	/* Root's new hash, back into the spare buffer and then the output. */
	error = account_password_hash(plan->root_password, hash, sizeof(hash));
	if (error != 0)
		return error;
	same = strcmp(hash, old_root_hash);
	if (same == 0)
		return EIO;
	error = account_shadow_replace(edit->output, written, "root", hash, day, spare, spare_capacity, &written);
	if (error != 0)
		return error;
	if (written > edit->capacity)
		return ERANGE;
	memcpy(edit->output, spare, written);
	memset(hash, 0, sizeof(hash));

	/* Succeeded: the shadow's new text. */
	edit->written = written;
	return 0;
}

/*
 * Renames the default user's home to the plan's user's (a home the base
 * unit does not carry is made at boot).  Returns 0, EEXIST when the new
 * home is there already, or the rename's errno value.
 */
static int
target_home_rename(
	int root,
	const char *old_name,
	const char *name)
{
	char old_path[PLAN_USER_MAX + 8U];
	char new_path[PLAN_USER_MAX + 8U];
	char leaf[TARGET_COMPONENT_MAX];
	struct stat status;
	int directory;
	int same;
	int error;

	/* The same name keeps its home. */
	same = strcmp(old_name, name);
	if (same == 0)
		return 0;

	/* The home directory, under /home. */
	snprintf(old_path, sizeof(old_path), "home/%s", old_name);
	snprintf(new_path, sizeof(new_path), "home/%s", name);
	error = target_open_parent(root, old_path, &directory, leaf, sizeof(leaf));
	if (error == ENOENT)
		return 0;
	if (error != 0)
		return error;

	/* No old home, or a new one already. */
	error = fstatat(directory, old_name, &status, AT_SYMLINK_NOFOLLOW);
	if (error != 0) {
		error = errno;
		close(directory);
		if (error == ENOENT)
			return 0;
		return error;
	}

	/* Refuses a new home that exists already. */
	error = fstatat(directory, name, &status, AT_SYMLINK_NOFOLLOW);
	if (error == 0) {
		close(directory);
		return EEXIST;
	}

	/* The rename; the uid stays 1000, so the files stay the user's. */
	error = renameat(directory, old_name, directory, name);
	if (error != 0) {
		error = errno;
		close(directory);
		return error;
	}

	/* Releases the directory once the rename is done. */
	close(directory);

	/* Succeeded: the home has the new name. */
	return 0;
}

/*
 * Sets /etc/keiland/autologin to the user's name (or empties it): the
 * file is the desktop unit's, so without the desktop there is none, and
 * an automatic login is then refused with ENOENT.
 */
static int
target_autologin(
	int root,
	const struct install_plan *plan)
{
	char line[PLAN_USER_MAX + 2U];
	size_t length;
	int error;

	/* The name and its newline, or nothing. */
	length = 0;
	line[0] = '\0';
	if (plan->autologin)
		length = (size_t)snprintf(line, sizeof(line), "%s\n", plan->user);

	/* The file, replaced. */
	error = target_file_replace(root, TARGET_AUTOLOGIN, line, length);
	if (error == ENOENT && !plan->autologin)
		return 0;
	if (error != 0)
		return error;

	/* Succeeded: the login at boot is the plan's. */
	return 0;
}
