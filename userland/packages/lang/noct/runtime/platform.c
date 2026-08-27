/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/packages/lang/noct/runtime/zedbsd-api.h"

#include <zedbsd/console.h>
#include <zedbsd/dirent.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int console_fd = -1;

static int
get_console(void)
{
	if (console_fd < 0)
		console_fd = open("/dev/console", O_RDWR);
	return console_fd;
}

static int
screen_clear(void *context)
{
	(void)context;
	return get_console() >= 0 &&
	       ioctl(console_fd, ZEDBSD_CONSOLE_CLEAR) == 0;
}
static int
screen_clear_row(void *context, unsigned row)
{
	struct console_row request = {row};
	(void)context;
	return get_console() >= 0 &&
	       ioctl(console_fd, ZEDBSD_CONSOLE_CLEAR_ROW, &request) == 0;
}
static int
screen_put_utf8(void *context, unsigned row, unsigned column, const char *text,
		unsigned length, uint8_t attribute)
{
	struct console_write_at request;
	(void)context;
	request.row = row;
	request.column = column;
	request.attribute = attribute;
	request.address = (uapi_ptr_t)(uintptr_t)text;
	request.length = length;
	return get_console() >= 0 &&
	       ioctl(console_fd, ZEDBSD_CONSOLE_WRITE_AT, &request) == 0;
}
static int
screen_put(void *context, unsigned row, unsigned column, const char *text,
	   uint8_t attribute)
{
	return screen_put_utf8(context, row, column, text,
			       (unsigned)strlen(text), attribute);
}
static int
screen_clear_to_eol(void *context, unsigned row, unsigned column)
{
	struct console_position request = {row, column};
	(void)context;
	return get_console() >= 0 &&
	       ioctl(console_fd, ZEDBSD_CONSOLE_CLEAR_TO_EOL, &request) == 0;
}
static int
screen_set_cursor(void *context, unsigned row, unsigned column)
{
	struct console_cursor request = {row, column, 1};
	(void)context;
	return get_console() >= 0 &&
	       ioctl(console_fd, ZEDBSD_CONSOLE_SET_CURSOR, &request) == 0;
}
static int
screen_show_cursor(void *context, int visible)
{
	struct console_cursor request = {0, 0, visible != 0};
	(void)context;
	return get_console() >= 0 &&
	       ioctl(console_fd, ZEDBSD_CONSOLE_SHOW_CURSOR, &request) == 0;
}
static int
keyboard_event(unsigned long command)
{
	struct console_event event;
	if (get_console() < 0 || ioctl(console_fd, command, &event) != 0)
		return -1;
	return (int)event.value;
}
static int
keyboard_poll(void *context)
{
	(void)context;
	return keyboard_event(ZEDBSD_CONSOLE_POLL_EVENT);
}
static int
keyboard_read(void *context)
{
	(void)context;
	return keyboard_event(ZEDBSD_CONSOLE_READ_EVENT);
}
static int
clock_second(void *context)
{
	struct timespec now;
	(void)context;
	return clock_gettime(CLOCK_MONOTONIC, &now) == 0
		   ? (int)(now.tv_sec % 60)
		   : -1;
}
static int
file_size(void *context, const char *path, uint32_t *size)
{
	struct stat status;
	(void)context;
	if (size == NULL || stat(path, &status) != 0 || status.st_size < 0)
		return 0;
	*size = (uint32_t)status.st_size;
	return (off_t)*size == status.st_size;
}
static int
file_read_at(void *context, const char *path, uint32_t offset, void *buffer,
	     uint32_t length)
{
	int fd;
	ssize_t count;
	(void)context;
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	if (lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset) {
		(void)close(fd);
		return 0;
	}
	count = read(fd, buffer, length);
	(void)close(fd);
	return count == (ssize_t)length;
}
static int
directory_read(void *context, const char *path, unsigned index,
	       struct noct_dirent *entry)
{
	DIR *directory;
	struct dirent *item = NULL;
	struct stat status;
	char child[512];
	unsigned i;
	(void)context;
	if (entry == NULL || (directory = opendir(path)) == NULL)
		return -1;
	for (i = 0; i <= index; i++) {
		item = readdir(directory);
		if (item == NULL)
			break;
	}
	if (item != NULL) {
		strncpy(entry->name, item->d_name, sizeof(entry->name) - 1U);
		entry->name[sizeof(entry->name) - 1U] = '\0';
		/* FAT 8.3 directory entries are conventionally returned in
		 * upper case, while zedBSD paths are case-insensitive and the
		 * user-facing namespace is rooted directly at /.  Present one
		 * stable, lower-case spelling so POSIX clients can perform
		 * case-sensitive completion on the names they typed. */
		for (char *name = entry->name; *name != '\0'; name++)
			if (*name >= 'A' && *name <= 'Z')
				*name = (char)(*name - 'A' + 'a');
		if (!strcmp(path, "/"))
			snprintf(child, sizeof(child), "/%s", item->d_name);
		else
			snprintf(child, sizeof(child), "%s/%s", path,
				 item->d_name);
		if (stat(child, &status) == 0 && status.st_size >= 0)
			entry->size = (uint64_t)status.st_size;
		else
			entry->size = 0;
		entry->attributes = item->d_type == DT_DIR ? 0x10U : 0x20U;
	}
	(void)closedir(directory);
	return item == NULL ? 0 : 1;
}

static const struct noct_services services = {
    NULL,
    NULL,
    screen_clear,
    screen_clear_row,
    screen_put,
    screen_put_utf8,
    screen_clear_to_eol,
    screen_set_cursor,
    screen_show_cursor,
    keyboard_poll,
    keyboard_read,
    clock_second,
    file_size,
    file_read_at,
    directory_read,
};

const struct noct_services *
user_noct_services(void)
{
	return &services;
}
