/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/archive.h"
#include "userland/base/common/elf_symbols.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *program = "ar";

static void
error_path(const char *path)
{
	fprintf(stderr, "%s: %s: %s\n", program, path, strerror(errno));
}

static int
selected(const struct archive_member *member, int argc, char **argv)
{
	if (!argc)
		return 1;
	for (int i = 0; i < argc; i++)
		if (!strcmp(member->name, archive_basename(argv[i])))
			return 1;
	return 0;
}

static void
member_release(struct archive_member *member)
{
	free(member->name);
	free(member->data);
	memset(member, 0, sizeof(*member));
}

static int
read_regular(const char *path, struct archive_member *member)
{
	struct stat st;
	size_t done = 0;
	int fd = open(path, O_RDONLY);

	memset(member, 0, sizeof(*member));
	if (fd < 0 || fstat(fd, &st))
		goto fail;
	if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
	    (uintmax_t)st.st_size > SIZE_MAX) {
		errno = EINVAL;
		goto fail;
	}
	member->name = strdup(archive_basename(path));
	member->data = malloc(st.st_size ? (size_t)st.st_size : 1);
	if (!member->name || !member->data)
		goto fail;
	while (done < (size_t)st.st_size) {
		ssize_t n =
		    read(fd, member->data + done, (size_t)st.st_size - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			if (!n)
				errno = EIO;
			goto fail;
		}
		done += (size_t)n;
	}
	close(fd);
	member->size = (size_t)st.st_size;
	member->mtime = (uint64_t)st.st_mtime;
	member->uid = st.st_uid;
	member->gid = st.st_gid;
	member->mode = st.st_mode & 07777;
	return 0;
fail: {
	int saved = errno;
	if (fd >= 0)
		close(fd);
	member_release(member);
	errno = saved;
	return -1;
}
}

static int
insert(struct archive_file *archive, size_t index,
       struct archive_member *member)
{
	struct archive_member *members;
	if (index > archive->count ||
	    archive->count == SIZE_MAX / sizeof(*members)) {
		errno = EOVERFLOW;
		return -1;
	}
	members =
	    realloc(archive->members, (archive->count + 1) * sizeof(*members));
	if (!members)
		return -1;
	archive->members = members;
	memmove(members + index + 1, members + index,
		(archive->count - index) * sizeof(*members));
	members[index] = *member;
	archive->count++;
	memset(member, 0, sizeof(*member));
	return 0;
}

static void
remove_at(struct archive_file *archive, size_t index)
{
	member_release(&archive->members[index]);
	memmove(archive->members + index, archive->members + index + 1,
		(archive->count - index - 1) * sizeof(*archive->members));
	archive->count--;
}

static size_t
position(const struct archive_file *archive, const char *name, int after)
{
	for (size_t i = 0; i < archive->count; i++)
		if (!archive->members[i].special &&
		    !strcmp(archive->members[i].name, name))
			return i + (after != 0);
	return archive->count;
}

static int
print_member(const struct archive_member *member)
{
	size_t done = 0;
	while (done < member->size) {
		ssize_t n = write(STDOUT_FILENO, member->data + done,
				  member->size - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		done += (size_t)n;
	}
	return 0;
}

static void
verbose_name(const struct archive_member *member)
{
	char modes[10] = "---------";
	static const unsigned bits[] = {0400, 0200, 0100, 0040, 0020,
					0010, 0004, 0002, 0001};
	static const char chars[] = "rwxrwxrwx";
	for (size_t i = 0; i < 9; i++)
		if (member->mode & bits[i])
			modes[i] = chars[i];
	printf("%s %u/%u %10zu %llu %s\n", modes, member->uid, member->gid,
	       member->size, (unsigned long long)member->mtime, member->name);
}

static int
extract_member(const struct archive_member *member, int verbose)
{
	const char *name = archive_basename(member->name);
	int fd;
	if (!*name || strcmp(name, member->name) || !strcmp(name, ".") ||
	    !strcmp(name, "..")) {
		errno = EINVAL;
		return -1;
	}
	fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, member->mode & 0777);
	if (fd < 0)
		return -1;
	if (fchmod(fd, member->mode & 07777)) {
		int saved = errno;
		(void)close(fd);
		errno = saved;
		return -1;
	}
	for (size_t done = 0; done < member->size;) {
		ssize_t count =
		    write(fd, member->data + done, member->size - done);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			int saved = count == 0 ? EIO : errno;

			(void)close(fd);
			errno = saved;
			return -1;
		}
		done += (size_t)count;
	}
	if (close(fd) != 0)
		return -1;
	if (verbose)
		printf("x - %s\n", name);
	return 0;
}

