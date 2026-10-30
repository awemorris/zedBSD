#!/bin/sh
# ws220-p001: the host model of a TCP transfer.  Builds src/kern/net/tcp.c, tcp-cubic.c, socket.c, packet-buf.c, checksum.c and in6.c
# as the kernel builds them (freestanding, the kernel's headers) with shim.c and model.c, and runs the transfer
# scenarios: A sends to B over a modelled link; the model prints each interval's rate and both ends' state.
#   plan/ws220/tests/tcp-model/run.sh                 the scenarios against WS220's targets, a summary line each and PASS/FAIL
#   plan/ws220/tests/tcp-model/run.sh NAME...         only the scenarios named (for a fix phase's own, e.g. lan-wrap overlap)
#   plan/ws220/tests/tcp-model/run.sh -- ARGS...      one run of the model with ARGS (see model.c), every interval printed
# OUT= chooses the build folder (default build/ws220-host, nothing in it is removed: each build overwrites its files).
# MODEL_CFLAGS= adds compile flags for a what-if (MODEL_CFLAGS=-DCONFIG_TCP_SEND_QUEUE_MAX=44: a larger send ring).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
out=${OUT:-build/ws220-host}
mkdir -p "$out"
cc=${CC:-clang}
flags="-std=c11 -O2 -g -nostdlibinc -Iinclude -Isrc -I. -Iplan/ws220/tests/tcp-model -DKERN_UAPI_NATIVE -DKERN_KCRT_NATIVE
	-DHAL_ARCH_AMD64 -DHAL_BOARD_PCAT -DKERN_USER_ABI_LP64 -ffreestanding -fno-builtin -Wall -Wextra -Werror ${MODEL_CFLAGS:-}"
objects=
for source in src/kern/net/tcp.c src/kern/net/tcp-cubic.c src/kern/net/socket.c src/kern/net/packet-buf.c src/kern/net/checksum.c \
    src/kern/net/in6.c plan/ws220/tests/tcp-model/shim.c plan/ws220/tests/tcp-model/model.c; do
	object=$out/$(basename "$source" .c).o
	# shellcheck disable=SC2086
	$cc $flags -c "$source" -o "$object" || { echo "tcp-model: build of $source failed"; exit 2; }
	objects="$objects $object"
done
# shellcheck disable=SC2086
$cc -o "$out/tcp-model" $objects -lc || { echo "tcp-model: link failed"; exit 2; }
# ws311-p002: a second build whose limits let A's window and B's take a bottleneck's round trip (the send ring, B's
# receive queue and reorder queue raised to 1024; the kernel's defaults are 768, 128 and 128): the congestion scenarios.
# The model's two hosts share one packet pool, 4096 (ws311-p003: A's copies and B's queues of a 1 MB window took all
# 1024, and the connection died; since ws315-p006 the kernel's own pool is 4096 too).
mkdir -p "$out/big"
big_flags="-DCONFIG_TCP_SEND_QUEUE_MAX=1024 -DCONFIG_TCP_RECEIVE_PACKETS=1024 -DCONFIG_TCP_REORDER_MAX=1024
	-DCONFIG_PACKET_BUF_POOL_COUNT=4096U"
objects=
for source in src/kern/net/tcp.c src/kern/net/tcp-cubic.c src/kern/net/socket.c src/kern/net/packet-buf.c src/kern/net/checksum.c \
    src/kern/net/in6.c plan/ws220/tests/tcp-model/shim.c plan/ws220/tests/tcp-model/model.c; do
	object=$out/big/$(basename "$source" .c).o
	# shellcheck disable=SC2086
	$cc $flags $big_flags -c "$source" -o "$object" || { echo "tcp-model: build of $source (big) failed"; exit 2; }
	objects="$objects $object"
done
# shellcheck disable=SC2086
$cc -o "$out/big/tcp-model" $objects -lc || { echo "tcp-model: link (big) failed"; exit 2; }

if [ "${1:-}" = "--" ]; then
	shift
	exec timeout 600 "$out/tcp-model" "$@"
fi

