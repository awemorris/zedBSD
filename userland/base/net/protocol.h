/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_NETWORKD_PROTOCOL_H
#define ZEDBSD_NETWORKD_PROTOCOL_H

#ifndef NETWORKD_SOCKET
#define NETWORKD_SOCKET "/run/networkd.sock"
#endif
#define NETWORKD_PROTOCOL_VERSION "V1"
#define NETWORKD_REQUEST_MAX 512
#define NETWORKD_RESPONSE_MAX 1024

#endif