struct index_symbol {
	const char *name;
	size_t member;
};

static void
put_be32(unsigned char *destination, uint32_t value)
{
	destination[0] = (unsigned char)(value >> 24);
	destination[1] = (unsigned char)(value >> 16);
	destination[2] = (unsigned char)(value >> 8);
	destination[3] = (unsigned char)value;
}

static size_t
stored_member_size(const struct archive_member *member)
{
	size_t name_length = strlen(member->name);
	size_t stored = member->size;

	if (name_length > 15U || strchr(member->name, ' ') != NULL) {
		if (stored > SIZE_MAX - name_length)
			return SIZE_MAX;
		stored += name_length;
	}
	if (stored > SIZE_MAX - 60U - (stored & 1U))
		return SIZE_MAX;
	return 60U + stored + (stored & 1U);
}

static int
build_symbol_index(struct archive_file *archive)
{
	struct elf_symbol_table *tables;
	struct index_symbol *symbols = NULL;
	size_t symbol_count = 0;
	size_t names_size = 0;
	size_t data_size;
	size_t member_offset;
	struct archive_member index_member = {0};

	tables = calloc(archive->count ? archive->count : 1U, sizeof(*tables));
	if (tables == NULL)
		return -1;
	for (size_t member = 0; member < archive->count; member++) {
		if (elf_symbols_read(archive->members[member].data,
				     archive->members[member].size, 0,
				     &tables[member]) != 0) {
			if (errno == EINVAL || errno == ENOENT)
				continue;
			goto fail;
		}
		for (size_t item = 0; item < tables[member].count; item++) {
			struct elf_symbol_record *symbol =
			    &tables[member].symbols[item];
			struct index_symbol *grown;
			size_t name_length;

			if ((symbol->binding != 1U && symbol->binding != 2U) ||
			    symbol->section == 0U || symbol->name[0] == '\0')
				continue;
			name_length = strlen(symbol->name) + 1U;
			if (symbol_count == SIZE_MAX / sizeof(*symbols) ||
			    names_size > SIZE_MAX - name_length)
				goto overflow;
			grown = realloc(symbols,
					(symbol_count + 1U) * sizeof(*symbols));
			if (grown == NULL)
				goto fail;
			symbols = grown;
			symbols[symbol_count].name = symbol->name;
			symbols[symbol_count].member = member;
			symbol_count++;
			names_size += name_length;
		}
	}
	if (symbol_count > UINT32_MAX ||
	    symbol_count > (SIZE_MAX - 4U - names_size) / 4U)
		goto overflow;
	data_size = 4U + symbol_count * 4U + names_size;
	index_member.name = strdup("/");
	index_member.data = malloc(data_size ? data_size : 1U);
	if (index_member.name == NULL || index_member.data == NULL)
		goto fail;
	index_member.size = data_size;
	index_member.mode = 0;
	index_member.special = 1;
	put_be32(index_member.data, (uint32_t)symbol_count);
	member_offset = 8U + 60U + data_size + (data_size & 1U);
	for (size_t member = 0, item = 0; member < archive->count; member++) {
		size_t stored = stored_member_size(&archive->members[member]);

		if (stored == SIZE_MAX || member_offset > UINT32_MAX)
			goto overflow;
		while (item < symbol_count && symbols[item].member == member)
			put_be32(index_member.data + 4U + item++ * 4U,
				 (uint32_t)member_offset);
		if (member_offset > SIZE_MAX - stored)
			goto overflow;
		member_offset += stored;
	}
	{
		unsigned char *names =
		    index_member.data + 4U + symbol_count * 4U;

		for (size_t item = 0; item < symbol_count; item++) {
			size_t length = strlen(symbols[item].name) + 1U;

			(void)memcpy(names, symbols[item].name, length);
			names += length;
		}
	}
	if (insert(archive, 0, &index_member) != 0)
		goto fail;
	for (size_t member = 0; member + 1U < archive->count; member++)
		elf_symbols_free(&tables[member]);
	free(tables);
	free(symbols);
	return 0;

overflow:
	errno = EOVERFLOW;
fail:
	for (size_t member = 0; member < archive->count; member++)
		elf_symbols_free(&tables[member]);
	free(tables);
	free(symbols);
	member_release(&index_member);
	return -1;
}