status=0
only=" $* "
# One scenario: its name, the least mean MB/s WS220 aims for in it (the stream must also arrive whole), and the model's
# arguments.  The targets are the model's (zedBSD at both ends, or a peer whose window does not move): p001 records which
# the code before the fix misses; the fix phases make them all pass.  --peer-window 65535 stands for a Linux peer.
scenario() {
	name=$1
	least=$2
	shift 2
	if [ "$only" != "  " ] && ! echo "$only" | grep -q " $name "; then
		return 0
	fi
	output=$(timeout 600 "$out/tcp-model" --quiet "$@")
	line=$(echo "$output" | grep '^tcp-model: summary')
	echo "$name: $line"
	if echo "$output" | grep -q '^tcpstats: FAIL'; then
		echo "$output" | grep '^tcpstats: FAIL' | sed 's/^/  /'
		status=1
	fi
	if echo "$output" | grep -q '^check: FAIL'; then
		echo "$output" | grep '^check: FAIL' | sed 's/^/  /'
		status=1
	fi
	complete=$(echo "$line" | sed -n 's/.*complete=\([01]\).*/\1/p')
	mean=$(echo "$line" | sed -n 's/.*mean_mbs=\([0-9.]*\).*/\1/p')
	if [ "$complete" != 1 ]; then
		echo "  FAIL: $name: the stream did not arrive whole"
		status=1
	elif ! awk -v a="$mean" -v b="$least" 'BEGIN { exit !(a >= b) }'; then
		echo "  FAIL: $name: mean $mean MB/s, below the target $least"
		status=1
	fi
}

# The clean LAN: gigabit, 0.2 ms round trip, a USB adapter's queue of 9 frames each way (no regression).
scenario lan-clean 30 --bytes 67108864
# Jumbo frames (ws221-p005): an MTU of 9000 at both ends and 64 KB writes give 8960-byte segments, in jumbo packets
# within the per-connection bounds; and with losses each lost frame is resent once.
scenario lan-jumbo 100 --bytes 67108864 --mtu 9000 --write-bytes 65536 --expect-mss 8960
scenario lan-jumbo-loss 50 --bytes 33554432 --mtu 9000 --write-bytes 65536 --loss-ab 0.1 --peer-window 65535 \
    --expect-mss 8960 --expect-retransmits-losses --expect-timeouts-max 0
scenario lan-1500-writes 100 --bytes 67108864 --write-bytes 65536 --expect-mss 1460
# ws221-p008: other senders to slow peers hold every copy the pool lets them (all but PACKET_BUF_HOLD_RESERVE), A is
# left 64: B's frames still find a packet (receive_no_packet 0) and the stream arrives whole, at what 64 copies carry.
scenario pool-held 5 --bytes 16777216 --pool-held 64
# ws221-p008: Nagle's algorithm: 1024-byte writes over 10 ms of round trip go in full 1460-byte segments (16 MiB is 11492
# of them, 16384 writes), and with TCP_NODELAY nearly each write is its own segment (only one queued behind a segment the
# device refused takes the next write's bytes; since ws315-p006 the ring of 768 and its 1152 KB buffer keep more writes
# queued while the device is full, 15570 segments).
scenario nagle-1024 4 --bytes 16777216 --delay-us 5000 --expect-segments-max 11600
scenario nodelay-1024 4 --bytes 16777216 --delay-us 5000 --nodelay --expect-segments-min 15000
# ws221-p008: SO_SNDBUF bounds the payload A's ring holds (16 KB: no more is queued, and the window it keeps busy).
scenario send-buffer-16k 20 --bytes 16777216 --send-buffer 16384
# 10 ms of round trip: the rate was the window over the round trip (64 KB: 6.4 MB/s; 128 KB: 12.1 MB/s); since ws315-p006
# B's buffer grows (32 MB/s with A's ring of 256; 60 MB/s with 768).
scenario rtt-10ms 4 --bytes 16777216 --delay-us 5000
# ws315-p006: B's receive buffer grows by itself (dynamic right-sizing) while its window is what holds the transfer
# back: over 10 ms of round trip the 128 KB window gave 12.1 MB/s, the buffer grows to the megabyte a connection may take
# and A's ring limits instead (32 MB/s with 256 segments; 60 MB/s with 768).  On the clean LAN, where the link limits,
# it grows no more than a quarter more than a window's time brings (174 KB; the measured round trip is a window's time,
# 1.2 ms, not the link's 0.2 ms; twice it gave 285 KB).  With SO_RCVBUF it stays where it was put: 128 KB keeps the rate at the window over the round trip, and 1 MB,
# larger than the default, is a 1 MB window too (before, the queue's 128 packets kept the window at 128 KB).
scenario receive-auto-10ms 20 --bytes 16777216 --delay-us 5000 --expect-receive-limit 524288:1048576
scenario receive-auto-lan 30 --bytes 67108864 --expect-receive-limit 131072:524288
scenario receive-fixed-10ms 10 --bytes 16777216 --delay-us 5000 --receive-buffer 131072 --expect-receive-limit 131072:131072
scenario receive-buffer-1m 20 --bytes 16777216 --delay-us 5000 --receive-buffer 1048576 --expect-receive-limit 1048576:1048576
# (The resends of these and of slow-reader stay at most the counts of ws220-p005, --expect-retransmits-max: p004 review-1 G2
# took p003's; p005 made slow-reader's 8566 (p003 14246; the probes of its closed window, which no longer count as
# duplicates) and lan-loss-1's 85 with 2 timeouts.
# rtt-10ms-loss-0.1 loses nothing to its losses since p005 (no timeout); its rate is the ring's 8 KB a round trip, p006.)
# Losses on the data with a Linux peer's window: each must cost a round trip or so, not a timeout.
# (ws221-p008: with the congestion window the ring holds the whole window; the three below resend each frame the link
# lost once and nothing more, --expect-retransmits-losses, rather than the counts the ring of 8 gave.)
scenario lan-loss-0.1 10 --bytes 33554432 --loss-ab 0.1 --peer-window 65535 --limit-s 300 --expect-retransmits-losses \
    --expect-timeouts-max 0
