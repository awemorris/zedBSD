/*
 * Exec target preparation helpers
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/exec.h"
#include "kern/cred.h"
#include "kern/kmem.h"
#include "kern/mount.h"

#include <errno.h>
#include <string.h>
#include <zedbsd/process.h>

static int
exec_bounded_length(const char *string, size_t maximum, size_t *result)
{
	size_t length;

	if (string == NULL || result == NULL)
		return EINVAL;
	for (length = 0; length < maximum && string[length] != '\0'; length++)
		;
	if (length == maximum)
		return E2BIG;
	*result = length + 1U;
	return 0;
}

int
exec_shebang_parse(const void *contents, size_t size, int at_eof,
	struct exec_shebang *result)
{
	const unsigned char *bytes = contents;
	size_t begin, end, path_end, argument_begin, argument_end, i;

	if ((contents == NULL && size != 0) || result == NULL)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	if (size < 2U || bytes[0] != '#' || bytes[1] != '!')
		return 0;
	for (end = 2U; end < size && bytes[end] != '\n'; end++) {
		if (bytes[end] == '\0' ||
		    (bytes[end] < 0x20U && bytes[end] != '\t' &&
		    bytes[end] != '\r'))
			return -ENOEXEC;
	}
	if (end == size && !at_eof)
		return -E2BIG;
	if (end > EXEC_SHEBANG_LINE_MAX)
		return -E2BIG;
	if (end > 2U && bytes[end - 1U] == '\r')
		end--;
	for (begin = 2U; begin < end &&
	    (bytes[begin] == ' ' || bytes[begin] == '\t'); begin++)
		;
	if (begin == end || bytes[begin] != '/')
		return -ENOEXEC;
	for (path_end = begin; path_end < end && bytes[path_end] != ' ' &&
	    bytes[path_end] != '\t'; path_end++) {
		if (bytes[path_end] == '\r')
			return -ENOEXEC;
	}
	if (path_end - begin >= sizeof(result->interpreter))
		return -ENAMETOOLONG;
	memcpy(result->interpreter, bytes + begin, path_end - begin);
	result->interpreter[path_end - begin] = '\0';
	for (argument_begin = path_end; argument_begin < end &&
	    (bytes[argument_begin] == ' ' || bytes[argument_begin] == '\t');
	    argument_begin++)
		;
	argument_end = end;
	while (argument_end > argument_begin &&
	    (bytes[argument_end - 1U] == ' ' ||
	    bytes[argument_end - 1U] == '\t'))
		argument_end--;
	for (i = argument_begin; i < argument_end; i++)
		if (bytes[i] == '\r')
			return -ENOEXEC;
	if (argument_end != argument_begin) {
		size_t length = argument_end - argument_begin;
		if (length >= sizeof(result->optional_argument))
			return -E2BIG;
		memcpy(result->optional_argument, bytes + argument_begin, length);
		result->optional_argument[length] = '\0';
		result->has_optional_argument = 1;
	}
	return 1;
}

int
exec_script_argv_build(char *const old_argv[],
	const struct exec_shebang *shebang, const char *script_path,
	char ***result)
{
	char **vector;
	char *cursor;
	size_t old_count = 0, new_count, table_bytes, string_bytes = 0;
	size_t length;
	unsigned index = 0;
	int error;

	if (old_argv == NULL || old_argv[0] == NULL || shebang == NULL ||
	    shebang->interpreter[0] == '\0' || script_path == NULL ||
	    script_path[0] == '\0' || result == NULL)
		return EINVAL;
	while (old_count < ZEDBSD_EXEC_VECTOR_MAX && old_argv[old_count] != NULL)
		old_count++;
	/* Do not probe beyond a caller-supplied vector merely to distinguish an
	 * exactly-full unterminated array from an oversized one. */
	if (old_count == ZEDBSD_EXEC_VECTOR_MAX)
		return E2BIG;
	new_count = old_count + 1U + shebang->has_optional_argument;
	if (new_count > ZEDBSD_EXEC_VECTOR_MAX)
		return E2BIG;
	table_bytes = (new_count + 1U) * sizeof(*vector);
	if (table_bytes > ZEDBSD_ARG_MAX)
		return E2BIG;
#define ADD_STRING(value) do { \
	error = exec_bounded_length((value), ZEDBSD_ARG_MAX - string_bytes, \
	    &length); \
	if (error != 0 || length > ZEDBSD_ARG_MAX - table_bytes - string_bytes) \
		return E2BIG; \
	string_bytes += length; \
} while (0)
	ADD_STRING(shebang->interpreter);
	if (shebang->has_optional_argument)
		ADD_STRING(shebang->optional_argument);
	ADD_STRING(script_path);
	for (old_count = 1U; old_argv[old_count] != NULL; old_count++)
		ADD_STRING(old_argv[old_count]);
#undef ADD_STRING
	vector = kern_calloc(1, table_bytes + string_bytes);
	if (vector == NULL)
		return ENOMEM;
	cursor = (char *)(vector + new_count + 1U);
#define COPY_STRING(value) do { \
	size_t copy_length = strlen(value) + 1U; \
	vector[index++] = cursor; \
	memcpy(cursor, (value), copy_length); \
	cursor += copy_length; \
} while (0)
	COPY_STRING(shebang->interpreter);
	if (shebang->has_optional_argument)
		COPY_STRING(shebang->optional_argument);
	COPY_STRING(script_path);
	for (old_count = 1U; old_argv[old_count] != NULL; old_count++)
		COPY_STRING(old_argv[old_count]);
#undef COPY_STRING
	vector[index] = NULL;
	*result = vector;
	return 0;
}

void
exec_script_argv_free(char **argv)
{
	kern_free(argv);
}

void
exec_credential_prepare(struct ucred *credential, const struct stat *status,
	unsigned mount_flags, int script, unsigned *secure)
{
	if (credential == NULL)
		return;
	if (!script && status != NULL && (mount_flags & MOUNT_NOSUID) == 0) {
		if ((status->st_mode & S_ISUID) != 0)
			credential->euid = status->st_uid;
		if ((status->st_mode & S_ISGID) != 0)
			credential->egid = status->st_gid;
	}
	credential->suid = credential->euid;
	credential->sgid = credential->egid;
	if (secure != NULL)
		*secure = credential->ruid != credential->euid ||
		    credential->rgid != credential->egid;
}