static int
move_members(struct archive_file *archive, const char *anchor, int after,
	     int argc, char **argv, int verbose)
{
	struct archive_member *ordered;
	struct archive_member *moved;
	size_t kept_count = 0;
	size_t moved_count = 0;
	size_t target;

	ordered =
	    calloc(archive->count ? archive->count : 1U, sizeof(*ordered));
	moved = calloc(archive->count ? archive->count : 1U, sizeof(*moved));
	if (ordered == NULL || moved == NULL) {
		free(ordered);
		free(moved);
		return -1;
	}
	for (size_t index = 0; index < archive->count; index++) {
		if (selected(&archive->members[index], argc, argv)) {
			moved[moved_count++] = archive->members[index];
			if (verbose)
				printf("m - %s\n",
				       archive->members[index].name);
		} else
			ordered[kept_count++] = archive->members[index];
	}
	free(archive->members);
	archive->members = ordered;
	archive->count = kept_count;
	if (anchor != NULL) {
		target = position(archive, anchor, after);
		if (target == archive->count &&
		    (archive->count == 0U ||
		     strcmp(archive->members[archive->count - 1U].name,
			    anchor) != 0)) {
			errno = EINVAL;
			goto fail;
		}
	} else
		target = archive->count;
	memmove(ordered + target + moved_count, ordered + target,
		(kept_count - target) * sizeof(*ordered));
	memcpy(ordered + target, moved, moved_count * sizeof(*moved));
	archive->count += moved_count;
	free(moved);
	return moved_count != 0U;

fail:
	for (size_t index = 0; index < moved_count; index++)
		member_release(&moved[index]);
	free(moved);
	return -1;
}

static void
usage(void)
{
	fprintf(
	    stderr,
	    "usage: ar -[abciuVv] -[dmpqrstx] [position] archive [file ...]\n");
}