scenario lan-loss-1 2 --bytes 8388608 --loss-ab 1 --peer-window 65535 --limit-s 300 --expect-retransmits-losses \
    --expect-timeouts-max 2
scenario rtt-10ms-loss-0.1 2 --bytes 8388608 --delay-us 5000 --loss-ab 0.1 --peer-window 65535 --limit-s 300 --expect-retransmits-losses \
    --expect-timeouts-max 0
# The same losses with zedBSD as the receiver (its window moves, which hides part of the sender's fault).
scenario lan-loss-1-zedbsd 2 --bytes 16777216 --loss-ab 1 --limit-s 300
# Losses on the acknowledgements only.
scenario lan-ackloss-1 30 --bytes 33554432 --loss-ba 1 --peer-window 65535 --expect-retransmits-max 0
# A's sequence numbers wrap 4 MiB into the transfer (a 1 GB transfer crosses the wrap one time in four).
scenario lan-wrap 10 --bytes 16777216 --wrap-before 4194304 --limit-s 120
# A reader slower than the link (2 MB/s): the window closes and opens again, the rate is the reader's.
scenario slow-reader 1.6 --bytes 16777216 --reader-kbps 16000 --expect-retransmits-max 8566
# ws220-p002: one data segment in twenty starts 100 bytes before B's receive_next (B takes the rest), also across the wrap.
scenario overlap 30 --bytes 33554432 --overlap-ab 5
scenario overlap-wrap 30 --bytes 16777216 --overlap-ab 5 --wrap-before 4194304 --limit-s 120
# ws220-p002: a blind reset within the window, one outside it and a SYN, 100 ms in: none ends the connection.
scenario blind-segments 30 --bytes 33554432 --blind-at-ms 100
# The close in each order after a transfer, B's listener left open: closed first, B's accepted end is freed once its FIN
# is taken, and A's FIN then reaches the listener, which answers it with a reset (before, it was dropped and A's close
# waited the drain's 10 s).  Neither end's close may wait more than a second.
scenario close-b-first 1 --bytes 1048576 --close-first b
scenario close-a-first 1 --bytes 1048576 --close-first a
# ws220-p003: a device queue shorter than the send ring (8): what the device refuses waits unsent, in order, and goes on the
# pass its transmit completion runs, instead of its writer sleeping 10 ms (before p003: 0.68, 0.49, 0.20, 0.10 MB/s).
scenario queue-7 30 --bytes 67108864 --queue 7
scenario queue-5 30 --bytes 67108864 --queue 5
scenario queue-2 30 --bytes 67108864 --queue 2
scenario queue-1 30 --bytes 33554432 --queue 1
# ws220-p003: two writers on one socket while the device refuses: no byte twice, none out of order (before p003: corrupt).
# (ws221-p008: writes of one full segment each, so that each write is whole in one segment: since Nagle's algorithm a
# short write is joined by the next, and the model's second writer would then land inside the first's bytes.)
scenario two-writers 30 --bytes 33554432 --queue 3 --second-writer --expect-in-order --write-bytes 1460
# ws220-p003: losses with a one-frame device queue: the timer's resends and the acknowledgements are refused too.
scenario queue-1-loss-1 2 --bytes 16777216 --queue 1 --loss-ab 1 --limit-s 300
# ws220-p003: A's neighbour is unresolved for 500 ms (IPv4 refuses with EAGAIN and asks ARP each time): TCP takes the
# segments as lost on the wire (review-1 G1), so ARP is asked once for each segment in flight and then at the timer's
# resends (ws221-p008: the flight is the 64 KB window, 64 of the model's 1024-byte segments, and at most 12 resends), and
# the transfer goes on after the timer's resend (a stall of about one RTO).
unresolved() {
	name=$1
	most=$2
	shift 2
	if [ "$only" != "  " ] && ! echo "$only" | grep -q " $name "; then
		return 0
	fi
	scenario "$@"
	refusals=$(echo "$line" | sed -n 's/.* unresolved_refusals=\([0-9]*\).*/\1/p')
	if [ -z "$refusals" ] || [ "$refusals" -gt "$most" ]; then
		echo "  FAIL: $name: $refusals refusals (ARP requests), more than $most"
		status=1
	fi
}
# (ws221-p009: the 128 KB window holds 128 of the model's 1024-byte segments, so a flight asks ARP 128 times; before,
# 64 and the bound 76.)
unresolved unresolved 140 unresolved 5 --bytes 16777216 --unresolved-at-ms 100 --unresolved-ms 500
# ws220-p003: the neighbour never resolves again: the connection is given up after R2 (100 s), with ARP asked only at the
# ring's segments and the timer's resends (review-1 G1: before, a retry every 10 ms for ever).
giveup_unresolved() {
	name=$1
	shift
	if [ "$only" != "  " ] && ! echo "$only" | grep -q " $name "; then
		return 0
	fi
	line=$(timeout 600 "$out/tcp-model" --quiet "$@" | grep '^tcp-model: summary')
	echo "$name: $line"
	seconds=$(echo "$line" | sed -n 's/.* seconds=\([0-9.]*\).*/\1/p')
	refusals=$(echo "$line" | sed -n 's/.* unresolved_refusals=\([0-9]*\).*/\1/p')
	if ! awk -v a="$seconds" 'BEGIN { exit !(a >= 100.5 && a <= 200) }'; then
		echo "  FAIL: $name: gave up after $seconds s, not between R2 (100 s) and 200 s"
		status=1
	fi
	if [ -z "$refusals" ] || [ "$refusals" -gt 148 ]; then
		echo "  FAIL: $name: $refusals refusals (ARP requests), more than 148 (the flight's 128 and 20 resends)"
		status=1
	fi
}
giveup_unresolved unresolved-giveup --bytes 1073741824 --unresolved-at-ms 500 --unresolved-ms 1000000 --limit-s 400
# ws220-p003: a connect whose neighbour never resolves gives up after R2 for a SYN (3 minutes), asking ARP at each resend.
connect_unresolved() {
	name=$1
	shift
	if [ "$only" != "  " ] && ! echo "$only" | grep -q " $name "; then
		return 0
	fi
	line=$(timeout 600 "$out/tcp-model" --quiet "$@" | grep '^tcp-model: summary')
	echo "$name: $line"
	seconds=$(echo "$line" | sed -n 's/.* after \([0-9.]*\) s.*/\1/p')
	refusals=$(echo "$line" | sed -n 's/.* unresolved_refusals=\([0-9]*\).*/\1/p')
	if ! awk -v a="$seconds" 'BEGIN { exit !(a >= 180 && a <= 300) }'; then
		echo "  FAIL: $name: gave up after $seconds s, not between R2 for a SYN (180 s) and 300 s"
		status=1
	fi
	if [ -z "$refusals" ] || [ "$refusals" -gt 20 ]; then
		echo "  FAIL: $name: $refusals refusals (ARP requests), more than 20"
		status=1
	fi
}
connect_unresolved connect-unresolved --connect-unresolved
# ws220-p004: the round-trip estimate (RFC 6298): A's SRTT on the LAN (0.2 ms, within a tick) and with 10 ms of round
# trip, and net.tcpstats gives it.
scenario rtt-estimate-lan 30 --bytes 16777216 --expect-srtt-us 0:2000
scenario rtt-estimate-10ms 0.5 --bytes 4194304 --delay-us 5000 --expect-srtt-us 9000:13000
# ws220-p004: A's first SYN is lost: the data starts with a timeout of 3 s (RFC 6298 section 5.7), and the first sample
# sets SRTT = R and RTTVAR = R / 2 (section 2.2; RTT 10 ms).
syn_retried() {
	name=$1
	shift
	if [ "$only" != "  " ] && ! echo "$only" | grep -q " $name "; then
		return 0
	fi
	scenario "$name" 0.5 "$@"
	connected=$(timeout 600 "$out/tcp-model" --quiet "$@" | grep '^tcp-model: connected')
	echo "$name: $connected"
	if ! echo "$connected" | grep -q ' rto_ms=3000 '; then
		echo "  FAIL: $name: the connection did not start with a 3 s timeout"
		status=1
	fi
}
syn_retried syn-retried --bytes 4194304 --delay-us 5000 --lose-first-syn --expect-first-srtt-us 9000:11000 \
    --expect-first-rttvar-us 4000:6000
