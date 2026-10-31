#!/bin/sh
# ws299: the NVMe read/write measurement on the Latitude 5330's zedBSD, run as root by nvme-bench-5330.sh.
#   sh bench-zedbsd.sh read | write | p7rawwrite | p7rawread | p7fswrite | p7fsread | p7direct
# read:  /dev/zero at three block sizes; random 4 KiB reads at depth 1 and 32 (randread, copied to RANDREAD);
#        the UFS file WFILE read cold (the write mode of an earlier boot wrote it); reads of /dev/nvme0n1 at three
#        block sizes and the curve of CHUNKS reads of CHUNK_MIB MiB, all inside the data region R0_MIB..+R_MIB
#        (blocks of a Debian file of random bytes, the same blocks bench-debian.sh reads), with the cache and I/O
#        counters after every step ("snap" blocks), and a re-read of the last chunk (a cache hit).  Only reads.
# write: REST seconds before each step.  WFILE (1 GiB on the root UFS, p5, zedBSD's own) written over in place
#        and synced; WFILE_NEW removed and written anew (new blocks: UFS sends no TRIM) and synced; then, when the
#        swap on p6 (zedBSD's own, discarded by the caller before this boot) can be turned off, 1 GiB of zeros onto
#        p6 in four pieces of 256 MiB and a sync (the caller puts the swap header back from Debian).  Debian's
#        partitions are never written.
# p7*:  the 32 GiB measurement partition p7 (zedBSD-nvme-bench; the caller discards it from Debian before the write
#        boots): P7_MIB MiB of patwrite's data written raw and synced, read back raw in the next boot; a UFS made
#        on p7 (mkfs), a file of P7_MIB MiB written and synced, read back in the next boot.
# p7direct: O_DIRECT (directio, copied to DIRECTIO): P7_MIB MiB written to p7 (discarded by the caller before the
#        boot) in 1 MiB requests and synced, read back in 1 MiB and 4 MiB requests, P7_MIB MiB of the data region
#        read in 1 MiB requests, and random 4 KiB reads at depth 1 and 32 (randread direct, one description and
#        one per thread).
# qdprobe: where depth 32 stops: random 4 KiB reads at depth 1 and 32, threads and processes, direct and through
#        the cache, without the clock in the loop, and getpid() alone; after each run the kernel's last "exp:" and
#        nvme latency lines (a measurement kernel prints "exp:").
# cpu:  cpuloop three times (one thread's compute, fill and memcpy rates), to compare the CPU's speed with Debian's.
# Every timed step prints "step NAME bytes=N" and the /bin/time lines (real/user/sys); the summary is made on the
# build host (summary.py).  The only file removed is WFILE_NEW, inside zedBSD, before it is written again.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
MODE=${1:?read|write|p7rawwrite|p7rawread|p7fswrite|p7fsread|p7direct|qdprobe|cpu}
DEV=/dev/nvme0n1
SWAP=/dev/nvme0n1p6
R0_MIB=${R0_MIB:-25681}
R_MIB=${R_MIB:-8064}
CHUNK_MIB=${CHUNK_MIB:-256}
CHUNKS=${CHUNKS:-16}
REST=${REST:-30}
RAND_SECONDS=${RAND_SECONDS:-15}
RANDREAD=${RANDREAD:-/tmp/randread-zedbsd}
FILE_MIB=${FILE_MIB:-1024}
WFILE=${WFILE:-/root/ws299-w.bin}
WFILE_NEW=${WFILE_NEW:-/root/ws299-n.bin}
PATWRITE=${PATWRITE:-/tmp/patwrite-zedbsd}
DIRECTIO=${DIRECTIO:-/tmp/directio-zedbsd}
P7=/dev/nvme0n1p7
P7_MIB=${P7_MIB:-4096}
P7_MNT=/mnt/bench7

snap() {
	echo "snap $1"
	for name in vfs.bufcache.max_bytes vfs.bufcache.current_bytes vfs.bufcache.stats vfs.cache_memory.stats vfs.io.stats hw.cputimes hw.memory.stats; do
		sysctl "$name"
	done
	echo "endsnap $1"
}

# step NAME BYTES dd-arguments...: one timed dd.
step() {
	name=$1
	bytes=$2
	shift 2
	echo "step $name bytes=$bytes"
	/bin/time dd "$@" 2>&1 | grep -E '^(real|user|sys) |^dd:|records|time:'
}

