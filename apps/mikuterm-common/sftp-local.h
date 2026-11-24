/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Local filesystem interface used by sftp-worker and sftp-transfer.
 *
 * Implemented for Windows by apps/terminal-win32/sftp-local-win32.c and
 * for host tests by a POSIX layer.
 */

#ifndef MIKUTERM_SFTP_LOCAL_H
#define MIKUTERM_SFTP_LOCAL_H

#include "mikuterm.h"

#include <stddef.h>
#include <stdint.h>

/* Metadata for one local file or directory entry. */
struct sftp_local_entry {
	char name[MIKUTERM_PATH_MAX];
	int is_dir;
	int is_reparse;
	uint64_t size;
	uint32_t mtime;
};

/* Opaque handle for an open local file. */
struct sftp_local_file;

int sftp_local_downloads_dir(char *path, size_t size);
int sftp_local_stat(const char *path, struct sftp_local_entry *entry);
int sftp_local_list(const char *dir_path, struct sftp_local_entry **entries, size_t *count);
void sftp_local_list_free(struct sftp_local_entry *entries, size_t count);

int sftp_local_open_read(const char *path, struct sftp_local_file **file, uint64_t *size, uint32_t *mtime);
int sftp_local_read_at(struct sftp_local_file *file, uint64_t offset, void *buf, size_t size, size_t *got);

int sftp_local_open_temp_write(const char *dir_path, const char *final_name, char *temp_path, size_t temp_cap, struct sftp_local_file **file);
int sftp_local_write_at(struct sftp_local_file *file, uint64_t offset, const void *buf, size_t size);
int sftp_local_set_mtime(struct sftp_local_file *file, uint32_t mtime);
void sftp_local_close(struct sftp_local_file *file);

int sftp_local_commit_temp(const char *temp_path, const char *final_path);
int sftp_local_remove(const char *path);
int sftp_local_mkdir(const char *path);
int sftp_local_join_path(char *out, size_t out_cap, const char *dir, const char *name);

#endif
