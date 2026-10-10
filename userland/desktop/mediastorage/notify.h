/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef MEDIA_DB_NOTIFY_H
#define MEDIA_DB_NOTIFY_H

/* Publishes a completed database mutation through the compositor's media extension. */
void media_db_notify(void);
void media_database_release(void);

#endif