# shstep NAME BYTES COMMAND: one timed shell command (a write and its sync together).
shstep() {
	echo "step $1 bytes=$2"
	/bin/time sh -c "$3" 2>&1 | grep -E '^(real|user|sys) |^dd:|time:'
}

echo "bench mode=$MODE"
uname -a
sysctl hw.ncpu
sysctl hw.memory.stats
echo "region ${R0_MIB}+${R_MIB} MiB"
snap start

case "$MODE" in
read)
	# The read system call alone (no disk): /dev/zero at the same three block sizes, 512 MiB each.
	step zero64k $((512 * 1048576)) if=/dev/zero of=/dev/null bs=64k count=8192
	step zero1m $((512 * 1048576)) if=/dev/zero of=/dev/null bs=1024k count=512
	step zero4m $((512 * 1048576)) if=/dev/zero of=/dev/null bs=4096k count=128
	snap zero

	# Random reads first, while nothing of the region is cached.
	if [ -x "$RANDREAD" ]; then
		echo "step rand4k-qd1 bytes=0"
		"$RANDREAD" $DEV 4096 1 "$RAND_SECONDS" "$R0_MIB" "$R_MIB"
		snap rand4k-qd1
		echo "step rand4k-qd32 bytes=0"
		"$RANDREAD" $DEV 4096 32 "$RAND_SECONDS" "$R0_MIB" "$R_MIB"
		snap rand4k-qd32
	else
		echo "no randread at $RANDREAD"
	fi

	# The UFS file, cold.
	if [ -f "$WFILE" ]; then
		step ufsread $(($(wc -c < "$WFILE"))) if="$WFILE" of=/dev/null bs=1024k
		snap ufsread
	fi

	# Block sizes, 128 MiB each.
	step bs64k $((128 * 1048576)) if=$DEV of=/dev/null bs=64k skip=$((R0_MIB * 16)) count=2048
	step bs1m $((128 * 1048576)) if=$DEV of=/dev/null bs=1024k skip=$((R0_MIB + 128)) count=128
	step bs4m $((128 * 1048576)) if=$DEV of=/dev/null bs=4096k skip=$(((R0_MIB + 256) / 4)) count=32
	snap bs

	# The curve.
	i=0
	while [ "$i" -lt "$CHUNKS" ]; do
		step "chunk$i" $((CHUNK_MIB * 1048576)) if=$DEV of=/dev/null bs=1024k \
		    skip=$((R0_MIB + 512 + i * CHUNK_MIB)) count=$CHUNK_MIB
		snap "chunk$i"
		i=$((i + 1))
	done

	# The last chunk again, which the cache should hold.
	last=$((i - 1))
	step reread $((CHUNK_MIB * 1048576)) if=$DEV of=/dev/null bs=1024k \
	    skip=$((R0_MIB + 512 + last * CHUNK_MIB)) count=$CHUNK_MIB
	snap reread
	;;
write)
	df /root
	bytes=$((FILE_MIB * 1048576))
	# WFILE must be whole before it is written over, or the write would allocate.
	have=0
	[ -f "$WFILE" ] && have=$(($(wc -c < "$WFILE")))
	if [ "$have" -lt "$bytes" ]; then
		echo "making $WFILE"
		dd if=/dev/zero of="$WFILE" bs=1024k count="$FILE_MIB" 2>&1 | grep -E '^dd:'
		sync
	fi
	snap prepared

	sleep "$REST"
	shstep ufsover "$bytes" "dd if=/dev/zero of=$WFILE bs=1024k count=$FILE_MIB conv=notrunc && sync"
	snap ufsover

	rm -f "$WFILE_NEW"
	sync
	sleep "$REST"
	shstep ufsnew "$bytes" "dd if=/dev/zero of=$WFILE_NEW bs=1024k count=$FILE_MIB && sync"
	snap ufsnew

	# The raw swap partition, when zedBSD lets go of it.
	if swapoff $SWAP; then
		echo "swapoff ok"
		sleep "$REST"
		i=0
		while [ "$i" -lt 4 ]; do
			step "raw$i" $((256 * 1048576)) if=/dev/zero of=$SWAP bs=1024k seek=$((i * 256)) count=256
			snap "raw$i"
			i=$((i + 1))
		done
		echo "step rawsync bytes=0"
		/bin/time sync 2>&1 | grep -E '^(real|user|sys) '
		snap rawsync
	else
		echo "swapoff failed: raw write skipped"
	fi
	;;
