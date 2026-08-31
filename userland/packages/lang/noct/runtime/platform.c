/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD package platform component.
 */

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

static int get_console(void);
static int screen_clear(void *context);
static int screen_clear_row(void *context, unsigned row);
static int screen_put_utf8(void *context, unsigned row, unsigned column, const char *text, unsigned length, uint8_t attribute);
static int screen_put(void *context, unsigned row, unsigned column, const char *text, uint8_t attribute);
static int screen_clear_to_eol(void *context, unsigned row, unsigned column);
static int screen_set_cursor(void *context, unsigned row, unsigned column);
static int screen_show_cursor(void *context, int visible);
static int keyboard_event(unsigned long command);
static int keyboard_poll(void *context);
static int keyboard_read(void *context);
static int clock_second(void *context);
static int file_size(void *context, const char *path, uint32_t *size);
static int file_read_at(void *context, const char *path, uint32_t offset, void *buffer, uint32_t length);
static int directory_read(void *context, const char *path, unsigned index, struct noct_dirent *entry);

/*
 * Implements the user noct services operation.
 */
const struct noct_services *
user_noct_services(
	void)
{
	/* Returns the computed result. */
	return &services;
}

/* Supports the get console operation. */
static int
get_console(
	void)
{
	/* Handles the console fd condition. */
	if (console_fd < 0)
		console_fd = open("/dev/console", O_RDWR);

	/* Returns the computed result. */
	return console_fd;
}