int
main(int argc, char **argv)
{
	struct archive_file archive = {0};
	char operation = 0;
	const char *position_name = NULL;
	const char *options;
	const char *archive_path;
	int after = 0, before = 0, create_silent = 0, update = 0, verbose = 0;
	int modify = 0, failed = 0;
	int argi = 2;

	if (argc < 3) {
		usage();
		return 2;
	}
	options = argv[1][0] == '-' ? argv[1] + 1 : argv[1];
	for (; *options; options++) {
		switch (*options) {
		case 'a':
			after = 1;
			break;
		case 'b':
		case 'i':
			before = 1;
			break;
		case 'c':
			create_silent = 1;
			break;
		case 'u':
			update = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'V':
			puts("zedBSD ar 1");
			return 0;
		case 's':
			if (!operation)
				operation = 's';
			break;
		case 'd':
		case 'm':
		case 'p':
		case 'q':
		case 'r':
		case 't':
		case 'x':
			if (operation && operation != 's') {
				usage();
				return 2;
			}
			operation = *options;
			break;
		default:
			usage();
			return 2;
		}
	}
	if (!operation ||
	    ((after || before) && operation != 'm' && operation != 'r')) {
		usage();
		return 2;
	}
	if (after || before) {
		if (argi >= argc) {
			usage();
			return 2;
		}
		position_name = archive_basename(argv[argi++]);
	}
	if (argi >= argc) {
		usage();
		return 2;
	}
	archive_path = argv[argi++];
	if (archive_read(archive_path, &archive)) {
		if (errno != ENOENT || (operation != 'q' && operation != 'r' &&
					operation != 's')) {
			error_path(archive_path);
			return 1;
		}
		memset(&archive, 0, sizeof(archive));
		if (!create_silent)
			fprintf(stderr, "%s: creating %s\n", program,
				archive_path);
	}

	if (operation == 't' || operation == 'p' || operation == 'x') {
		for (size_t i = 0; i < archive.count; i++) {
			struct archive_member *member = &archive.members[i];
			if (member->special ||
			    !selected(member, argc - argi, argv + argi))
				continue;
			if (operation == 't') {
				if (verbose)
					verbose_name(member);
				else
					puts(member->name);
			} else if (operation == 'p') {
				if (verbose)
					printf("\n<%s>\n\n", member->name);
				if (print_member(member)) {
					error_path(member->name);
					failed = 1;
				}
			} else if (extract_member(member, verbose)) {
				error_path(member->name);
				failed = 1;
			}
		}
		archive_free(&archive);
		return failed;
	}

	/* Symbol tables become stale after any mutation.  Drop them; the zedBSD
	 * static linker accepts archives without an index and ar -s remains an
	 * explicit, deterministic rewrite operation. */
	for (size_t i = 0; i < archive.count;)
		if (archive.members[i].special)
			remove_at(&archive, i);
		else
			i++;

	if (operation == 'd') {
		for (size_t i = 0; i < archive.count;)
			if (selected(&archive.members[i], argc - argi,
				     argv + argi)) {
				if (verbose)
					printf("d - %s\n",
					       archive.members[i].name);
				remove_at(&archive, i);
				modify = 1;
			} else
				i++;
	} else if (operation == 'm') {
		int moved = move_members(&archive, position_name, after,
					 argc - argi, argv + argi, verbose);

		if (moved < 0) {
			error_path(position_name ? position_name
						 : archive_path);
			failed = 1;
		} else if (moved)
			modify = 1;
	} else if (operation == 'q' || operation == 'r') {
		size_t target = position_name
				    ? position(&archive, position_name, after)
				    : archive.count;
		for (; argi < argc; argi++) {
			struct archive_member member;
			ssize_t found = -1;
			if (read_regular(argv[argi], &member)) {
				error_path(argv[argi]);
				failed = 1;
				continue;
			}
			if (operation == 'r')
				for (size_t i = 0; i < archive.count; i++)
					if (!strcmp(archive.members[i].name,
						    member.name)) {
						found = (ssize_t)i;
						break;
					}
			if (found >= 0 && update &&
			    archive.members[found].mtime >= member.mtime) {
				member_release(&member);
				continue;
			}
			if (found >= 0) {
				member_release(&archive.members[found]);
				archive.members[found] = member;
			} else if (insert(&archive, target++, &member)) {
				error_path(archive_path);
				member_release(&member);
				failed = 1;
				break;
			}
			if (verbose)
				printf("%c - %s\n", found >= 0 ? 'r' : 'a',
				       argv[argi]);
			modify = 1;
		}
	} else if (operation == 's') {
		modify = 1;
	}
	if (modify && build_symbol_index(&archive) != 0) {
		error_path(archive_path);
		failed = 1;
		modify = 0;
	}

	if ((modify || operation == 's') &&
	    archive_write_atomic(archive_path, &archive)) {
		error_path(archive_path);
		failed = 1;
	}
	archive_free(&archive);
	return failed;
}