# ws220-p004 review-1: the losses' stalls are what the timeouts explain (one RTO a loss, more only for a resend lost
# too or a loss before the backoff ended), the seed fixed (the counts are printed on the "stall:" line).
scenario loss-stall-0.1 0 --bytes 33554432 --loss-ab 0.1 --peer-window 65535 --limit-s 300 --expect-stall-rto
scenario loss-stall-1 0 --bytes 8388608 --loss-ab 1 --peer-window 65535 --limit-s 300 --expect-stall-rto
# ws220-p004 review-1: a peer that delays its acknowledgements 40 ms (Linux) or 200 ms (the longest), each segment alone
# (a writer with gaps) or in a stream: no spurious resend, and SRTT takes the delay in.
scenario delayed-ack-40 0 --bytes 262144 --writer-gap-ms 300 --ack-delay-ms 40 --expect-retransmits-max 0 \
    --expect-srtt-us 30000:50000
scenario delayed-ack-200 0 --bytes 262144 --writer-gap-ms 300 --ack-delay-ms 200 --expect-retransmits-max 0 \
    --expect-srtt-us 190000:210000 --expect-fast-retransmits 0:0
scenario delayed-ack-stream 30 --bytes 16777216 --ack-delay-ms 200 --expect-retransmits-max 0
# ws220-p004 review-1 G1: the round trip grows from 0.2 ms to 600 ms past the timeout: the backoff holds until a sample,
# the resends stay few, and SRTT follows (a Linux-like peer, whose window does not move).  The step comes 2 ms in, while
# the megabyte is still on its way (ws221-p008: at 100 MB/s it takes about 10 ms).  The spurious timeouts resend no
# more than the flight (ws221-p008: the 64 KB window, 64 of the model's 1024-byte segments; before, the ring's 8).
# (ws221-p008: a timeout goes back to the oldest segment and resends the flight in slow start; after this spurious one
# the resends of segments B had come back as duplicates, which may bring one fast retransmit, as on BSD.)
# (ws311-p004: with CUBIC, the default since, a timeout leaves the threshold at 0.7 of the flight rather than half, so
# the slow start after the second spurious timeout resends a few more segments B had: 69 (Reno 57), at most 72.)
scenario rtt-step 0 --bytes 1048576 --delay-step-at-ms 2 --delay-step-us 300000 --peer-window 65535 --limit-s 120 \
    --expect-retransmits-max 72 --expect-srtt-us 500000:700000 --expect-fast-retransmits 0:1
