#!/bin/sh
# QEMU for plan/tools/boot-test.sh (QEMU=this) with the CPU count of BUG423_SMP (default 4) in place of the
# script's -smp 4, and, with BUG423_GDB=PORT, QEMU's gdbstub on that local TCP port.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
smp=${BUG423_SMP:-4}
first=1
for arg in "$@"; do
	if [ "$first" = 1 ]; then
		set --
		first=0
	fi
	if [ "${prev:-}" = -smp ]; then
		arg=$smp
	fi
	set -- "$@" "$arg"
	prev=$arg
done
if [ -n "${BUG423_GDB:-}" ]; then
	set -- "$@" -gdb "tcp:127.0.0.1:$BUG423_GDB"
fi
exec qemu-system-x86_64 "$@"
