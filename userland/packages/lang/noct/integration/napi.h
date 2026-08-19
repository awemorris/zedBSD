/*
 * zedBSD Noct native APIs
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_NOCT_NAPI_H
#define ZEDBSD_NOCT_NAPI_H

#include <stddef.h>
#include <stdint.h>

/*
 * The normalized key namespace lives upstream with BeUI, because the
 * same compiled script must observe identical Key.* values in zedBSD, on
 * PC-98 MS-DOS, and on a desktop host.  zedBSD' own Screen and Keyboard
 * modules use the same codes so a script never has two vocabularies.
 */
#include <noct/beui.h>

typedef struct rt_env NoctEnv;

#define ZEDBSD_NOCT_DIRECTORY_MAX 256U
#define ZEDBSD_NOCT_PATH_MAX 256U
#define ZEDBSD_NOCT_SOURCE_MAX (256U * 1024U)

struct zedbsd_noct_dirent {
	char name[ZEDBSD_NOCT_PATH_MAX];
	uint64_t size;
	uint8_t attributes;
};

/*
 * The core NAPI only knows this injectable interface.  The boot target maps it
 * to the GDC, BIOS keyboard gateway, and the selected filesystem.  Host tests
 * supply deterministic in-memory implementations.
 */
struct zedbsd_noct_services {
	void *context;
	/*
	 * Optional.  Binding this pointer does not touch graphical hardware.
	 * BeUI itself, including its key-state input HAL, lives upstream in
	 * Noct; the boot target only supplies the backend.
	 */
	const struct noct_beui_hal *beui;
	int (*screen_clear)(void *context);
	int (*screen_clear_row)(void *context, unsigned row);
	int (*screen_put)(void *context, unsigned row, unsigned column,
			  const char *text, uint8_t attribute);
	int (*screen_put_utf8)(void *context, unsigned row, unsigned column,
			       const char *text, unsigned length,
			       uint8_t attribute);
	int (*screen_clear_to_eol)(void *context, unsigned row,
				   unsigned column);
	int (*screen_set_cursor)(void *context, unsigned row, unsigned column);
	int (*screen_show_cursor)(void *context, int visible);
	int (*keyboard_poll)(void *context);
	int (*keyboard_read)(void *context);
	/* Seconds in the current minute (0..59), or -1 if unavailable. */
	int (*clock_second)(void *context);
	int (*file_size)(void *context, const char *path, uint32_t *size);
	int (*file_read)(void *context, const char *path, uint32_t offset,
			 void *buffer, uint32_t length);
	/* Return 1 for an entry, 0 at end, and -1 for an invalid path/I/O. */
	int (*directory_read)(void *context, const char *path, unsigned index,
			      struct zedbsd_noct_dirent *entry);
};

struct zedbsd_noct_options;

/* Convert the PC-98 BIOS AX pair to the stable BE key namespace. */
int zedbsd_key_normalize_bios_ax(uint16_t bios_ax);

int zedbsd_noct_napi_register(NoctEnv *env,
			      const struct zedbsd_noct_options *options);
void zedbsd_noct_napi_cleanup(void);

#endif