# ws220-p005: the same with zedBSD as the receiver (its window moves): no resend on a reopened window keeps the sample.
# (ws221-p009: zedBSD's 128 KB window, 128 segments of the flight; the Linux-like peer above keeps 64 KB.)
# (ws221-p012: with SACK on both ends an acknowledgement that only reports a duplicate (D-SACK) is no duplicate
# acknowledgement (RFC 6675 section 2), so the spurious fast retransmit is gone and the megabyte is through before SRTT
# has settled at 600 ms: 494 ms after 19 samples, still rising.  The lower bound follows.)
scenario rtt-step-zedbsd 0 --bytes 1048576 --delay-step-at-ms 2 --delay-step-us 300000 --limit-s 120 \
    --expect-retransmits-max 128 --expect-srtt-us 450000:700000 --expect-fast-retransmits 0:1
# ws220-p004 review-1 G2: B's reader stops for 5 s, B's window closes: A's probes double apart (at most 10, p003: 17739).
# ws220-p005: the duplicates of the closed window start no fast retransmit; one after the window opens (the hole of a
# segment B refused) may.
scenario zero-window 2.5 --bytes 16777216 --reader-pause-at-ms 100 --reader-pause-ms 5000 --expect-retransmits-max 10 \
    --expect-fast-retransmits 0:1