p7rawwrite)
	echo "step p7-rawwrite bytes=$((P7_MIB * 1048576))"
	"$PATWRITE" $P7 "$P7_MIB"
	snap p7-rawwrite
	;;
p7rawread)
	step p7-rawread $((P7_MIB * 1048576)) if=$P7 of=/dev/null bs=1024k count="$P7_MIB"
	snap p7-rawread
	;;
p7fswrite)
	# MKFS=0 keeps the UFS Debian put on p7 (bench-debian.sh p7ufs).
	if [ "${MKFS:-1}" = 1 ]; then
	# mkfs asks for the device's registration, which a first refused run prints (it writes nothing).
	refused=$(echo no | mkfs -t ufs --profile=native nvme0n1p7 2>&1)
	reg=$(echo "$refused" | sed -n 's/.*registration=\([0-9]*\).*/\1/p')
	[ -n "$reg" ] || { echo "$refused"; echo "mkfs failed: no registration"; exit 1; }
	echo "FORMAT nvme0n1p7:$reg" | mkfs -t ufs --profile=native nvme0n1p7 || { echo "mkfs failed"; exit 1; }
	fi
	mkdir -p $P7_MNT
	mount -t ufs $P7 $P7_MNT || { echo "mount failed"; exit 1; }
	df $P7_MNT
	snap prepared
	sleep "$REST"
	echo "step p7-fswrite bytes=$((P7_MIB * 1048576))"
	"$PATWRITE" $P7_MNT/f.bin "$P7_MIB"
	snap p7-fswrite
	umount $P7_MNT
	;;
p7fsread)
	mkdir -p $P7_MNT
	mount -t ufs $P7 $P7_MNT || { echo "mount failed"; exit 1; }
	step p7-fsread $((P7_MIB * 1048576)) if=$P7_MNT/f.bin of=/dev/null bs=1024k
	snap p7-fsread
	umount $P7_MNT
	;;
p7direct)
	snap prepared
	sleep "$REST"
	echo "step p7-directwrite bytes=$((P7_MIB * 1048576))"
	"$DIRECTIO" write $P7 "$P7_MIB" 1024
	snap p7-directwrite
	echo "step p7-directread bytes=$((P7_MIB * 1048576))"
	"$DIRECTIO" read $P7 "$P7_MIB" 1024
	echo "step p7-directread4m bytes=$((P7_MIB * 1048576))"
	"$DIRECTIO" read $P7 "$P7_MIB" 4096
	echo "step data-directread bytes=$((P7_MIB * 1048576))"
	"$DIRECTIO" read $DEV "$P7_MIB" 1024 "$R0_MIB"
	snap directread
	for depth in 1 32; do
		echo "step rand4k-direct-qd$depth bytes=0"
		"$RANDREAD" $DEV 4096 $depth "$RAND_SECONDS" "$R0_MIB" "$R_MIB" direct
		echo "step rand4k-direct-fdeach-qd$depth bytes=0"
		"$RANDREAD" $DEV 4096 $depth "$RAND_SECONDS" "$R0_MIB" "$R_MIB" direct fdeach
	done
	snap rand-direct
	;;
cpu)
	# How fast one thread computes and copies here (cpuloop, copied to CPULOOP), against Debian's same mode.
	for run in 1 2 3; do
		"${CPULOOP:-/tmp/cpuloop-zedbsd}"
	done
	;;
qdprobe)
	for run in "1 direct notime" "32 direct notime" "32 direct procs notime" "1 notime" "32 notime" "32 procs notime" \
	    "1 nullcall notime" "32 nullcall notime" "32 nullcall procs notime"; do
		depth=${run%% *}
		how=${run#* }
		echo "step qd$depth-$(echo "$how" | tr ' ' '-') bytes=0"
		"$RANDREAD" $DEV 4096 "$depth" "$RAND_SECONDS" "$R0_MIB" "$R_MIB" $how
		dmesg | grep -E '^exp:|^nvme: [0-9]+ commands' | tail -4
	done
	snap qdprobe
	;;
esac
echo "bench done"
