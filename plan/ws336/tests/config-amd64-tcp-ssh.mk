# The guest image of config-amd64-ssh.mk (ours in the place of OpenSSH) with sysctl, ifconfig and tcp-perf, the
# programs plan/tools/tcp-close/qemu-close.sh and the TCP tests use; plan/ws221/tests/config-amd64-tcp.mk with ours.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/ws336/tests/config-amd64-ssh.mk
ZEDBSD_USER_PROGRAMS += sysctl ifconfig tcp-perf