# ws220-p004 review-1 M1: A's route fails for 50 ms; a write taken back and written again is not timed from its first
# try (SRTT stays within 5 ms).
scenario no-route 10 --bytes 16777216 --no-route-at-ms 100 --no-route-ms 50 --expect-srtt-max-us 5000
# ws220-p005: NewReno (RFC 6582): two segments of one window lost: one fast retransmit, then the partial
# acknowledgement's resend, no timeout (a Linux-like peer and zedBSD); a whole window lost: one timeout, its partial
# acknowledgements' resends, and no fast retransmit from the duplicates the resends bring.  (ws221-p008: the eight
# segments are no longer the whole flight, so the ones after them bring duplicates and one fast retransmit may come
# instead of the timeout.)
scenario two-holes 30 --bytes 1048576 --drop-ab 100,103 --peer-window 65535 --expect-fast-retransmits 1:1 \
    --expect-timeouts-max 0 --expect-retransmits-max 2
scenario two-holes-zedbsd 30 --bytes 1048576 --drop-ab 100,103 --expect-fast-retransmits 1:1 --expect-timeouts-max 0 \
    --expect-retransmits-max 2
# ws221-p008: a burst loses A's frames for 2 ms 50 ms in (a whole flight) and B holds a lone acknowledgement 40 ms as
# Linux does: one timeout, then the go-back-N in slow start resends the flight in a few round trips (before: a segment
# a partial acknowledgement, each held 40 ms; on the VisionFive 2, 9 s).
scenario burst-loss 20 --bytes 16777216 --drop-ab-run 50:2000 --ack-delay-ms 40 --expect-stall-max-ms 700
scenario window-lost 2 --bytes 1048576 --drop-ab 100,101,102,103,104,105,106,107 --peer-window 65535 \
    --expect-fast-retransmits 0:1 --expect-timeouts-max 1 --expect-retransmits-max 8
# ws220-p005 review-1 G2: losses across the sequence numbers' wrap: the fast retransmits go on (recover is compared only
# inside a recovery).
scenario lan-wrap-loss 10 --bytes 16777216 --wrap-before 4194304 --loss-ab 0.1 --peer-window 65535 --limit-s 120 \
    --expect-fast-retransmits 1:100 --expect-timeouts-max 0
# ws220-p005 (M7, the user's decision 2026-10-13: as FreeBSD): B's reader stops for 300 s and B keeps answering the probes
# of its closed window: A is never given up, probes every 5 to 60 s (the persist timer), and the stream completes.  (64 MB:
# ws221-p008 sends 4 MB within the first 100 ms.)
scenario persist-answering 0 --bytes 67108864 --reader-pause-at-ms 100 --reader-pause-ms 300000 --limit-s 400 \
    --expect-probe-gap-s 5:60 --expect-retransmits-max 16
