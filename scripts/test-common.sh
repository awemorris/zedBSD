#!/usr/bin/env bash
# Shared helpers for reproducible zedBSD regression tests.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

if test -n "${ZEDBSD_TEST_COMMON_LOADED:-}"; then
	return 0
fi
ZEDBSD_TEST_COMMON_LOADED=1

zedbsd_test_repo()
{
	cd "$(dirname "${BASH_SOURCE[1]}")/.." && pwd
}

zedbsd_test_make_workdir()
{
	local suite="$1"
	mktemp -d "${TMPDIR:-/tmp}/zedbsd-${suite}.XXXXXX"
}

zedbsd_test_require_command()
{
	local command="$1"
	if ! command -v "$command" >/dev/null 2>&1; then
		echo "zedBSD test prerequisite missing: $command" >&2
		return 127
	fi
}

zedbsd_test_sha256()
{
	local path="$1"
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$path" | awk '{print $1}'
	elif command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$path" | awk '{print $1}'
	else
		python3 - "$path" <<'PY'
import hashlib
import pathlib
import sys

digest = hashlib.sha256()
with pathlib.Path(sys.argv[1]).open("rb") as stream:
    for block in iter(lambda: stream.read(1024 * 1024), b""):
        digest.update(block)
print(digest.hexdigest())
PY
	fi
}

# Wait for one success marker while treating an error marker, process exit, and
# timeout as distinct failures.  The caller owns and later reaps the process.
zedbsd_test_wait_for_marker()
{
	local pid="$1" log="$2" success="$3" error="$4" timeout_ms="$5"
	local elapsed=0 interval_ms=100
	while test "$elapsed" -lt "$timeout_ms"; do
		if test -f "$log" && grep -Fq "$success" "$log"; then
			return 0
		fi
		if test -n "$error" && test -f "$log" && grep -Fq "$error" "$log"; then
			return 2
		fi
		if ! kill -0 "$pid" 2>/dev/null; then
			return 3
		fi
		sleep 0.1
		elapsed=$((elapsed + interval_ms))
	done
	return 4
}

zedbsd_test_status_name()
{
	case "$1" in
	0) echo PASS ;;
	2) echo ERROR_MARKER ;;
	3) echo PROCESS_EXIT ;;
	4) echo TIMEOUT ;;
	125) echo SKIP ;;
	127) echo MISSING_TOOL ;;
	*) echo FAIL ;;
	esac
}

# Write a stable result record without requiring jq.  Details are supplied as
# key=value arguments and are encoded as strings so shell callers cannot emit
# malformed JSON.
zedbsd_test_write_result()
{
	local output="$1" suite="$2" arch="$3" status="$4" start="$5" end="$6"
	shift 6
	mkdir -p "$(dirname "$output")"
	python3 - "$output" "$suite" "$arch" "$status" "$start" "$end" "$@" <<'PY'
import json
import pathlib
import sys

output, suite, arch, status, start, end, *items = sys.argv[1:]
details = {}
for item in items:
    key, separator, value = item.partition("=")
    if not separator or not key:
        raise SystemExit(f"invalid result detail: {item!r}")
    details[key] = value
record = {
    "schema": 1,
    "suite": suite,
    "architecture": arch,
    "status": status,
    "started_utc": start,
    "finished_utc": end,
    "details": details,
}
path = pathlib.Path(output)
temporary = path.with_suffix(path.suffix + ".tmp")
temporary.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n",
                     encoding="utf-8")
temporary.replace(path)
PY
}

zedbsd_test_utc_now()
{
	date -u +%Y-%m-%dT%H:%M:%SZ
}
