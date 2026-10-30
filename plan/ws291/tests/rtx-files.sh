#!/bin/sh
# ws291: the NVMe zedBSD's programs and libraries of the RTX 2070 machine (10.0.10.3) swapped for one tree's, sourced
# (". plan/ws291/tests/rtx-files.sh") from the top of the tree by the measurement scripts beside it.  FILES lists the
# files as "DEST=SOURCE ..." (DEST a path on zedBSD, SOURCE a file of the tree's build).
#
#     files_restore_kernel OUT    the NVMe's /boot/boot0/vmunix read back to OUT/nvme-vmunix (the kernel to put back),
#                                 checked against RESTORE_CKSUM when it is set
#     files_put OUT               each DEST kept as DEST.p7old (once: a kept one is not overwritten), the SOURCE copied
#                                 to DEST.p7new (DEST's mode) and renamed over DEST (a running program keeps its old file); the
#                                 compositor, when /bin/wayland is among them, loaded once first (its usage line)
#     files_back OUT              each DEST.p7old renamed back over DEST, the kernel OUT/nvme-vmunix put in
#
# They need ip (the running zedBSD's address), z (an SSH command to it), say (a line printed and kept), KEY and SSHOPT.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

files_restore_kernel() {
	timeout 120 scp -O -i "$KEY" $SSHOPT "root@$ip:/boot/boot0/vmunix" "$1/nvme-vmunix" > /dev/null 2>&1 ||
	    { say "FAIL the NVMe kernel was not read back"; return 1; }
	sum=$(cksum < "$1/nvme-vmunix" | cut -d' ' -f1)
	say "nvme kernel $sum"
	if [ -n "${RESTORE_CKSUM:-}" ] && [ "$sum" != "$RESTORE_CKSUM" ]; then
		say "FAIL the NVMe kernel is $sum, not $RESTORE_CKSUM"
		return 1
	fi
}

files_put() {
	: > "$1/files-old.txt"
	for pair in ${FILES:-}; do
		dest=${pair%%=*}
		src=${pair#*=}
		[ -s "$src" ] || { say "FAIL no $src"; return 1; }
		z "test -f $dest.p7old || cp -p $dest $dest.p7old; cksum $dest.p7old" >> "$1/files-old.txt" 2>&1
	done
	say "kept $(awk '{print $1, $3}' "$1/files-old.txt" | tr '\n' ' ')"
	for pair in ${FILES:-}; do
		dest=${pair%%=*}
		src=${pair#*=}
		timeout 120 scp -O -i "$KEY" $SSHOPT "$src" "root@$ip:/tmp/p7.file" > /dev/null 2>&1 ||
		    { say "FAIL $src was not copied"; return 1; }
		if [ "$dest" = /bin/wayland ]; then
			z 'chmod 755 /tmp/p7.file; /tmp/p7.file --no-such-option 2>&1 | head -5' > "$1/wayland-check.txt" 2>&1
			grep -q '^usage: wayland' "$1/wayland-check.txt" ||
			    { say "FAIL the compositor does not load: $(head -1 "$1/wayland-check.txt")"; return 1; }
		fi
		z "cp -p $dest.p7old $dest.p7new && cat /tmp/p7.file > $dest.p7new && mv $dest.p7new $dest && sync" ||
		    { say "FAIL $dest was not put in"; return 1; }
		say "put $dest $(cksum < "$src" | cut -d' ' -f1)"
	done
	# The compositor is loaded again with the libraries put in.
	z 'test -x /bin/wayland && /bin/wayland --no-such-option 2>&1 | head -1' | sed 's/^/compositor now: /' | tee -a "$summary"
}

files_back() {
	for pair in ${FILES:-}; do
		dest=${pair%%=*}
		z "test -f $dest.p7old && cp -p $dest.p7old $dest.p7new && mv $dest.p7new $dest" ||
		    say "FAIL $dest was not put back"
	done
	timeout 120 scp -O -i "$KEY" $SSHOPT "$1/nvme-vmunix" "root@$ip:/tmp/vmunix.restore" > /dev/null 2>&1 &&
	    z 'cp /tmp/vmunix.restore /boot/boot0/vmunix && sync' || say "FAIL the kernel was not put back"
	list=
	for pair in ${FILES:-}; do
		list="$list ${pair%%=*}"
	done
	z "sync; cksum$list /boot/boot0/vmunix; grep -c '^nvrtx.vk.perf=' /boot/boot0/zedbsd.cfg" > "$1/files-back.txt" 2>&1
	say "back $(awk '{print $1, $3}' "$1/files-back.txt" | tr '\n' ' ')"
	say "expected $(awk '{print $1}' "$1/files-old.txt" | tr '\n' ' ')$(cksum < "$1/nvme-vmunix" | cut -d' ' -f1)"
}