# The same peer goes silent (the link loses everything from 2 s), or A has shut its side at 1 s (FIN_WAIT_1), or A sets
# TCP_MAXUNACKTIME 10: A drops the connection after the persist timer's 12 steps and min(2 h, RTO x 2559) of silence
# (about 9 minutes here), after 60 s of silence, or 10 s after its data last moved with a reset B takes (B closed, 0).
closes() {
	name=$1
	low=$2
	high=$3
	b_state=$4
	shift 4
	if [ "$only" != "  " ] && ! echo "$only" | grep -q " $name "; then
		return 0
	fi
	output=$(timeout 900 "$out/tcp-model" --quiet "$@")
	line=$(echo "$output" | grep '^close:')
	echo "$name: $line $(echo "$output" | grep '^persist:')"
	closed=$(echo "$line" | sed -n 's/.*a_closed_at_s=\([-0-9.]*\).*/\1/p')
	state=$(echo "$line" | sed -n 's/.* b_state=\([-0-9]*\).*/\1/p')
	if ! awk -v a="$closed" -v l="$low" -v h="$high" 'BEGIN { exit !(a >= l && a <= h) }'; then
		echo "  FAIL: $name: A closed at $closed s, not between $low and $high s"
		status=1
	fi
	if [ "$b_state" != "-" ] && [ "$state" != "$b_state" ]; then
		echo "  FAIL: $name: B is in state $state, not $b_state"
		status=1
	fi
	if echo "$output" | grep -q '^check: FAIL'; then
		echo "$output" | grep '^check: FAIL' | sed 's/^/  /'
		status=1
	fi
}
closes persist-silent 500 800 - --bytes 67108864 --reader-pause-at-ms 100 --reader-pause-ms 100000000 --blackhole-at-ms 2000 \
    --limit-s 1200 --expect-probe-gap-s 5:60
closes persist-closed 60 80 - --bytes 67108864 --reader-pause-at-ms 100 --reader-pause-ms 100000000 --blackhole-at-ms 2000 \
    --close-at-ms 1000 --limit-s 120
closes maxunacktime 10 11 0 --bytes 67108864 --reader-pause-at-ms 100 --reader-pause-ms 300000 --maxunacktime 10 --limit-s 400
# ws220-p002: the link loses everything 500 ms in: A gives up after R2 (at least 100 s; 1+2+4+8+16+32+60 s of resends).
giveup() {
	name=$1
	shift
	if [ "$only" != "  " ] && ! echo "$only" | grep -q " $name "; then
		return 0
	fi
	line=$(timeout 600 "$out/tcp-model" --quiet "$@" | grep '^tcp-model: summary')
	echo "$name: $line"
	seconds=$(echo "$line" | sed -n 's/.* seconds=\([0-9.]*\).*/\1/p')
	if ! awk -v a="$seconds" 'BEGIN { exit !(a >= 100.5 && a <= 200) }'; then
		echo "  FAIL: $name: gave up after $seconds s, not between R2 (100 s) and 200 s"
		status=1
	fi
}
giveup data-giveup --bytes 1073741824 --blackhole-at-ms 500 --limit-s 400
# ws220-p002: A's SYN is lost from the start: the connect gives up only after 3 minutes (R2 for a SYN).
scenario syn-giveup 0 --connect-blackhole
# ws311-p002: A's data frames are all lost for 700 ms: the timer's resends are lost too, and each timeout repeated within
# the recovery the first started holds the slow start threshold (RFC 5681 section 3.1; before: 2 segments).
scenario timeout-repeated 10 --bytes 33554432 --drop-ab-run 100:700000 --peer-window 65535 --limit-s 120 \
    --expect-ssthresh-held --expect-timeouts-max 3
# ws311-p002: the congestion window's use (RFC 7661) over a bottleneck, with the big build: a 100 Mbps router with a
# drop-tail queue of a quarter of the 250 KB the 20 ms round trip holds, B's buffers at 1 MB.  A's window limits it most of
# the time, its slow start doubles each round trip, its congestion avoidance grows a segment a round trip (Reno), and its
# window never grows past twice what it sent (the check of every run).  60 s, timed; the rate is what the losses leave.
big_scenario() {
	saved=$out
	out=$out/big
	scenario "$@"
	out=$saved
}
big_scenario reno-bottleneck 4 --cc reno --run-s 60 --delay-us 10000 --bottleneck-mbps 100 --bottleneck-queue 62500 \
    --send-buffer 1048576 --receive-buffer 1048576 --write-bytes 65536 --expect-slow-start --expect-ca-growth 0.8 \
    --expect-cwnd-limited 0.8
# ws311-p002: a second connection 5 s in over the same router (the "fair:" line gives the shares; p003 sets what they
# must be): both streams arrive intact (the least rate is the first connection's, which shares the router).
big_scenario reno-two-flows 1 --cc reno --second-flow-cc reno --run-s 60 --delay-us 10000 --bottleneck-mbps 100 --bottleneck-queue 62500 \
    --send-buffer 1048576 --receive-buffer 1048576 --write-bytes 65536 --second-flow-at-ms 5000

[ $status = 0 ] && echo "tcp-model: PASS" || echo "tcp-model: FAIL"
exit $status