/* Supports the screen clear operation. */
static int
screen_clear(
	void *context)
{
	int function_result;

	(void)context;

	/* Computes the function result. */
	function_result = get_console() >= 0 &&
	       ioctl(console_fd, ZEDBSD_CONSOLE_CLEAR) == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the screen clear row operation. */
static int
screen_clear_row(
	void *context,
	unsigned row)
{
	int function_result;
	struct console_row request = {row};

	(void)context;

	/* Computes the function result. */
	function_result = get_console() >= 0 &&
	       ioctl(console_fd, ZEDBSD_CONSOLE_CLEAR_ROW, &request) == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the screen put utf8 operation. */
static int
screen_put_utf8(
	void *context,
	unsigned row,
	unsigned column,
	const char *text,
	unsigned length,
	uint8_t attribute)
{
	int function_result;
	struct console_write_at request;

	(void)context;
	request.row = row;
	request.column = column;
	request.attribute = attribute;
	request.address = (uapi_ptr_t)(uintptr_t)text;
	request.length = length;

	/* Computes the function result. */
	function_result = get_console() >= 0 &&
	       ioctl(console_fd, ZEDBSD_CONSOLE_WRITE_AT, &request) == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the screen put operation. */
static int
screen_put(
	void *context,
	unsigned row,
	unsigned column,
	const char *text,
	uint8_t attribute)
{
	int function_result;

	/* Obtains the screen put utf8 result. */
	function_result = screen_put_utf8(context, row, column, text,
			       (unsigned)strlen(text), attribute);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the screen clear to eol operation. */
static int
screen_clear_to_eol(
	void *context,
	unsigned row,
	unsigned column)
{
	int function_result;
	struct console_position request = {row, column};

	(void)context;

	/* Computes the function result. */
	function_result = get_console() >= 0 &&
	       ioctl(console_fd, ZEDBSD_CONSOLE_CLEAR_TO_EOL, &request) == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the screen set cursor operation. */
static int
screen_set_cursor(
	void *context,
	unsigned row,
	unsigned column)
{
	int function_result;
	struct console_cursor request = {row, column, 1};

	(void)context;

	/* Computes the function result. */
	function_result = get_console() >= 0 &&
	       ioctl(console_fd, ZEDBSD_CONSOLE_SET_CURSOR, &request) == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the screen show cursor operation. */
static int
screen_show_cursor(
	void *context,
	int visible)
{
	int function_result;
	struct console_cursor request = {0, 0, visible != 0};

	(void)context;

	/* Computes the function result. */
	function_result = get_console() >= 0 &&
	       ioctl(console_fd, ZEDBSD_CONSOLE_SHOW_CURSOR, &request) == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the keyboard event operation. */
static int
keyboard_event(
	unsigned long command)
{
	struct console_event event;

	/* Handles a failed get console operation. */
	if (get_console() < 0 || ioctl(console_fd, command, &event) != 0)
		return -1;

	/* Returns the computed result. */
	return (int)event.value;
}

/* Supports the keyboard poll operation. */
static int
keyboard_poll(
	void *context)
{
	int function_result;

	(void)context;

	/* Obtains the keyboard event result. */
	function_result = keyboard_event(ZEDBSD_CONSOLE_POLL_EVENT);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the keyboard read operation. */
static int
keyboard_read(
	void *context)
{
	int function_result;

	(void)context;

	/* Obtains the keyboard event result. */
	function_result = keyboard_event(ZEDBSD_CONSOLE_READ_EVENT);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the clock second operation. */
static int
clock_second(
	void *context)
{
	int function_result;
	struct timespec now;

	(void)context;

	/* Computes the function result. */
	function_result = clock_gettime(CLOCK_MONOTONIC, &now) == 0
		   ? (int)(now.tv_sec % 60)
		   : -1;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the file size operation. */
static int
file_size(
	void *context,
	const char *path,
	uint32_t *size)
{
	struct stat status;

	(void)context;

	/* Handles a failed stat operation. */
	if (size == NULL || stat(path, &status) != 0 || status.st_size < 0)
		return 0;
	*size = (uint32_t)status.st_size;
	/* Returns the computed result. */
	return (off_t)*size == status.st_size;
}

/* Supports the file read at operation. */
static int
file_read_at(
	void *context,
	const char *path,
	uint32_t offset,
	void *buffer,
	uint32_t length)
{
	int fd;
	ssize_t count;

	(void)context;
	fd = open(path, O_RDONLY);

	/* Checks the file descriptor. */
	if (fd < 0)
		return 0;

	/* Handles a failed lseek operation. */
	if (lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset) {
		(void)close(fd);

		/* Reports successful completion. */
		return 0;
	}
	count = read(fd, buffer, length);
	(void)close(fd);

	/* Returns the computed result. */
	return count == (ssize_t)length;
}

/* Supports the directory read operation. */
static int
directory_read(
	void *context,
	const char *path,
	unsigned index,
	struct noct_dirent *entry)
{
	char *name_for;
	DIR *directory;
	struct dirent *item;
	struct stat status;
	char child[512];
	unsigned i;

	item = NULL;
	(void)context;

	/* Handles a failed opendir operation. */
	if (entry == NULL || (directory = opendir(path)) == NULL)
		return -1;

	/* Process each remaining element. */
	for (i = 0; i <= index; i++) {
		item = readdir(directory);

		/* Handles the item availability. */
		if (item == NULL)
			break;
	}

	/* Handles the item availability. */
	if (item != NULL) {
		strncpy(entry->name, item->d_name, sizeof(entry->name) - 1U);
		entry->name[sizeof(entry->name) - 1U] = '\0';

		/*
 * FAT 8.3 directory entries are conventionally returned in
		 * upper case, while zedBSD paths are case-insensitive and the
		 * user-facing namespace is rooted directly at /.  Present one
		 * stable, lower-case spelling so POSIX clients can perform
		 * case-sensitive completion on the names they typed. */
		/* Process each element required by the operation. */
		for (name_for = entry->name_for; *name_for != '\0'; name_for++) {
			/* Handles the name for condition. */
			if (*name_for >= 'A' && *name_for <= 'Z')
				*name_for = (char)(*name_for - 'A' + 'a');
		}

		/* Selects the matching value. */
		if (!strcmp(path, "/"))
			snprintf(child, sizeof(child), "/%s", item->d_name);
		else
			snprintf(child, sizeof(child), "%s/%s", path,
				 item->d_name);

		/* Handles a failed stat operation. */
		if (stat(child, &status) == 0 && status.st_size >= 0)
			entry->size = (uint64_t)status.st_size;
		else
			entry->size = 0;
		entry->attributes = item->d_type == DT_DIR ? 0x10U : 0x20U;
	}
	(void)closedir(directory);

	/* Returns the computed result. */
	return item == NULL ? 0 : 1;
}
