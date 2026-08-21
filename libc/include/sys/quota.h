/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_QUOTA_H
#define ZEDBSD_SYS_QUOTA_H

#include <zedbsd/quota.h>

int quotactl(const char *, struct quota_control *);

#endif
