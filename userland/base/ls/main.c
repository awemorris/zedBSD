/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LS_PATH_CAPACITY 1024U
#define LS_RECURSION_LIMIT 64

struct options {
	int all, directory, classify, human, long_format, recursive, reverse,
	    time_sort, one, columns;
};
struct entry {
	char *name;
	struct stat status;
	int status_valid;
};

static char *
copy_string(const char *text)
{
	size_t n = strlen(text) + 1U;
	char *p = malloc(n);
	if (p)
		memcpy(p, text, n);
	return p;
}
static int
join_path(const char *directory, const char *name, char *out, size_t cap)
{
	size_t a = strlen(directory), b = strlen(name);
	int slash = a && directory[a - 1] != '/';
	if (a + (size_t)slash + b + 1U > cap) {
		errno = ENAMETOOLONG;
		return 0;
	}
	memcpy(out, directory, a);
	if (slash)
		out[a++] = '/';
	memcpy(out + a, name, b + 1U);
	return 1;
}
static char
type_char(mode_t m)
{
	if (S_ISDIR(m))
		return 'd';
	if (S_ISCHR(m))
		return 'c';
	if (S_ISBLK(m))
		return 'b';
	if (S_ISFIFO(m))
		return 'p';
	if (S_ISLNK(m))
		return 'l';
	if (S_ISSOCK(m))
		return 's';
	return '-';
}
static void
mode_text(mode_t m, char out[11])
{
	static const mode_t bits[] = {S_IRUSR, S_IWUSR, S_IXUSR,
				      S_IRGRP, S_IWGRP, S_IXGRP,
				      S_IROTH, S_IWOTH, S_IXOTH};
	static const char letters[] = "rwx";
	unsigned i;
	out[0] = type_char(m);
	for (i = 0; i < 9; i++)
		out[i + 1] = (m & bits[i]) ? letters[i % 3] : '-';
	if (m & S_ISUID)
		out[3] = (m & S_IXUSR) ? 's' : 'S';
	if (m & S_ISGID)
		out[6] = (m & S_IXGRP) ? 's' : 'S';
	if (m & S_ISVTX)
		out[9] = (m & S_IXOTH) ? 't' : 'T';
	out[10] = '\0';
}
static char
suffix(mode_t mode)
{
	if (S_ISDIR(mode))
		return '/';
	if (S_ISLNK(mode))
		return '@';
	if (S_ISFIFO(mode))
		return '|';
	if (S_ISSOCK(mode))
		return '=';
	if (mode & (S_IXUSR | S_IXGRP | S_IXOTH))
		return '*';
	return '\0';
}
static int
compare(const struct entry *a, const struct entry *b, const struct options *o)
{
	int value;
	if (o->time_sort && a->status_valid && b->status_valid) {
		if (a->status.st_mtime > b->status.st_mtime)
			value = -1;
		else if (a->status.st_mtime < b->status.st_mtime)
			value = 1;
		else
			value = strcmp(a->name, b->name);
	} else
		value = strcmp(a->name, b->name);
	return o->reverse ? -value : value;
}
static void
sort_entries(struct entry *items, size_t count, const struct options *o)
{
	size_t i;
	for (i = 1; i < count; i++) {
		struct entry value = items[i];
		size_t at = i;
		while (at && compare(&items[at - 1U], &value, o) > 0) {
			items[at] = items[at - 1U];
			at--;
		}
		items[at] = value;
	}
}
static int
load(const char *path, const struct options *o, struct entry **result,
     size_t *result_count)
{
	DIR *d = opendir(path);
	struct dirent *de;
	struct entry *items = NULL;
	size_t count = 0, capacity = 0;
	static const char *const dots[] = {".", ".."};
	unsigned dot = 0;
	if (!d)
		return 0;
	for (;;) {
		const char *name;
		char child[LS_PATH_CAPACITY];
		struct entry *larger;
		if (o->all && dot < 2U)
			name = dots[dot++];
		else {
			de = readdir(d);
			if (!de)
				break;
			name = de->d_name;
			if (!o->all && name[0] == '.')
				continue;
			if (o->all &&
			    (!strcmp(name, ".") || !strcmp(name, "..")))
				continue;
		}
		if (count == capacity) {
			capacity = capacity ? capacity * 2U : 16U;
			larger = realloc(items, capacity * sizeof(*items));
			if (!larger)
				goto failed;
			items = larger;
		}
		items[count].name = copy_string(name);
		if (!items[count].name)
			goto failed;
		items[count].status_valid =
		    join_path(path, name, child, sizeof(child)) &&
		    lstat(child, &items[count].status) == 0;
		count++;
	}
	if (closedir(d)) {
		d = NULL;
		goto failed;
	}
	sort_entries(items, count, o);
	*result = items;
	*result_count = count;
	return 1;
failed: {
	int saved = errno;
	size_t i;
	if (d)
		closedir(d);
	for (i = 0; i < count; i++)
		free(items[i].name);
	free(items);
	errno = saved;
	return 0;
}
}
static void
free_entries(struct entry *items, size_t count)
{
	size_t i;
	for (i = 0; i < count; i++)
		free(items[i].name);
	free(items);
}
static void
print_name(const struct entry *item, const struct options *o)
{
	printf("%s", item->name);
	if (o->classify && item->status_valid) {
		char mark = suffix(item->status.st_mode);
		if (mark)
			putchar(mark);
	}
}
static void
human_size(off_t value, char out[16])
{
	static const char suffixes[] = "BKMGTPE";
	unsigned unit = 0;
	unsigned long long scale = 1, magnitude;
	if (value < 0) {
		snprintf(out, 16, "%lld", (long long)value);
		return;
	}
	magnitude = (unsigned long long)value;
	while (unit + 1U < sizeof(suffixes) - 1U &&
	       magnitude >= scale * 1024ULL) {
		scale *= 1024ULL;
		unit++;
	}
	if (unit == 0) {
		snprintf(out, 16, "%llu", magnitude);
		return;
	}
	{
		unsigned long long whole = magnitude / scale,
				   remainder = magnitude % scale;
		if (whole < 10U) {
			unsigned long long tenth =
			    (remainder * 10ULL + scale / 2ULL) / scale;
			if (tenth == 10U) {
				whole++;
				tenth = 0;
			}
			snprintf(out, 16, "%llu.%llu%c", whole, tenth,
				 suffixes[unit]);
		} else {
			whole = (magnitude + scale / 2ULL) / scale;
			snprintf(out, 16, "%llu%c", whole, suffixes[unit]);
		}
	}
}
static int
leap(long long y)
{
	return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
}
static void
ls_time(time_t value, char out[32])
{
	static const int md[] = {31, 28, 31, 30, 31, 30,
				 31, 31, 30, 31, 30, 31};
	static const char *mn[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
				   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	long long days = value / 86400, sec = value % 86400, y = 1970;
	time_t now = time(NULL);
	int m = 0;
	if (sec < 0) {
		sec += 86400;
		days--;
	}
	while (days >= 365 + leap(y)) {
		days -= 365 + leap(y);
		y++;
	}
	while (days < 0) {
		y--;
		days += 365 + leap(y);
	}
	while (m < 11 && days >= md[m] + (m == 1 && leap(y))) {
		days -= md[m] + (m == 1 && leap(y));
		m++;
	}
	if (now != (time_t)-1 && (value < now - 15552000 || value > now + 3600))
		snprintf(out, 32, "%s %2d  %4lld", mn[m], (int)days + 1, y);
	else
		snprintf(out, 32, "%s %2d %02lld:%02lld", mn[m], (int)days + 1,
			 sec / 3600, (sec / 60) % 60);
}
static const char *
uid_name(uid_t id, char out[24])
{
	struct passwd p, *r = NULL;
	char b[512];
	if (getpwuid_r(id, &p, b, sizeof(b), &r) == 0 && r && r->pw_name) {
		snprintf(out, 24, "%s", r->pw_name);
		return out;
	}
	snprintf(out, 24, "%u", (unsigned)id);
	return out;
}
static const char *
gid_name(gid_t id, char out[24])
{
	struct group g, *r = NULL;
	char b[512];
	if (getgrgid_r(id, &g, b, sizeof(b), &r) == 0 && r && r->gr_name) {
		snprintf(out, 24, "%s", r->gr_name);
		return out;
	}
	snprintf(out, 24, "%u", (unsigned)id);
	return out;
}
struct long_widths {
	size_t links, user, group, size;
};
static void
measure_long(const struct entry *items, size_t count, const struct options *o,
	     struct long_widths *w)
{
	size_t i;
	memset(w, 0, sizeof(*w));
	for (i = 0; i < count; i++) {
		char links[24], size[32], ub[24], gb[24];
		const char *u, *g;
		if (!items[i].status_valid)
			continue;
		snprintf(links, sizeof(links), "%lu",
			 (unsigned long)items[i].status.st_nlink);
		if (o->human)
			human_size(items[i].status.st_size, size);
		else
			snprintf(size, sizeof(size), "%lld",
				 (long long)items[i].status.st_size);
		u = uid_name(items[i].status.st_uid, ub);
		g = gid_name(items[i].status.st_gid, gb);
		if (strlen(links) > w->links)
			w->links = strlen(links);
		if (strlen(u) > w->user)
			w->user = strlen(u);
		if (strlen(g) > w->group)
			w->group = strlen(g);
		if (strlen(size) > w->size)
			w->size = strlen(size);
	}
}
static int
print_long(const char *directory, const struct entry *item,
	   const struct options *o, const struct long_widths *w)
{
	char mode[11], path[LS_PATH_CAPACITY], size[32], when[32], ub[24],
	    gb[24];
	const char *user, *group;
	if (!item->status_valid) {
		command_error("ls", item->name);
		return 0;
	}
	mode_text(item->status.st_mode, mode);
	if (o->human)
		human_size(item->status.st_size, size);
	else
		snprintf(size, sizeof(size), "%lld",
			 (long long)item->status.st_size);
	ls_time(item->status.st_mtime, when);
	user = uid_name(item->status.st_uid, ub);
	group = gid_name(item->status.st_gid, gb);
	printf("%s %*lu %-*s %-*s %*s %s %s", mode, (int)w->links,
	       (unsigned long)item->status.st_nlink, (int)w->user, user,
	       (int)w->group, group, (int)w->size, size, when, item->name);
	if (S_ISLNK(item->status.st_mode) &&
	    join_path(directory, item->name, path, sizeof(path))) {
		char target[LS_PATH_CAPACITY];
		ssize_t n = readlink(path, target, sizeof(target) - 1U);
		if (n >= 0) {
			target[n] = '\0';
			printf(" -> %s", target);
		}
	}
	putchar('\n');
	return 1;
}
static int
print_entries(const char *path, struct entry *items, size_t count,
	      const struct options *o)
{
	size_t i;
	int ok = 1;
	if (o->long_format) {
		unsigned long long blocks = 0;
		char total[24];
		struct long_widths widths;
		measure_long(items, count, o, &widths);
		for (i = 0; i < count; i++)
			if (items[i].status_valid &&
			    items[i].status.st_blocks > 0)
				blocks += (unsigned long long)items[i]
					      .status.st_blocks;
		if (o->human)
			human_size((off_t)(blocks * 512ULL), total);
		else
			snprintf(total, sizeof(total), "%llu",
				 (blocks + 1ULL) / 2ULL);
		printf("total %s\n", total);
		for (i = 0; i < count; i++)
			if (!print_long(path, &items[i], o, &widths))
				ok = 0;
		return ok;
	}
	if (o->one || !o->columns) {
		for (i = 0; i < count; i++) {
			print_name(&items[i], o);
			putchar('\n');
		}
		return 1;
	}
	{
		size_t width = 1, columns, rows, row, column;
		for (i = 0; i < count; i++) {
			size_t n =
			    strlen(items[i].name) + (o->classify ? 1U : 0U);
			if (n > width)
				width = n;
		}
		width += 2U;
		columns = 80U / width;
		if (columns == 0)
			columns = 1;
		rows = (count + columns - 1U) / columns;
		for (row = 0; row < rows; row++) {
			for (column = 0; column < columns; column++) {
				size_t index = column * rows + row, n;
				if (index >= count)
					continue;
				print_name(&items[index], o);
				n = strlen(items[index].name) +
				    (o->classify && items[index].status_valid &&
					     suffix(items[index].status.st_mode)
					 ? 1U
					 : 0U);
				if (column + 1U < columns &&
				    index + rows < count)
					while (n++ < width)
						putchar(' ');
			}
			putchar('\n');
		}
		return 1;
	}
}
static int
list_directory(const char *path, const struct options *o, int header, int depth)
{
	struct entry *items;
	size_t count, i;
	int ok = 1;
	if (depth > LS_RECURSION_LIMIT) {
		errno = ELOOP;
		command_error("ls", path);
		return 0;
	}
	if (!load(path, o, &items, &count)) {
		command_error("ls", path);
		return 0;
	}
	if (header)
		printf("%s:\n", path);
	if (!print_entries(path, items, count, o))
		ok = 0;
	if (o->recursive) {
		for (i = 0; i < count; i++) {
			char child[LS_PATH_CAPACITY];
			if (!items[i].status_valid ||
			    !S_ISDIR(items[i].status.st_mode) ||
			    !strcmp(items[i].name, ".") ||
			    !strcmp(items[i].name, ".."))
				continue;
			if (!join_path(path, items[i].name, child,
				       sizeof(child))) {
				command_error("ls", items[i].name);
				ok = 0;
				continue;
			}
			putchar('\n');
			if (!list_directory(child, o, 1, depth + 1))
				ok = 0;
		}
	}
	free_entries(items, count);
	return ok;
}
static int
list_operand(const char *path, const struct options *o, int header)
{
	struct stat status;
	struct entry item;
	if (lstat(path, &status)) {
		command_error("ls", path);
		return 0;
	}
	if (S_ISDIR(status.st_mode) && !o->directory)
		return list_directory(path, o, header, 0);
	memset(&item, 0, sizeof(item));
	item.name = (char *)path;
	item.status = status;
	item.status_valid = 1;
	if (o->long_format) {
		struct long_widths widths;
		measure_long(&item, 1, o, &widths);
		return print_long("", &item, o, &widths);
	}
	print_name(&item, o);
	putchar('\n');
	return 1;
}
int
main(int argc, char **argv)
{
	struct options o = {0};
	int index = 1, failed = 0, operands;
	for (; index < argc && argv[index][0] == '-' && argv[index][1];
	     index++) {
		const char *p = argv[index] + 1;
		if (!strcmp(argv[index], "--")) {
			index++;
			break;
		}
		for (; *p; p++)
			switch (*p) {
			case 'a':
				o.all = 1;
				break;
			case 'd':
				o.directory = 1;
				break;
			case 'F':
				o.classify = 1;
				break;
			case 'h':
				o.human = 1;
				break;
			case 'l':
				o.long_format = 1;
				o.one = 1;
				break;
			case 'R':
				o.recursive = 1;
				break;
			case 'r':
				o.reverse = 1;
				break;
			case 't':
				o.time_sort = 1;
				break;
			case '1':
				o.one = 1;
				o.columns = 0;
				break;
			case 'C':
				o.columns = 1;
				o.one = 0;
				break;
			default:
				fprintf(stderr,
					"usage: ls [-adFhlRrt1C] [file...]\n");
				return 1;
			}
	}
	operands = argc - index;
	if (!operands)
		return list_operand(".", &o, 0) ? 0 : 1;
	for (; index < argc; index++) {
		if (!list_operand(argv[index], &o, operands > 1))
			failed = 1;
		if (index + 1 < argc)
			putchar('\n');
	}
	return failed;
}
