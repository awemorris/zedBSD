/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_NETCONF_RECONCILE_H
#define ZEDBSD_NETCONF_RECONCILE_H

#include "userland/base/net/netconf.h"

typedef int (*netconf_reconcile_emit)(const char *, const char *, void *);

int netconf_reconcile_supported(const struct netconf *, char *, size_t);
int netconf_reconcile(const struct netconf *, const struct netconf *,
	netconf_reconcile_emit, void *, char *, size_t);

#endif
