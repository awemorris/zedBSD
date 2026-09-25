#!/bin/sh
# zedBSD guest harness: SSH, file transfer, and debugging for an emulated guest.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
exec python3 "$(dirname -- "$0")/guest.py" "$@"
