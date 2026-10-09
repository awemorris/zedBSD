/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's record of the phone used as a phone (ws197-p003,
 * plan/ws197/phase003/phase.md section 3): one file beside the phone's
 * bond, FOLDER/<controller>/<address>-bredr.phone, that says who owns the
 * phone link (the uid and its account's name, which tells a reused uid),
 * which profiles are on, and whether the link is wanted at all.  Written
 * whole (a temporary file, fsync, rename); a record with a line missing,
 * repeated or out of range is refused whole.
 *
 * A record is valid when it reads, its bond is there with a key made with
 * a number the user confirmed, and its uid's account still has the name
 * written.  The owner's data goes to nobody else, so an invalid record
 * owns nothing.
 */

#ifndef BLUETOOTHD_PHONEREC_H
#define BLUETOOTHD_PHONEREC_H

#include "userland/base/bluetoothd/hci.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* The longest account name kept (and its NUL), and the most a record's file holds. */
#define BTD_PHONEREC_USER_MAX		64U
#define BTD_PHONEREC_TEXT_MAX		512U

/* How many records one listing of the controller's folder reads at most. */
#define BTD_PHONEREC_LIST_MAX		8U

/* The profiles of a record, as bits: messages (MAP), contacts (PBAP), calls (HFP). */
#define BTD_PHONEREC_MESSAGES		0x01U
#define BTD_PHONEREC_CONTACTS		0x02U
#define BTD_PHONEREC_CALLS		0x04U
#define BTD_PHONEREC_PROFILES		0x07U

/*
 * Gives the name of a uid's account (0), or ENOENT when there is none.
 * The daemon asks the system's accounts; the host tests answer by hand.
 */
typedef int (*btd_phonerec_account_fn)(void *context, uid_t uid, char *name, size_t size);

/*
 * The record of the phone: its address, its owner's uid and account
 * name, the profiles on, and whether the phone link is wanted.  A record
 * is filled by a read, a parse, or the phone link before a write.
 */
struct btd_phonerec {
	uint8_t address[BTD_ADDRESS_BYTES];
	uid_t uid;
	char user[BTD_PHONEREC_USER_MAX];
	unsigned profiles;
	int enabled;
};

int btd_phonerec_path(const char *folder, const uint8_t *controller, const uint8_t *address, char *path, size_t size);
int btd_phonerec_user_ok(const char *name);
int btd_phonerec_format(const struct btd_phonerec *record, char *text, size_t size);
int btd_phonerec_parse(const char *text, size_t length, struct btd_phonerec *record);
int btd_phonerec_write(const char *folder, const uint8_t *controller, const struct btd_phonerec *record);
int btd_phonerec_read(const char *folder, const uint8_t *controller, const uint8_t *address, struct btd_phonerec *record);
int btd_phonerec_forget(const char *folder, const uint8_t *controller, const uint8_t *address);
int btd_phonerec_list(const char *folder, const uint8_t *controller, struct btd_phonerec *records, unsigned max, unsigned *count);
int btd_phonerec_valid(const char *folder, const uint8_t *controller, const struct btd_phonerec *record, btd_phonerec_account_fn account, void *context);
int btd_phonerec_prune(const char *folder, const uint8_t *controller);

#endif
