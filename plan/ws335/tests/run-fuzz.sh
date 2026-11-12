#!/bin/sh
# The fuzz targets of libcrypto-compat and libssl-compat (plan/ws335/tests/fuzz/fuzz-*.c), built with the host's
# clang and libFuzzer (-fsanitize=fuzzer,address,undefined) over the sources of both libraries, and run for
# FUZZ_SECONDS each (default 300), at most FUZZ_JOBS at a time (default 4, the most allowed): FUZZ_JOBS workers each
# take every FUZZ_JOBS-th target in turn, so the whole run lasts about ceil(targets / FUZZ_JOBS) * FUZZ_SECONDS.
# Targets:
#   der x509 pem                  the DER reader, certificates (cryc_x509_parse, the name checks, d2i_X509), PEM
#   server-hello encrypted-extensions certificate server-key-exchange msg-other   the handshake message parsers
#   record                        record protection (plaintext, TLS 1.3 with a valid tag, TLS 1.2 round trip)
#   conn                          the whole client receive path through SSL_connect
#   mlkem                         ML-KEM-768 decapsulation and encapsulation-key import
#   sh plan/ws335/tests/run-fuzz.sh [TARGET...]          (all when none is named)
#   FUZZ_SECONDS=3600 sh plan/ws335/tests/run-fuzz.sh    (the hour of the review; long runs need the user's OK)
#   FUZZ_JOBS=2 sh plan/ws335/tests/run-fuzz.sh          (two targets at a time)
# The corpora are kept in build/tmp/ws335-fuzz-corpus/<target> (seeded by fuzz/seeds.py from
# WS335_VECTORS/rec/limbo.vec, default build/ws335-vectors) and grow from run to run; each run's logs and any crash
# input are in build/tmp/ws335-fuzz (a fresh run directory).  Each target prints "fuzz <target>: PASS (<runs>
# runs, <seconds> s, <corpus> inputs)" or "fuzz <target>: FAIL (...)"; the last line is run-fuzz: PASS or FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
top=$(pwd)
PATH=/usr/bin:/bin:$PATH
export PATH
. plan/tools/fresh-out.sh
out=build/tmp/ws335-fuzz
fresh_out "$out"
corpora=build/tmp/ws335-fuzz-corpus
seconds=${FUZZ_SECONDS:-300}
jobs=${FUZZ_JOBS:-4}
case "$jobs" in
'' | *[!0-9]* | 0) echo "run-fuzz: FAIL (FUZZ_JOBS=$jobs is not a count)"; exit 1 ;;
esac
[ "$jobs" -le 4 ] || jobs=4
WS335_VECTORS=${WS335_VECTORS:-$top/build/ws335-vectors}
mkdir -p "$out/include" "$corpora"
ln -sfn "$top/include/libc/compat" "$out/include/compat"

targets="$*"
[ -n "$targets" ] || targets="der x509 pem server-hello encrypted-extensions certificate server-key-exchange msg-other record conn mlkem"
if ! command -v clang > /dev/null 2>&1; then
	echo "run-fuzz: FAIL (no clang)"
	exit 1
fi
if [ ! -f "$WS335_VECTORS/rec/limbo.vec" ]; then
	echo "run-fuzz: FAIL (no $WS335_VECTORS/rec/limbo.vec)"
	exit 1
fi
/usr/bin/python3 -I plan/ws335/tests/fuzz/seeds.py "$WS335_VECTORS/rec/limbo.vec" "$out/seeds" || {
	echo "run-fuzz: FAIL (seeds)"
	exit 1
}

modules=
for file in userland/base/libcrypto-compat/*.c userland/base/libssl-compat/*.c; do
	case "$file" in
	*-win32.c) ;;
	*) modules="$modules $file" ;;
	esac
done

# The source and the flags of each target.
target_source() {
	case "$1" in
	server-hello) echo "fuzz-msg.c -DFUZZ_MESSAGE=1" ;;
	encrypted-extensions) echo "fuzz-msg.c -DFUZZ_MESSAGE=2" ;;
	certificate) echo "fuzz-msg.c -DFUZZ_MESSAGE=3" ;;
	server-key-exchange) echo "fuzz-msg.c -DFUZZ_MESSAGE=4" ;;
	msg-other) echo "fuzz-msg.c -DFUZZ_MESSAGE=5" ;;
	*) echo "fuzz-$1.c" ;;
	esac
}

# 1. The builds (one at a time, warnings are errors).
for target in $targets; do
	set -- $(target_source "$target")
	source=plan/ws335/tests/fuzz/$1
	shift
	if [ ! -f "$source" ]; then
		echo "fuzz $target: FAIL (no $source)"
		echo "run-fuzz: FAIL"
		exit 1
	fi
	if ! timeout 900 clang -std=c89 -g -O1 -Wall -Wextra -Werror -D_DEFAULT_SOURCE "$@" \
	    -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer \
	    -I"$out/include" -I. "$source" $modules -lpthread -o "$out/fuzz-$target" > "$out/$target.build.log" 2>&1 ||
	    [ -s "$out/$target.build.log" ]; then
		head -20 "$out/$target.build.log"
		echo "fuzz $target: FAIL (build; $out/$target.build.log)"
		echo "run-fuzz: FAIL"
		exit 1
	fi
	mkdir -p "$corpora/$target" "$out/$target"
done
echo "fuzz: built $(echo $targets | wc -w) targets; each runs $seconds s, $jobs at a time"

# 2. The runs: worker w (0 to jobs - 1) runs targets w, w + jobs, w + 2 * jobs, ... one after another.
run_target() {
	(cd "$out/$1" && ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	    timeout $((seconds + 600)) "$top/$out/fuzz-$1" -max_total_time="$seconds" -max_len=65536 \
	    -rss_limit_mb=4096 -timeout=30 -print_final_stats=1 -artifact_prefix="$top/$out/$1/" \
	    "$top/$corpora/$1" "$top/$out/seeds/$1") > "$out/$1.log" 2>&1
}
worker=0
while [ $worker -lt $jobs ]; do
	(
		index=0
		for target in $targets; do
			[ $((index % jobs)) -ne $worker ] || run_target "$target"
			index=$((index + 1))
		done
	) &
	worker=$((worker + 1))
done
wait

# 3. The verdicts: a clean end, no sanitizer report, no crash input.
status=0
for target in $targets; do
	runs=$(sed -n 's/^stat::number_of_executed_units: *//p' "$out/$target.log" | tail -1)
	time=$(sed -n 's/^stat::average_exec_per_sec: *//p' "$out/$target.log" | tail -1)
	inputs=$(find "$corpora/$target" -type f | wc -l)
	crashes=$(find "$out/$target" -type f \( -name 'crash-*' -o -name 'leak-*' -o -name 'timeout-*' -o -name 'oom-*' \) | wc -l)
	if [ -z "$runs" ] || [ "$crashes" -ne 0 ] || grep -q -E '^==[0-9]+==ERROR|runtime error|^SUMMARY' "$out/$target.log"; then
		echo "fuzz $target: FAIL ($crashes saved inputs; $out/$target.log)"
		grep -m 8 -E 'ERROR|runtime error|SUMMARY|#[0-9] 0x' "$out/$target.log"
		status=1
	else
		echo "fuzz $target: PASS ($runs runs, $time per second, $inputs inputs in the corpus)"
	fi
done
if [ $status -eq 0 ]; then echo "run-fuzz: PASS"; else echo "run-fuzz: FAIL"; fi
exit $status
