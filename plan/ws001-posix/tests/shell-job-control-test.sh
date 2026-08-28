#!/usr/bin/env bash
# WS001-p014 deterministic foreground pipeline and fg ordering acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
shell_source=$repo/userland/base/sh
libedit_source=$repo/userland/base/libedit

if [[ $# -gt 1 ]]; then
	echo "usage: $0 [OUTPUT-DIRECTORY]" >&2
	exit 2
fi
for command in awk cc rg; do
	command -v "$command" >/dev/null || {
		echo "required command not found: $command" >&2
		exit 2
	}
done
if [[ $# -eq 1 ]]; then
	output=$1
	[[ ! -e $output ]] || {
		echo "output path already exists: $output" >&2
		exit 2
	}
	mkdir -p -- "$output"
else
	mkdir -p -- "$repo/plan/ws001-posix/temp"
	output=$(mktemp -d "$repo/plan/ws001-posix/temp/q023-p014-job-control.XXXXXX")
fi
output=$(cd -- "$output" && pwd)
case "$repo$output" in
*' '*) echo "test paths containing spaces are not supported" >&2; exit 2 ;;
esac

instrumented_shell=$output/sh-job-control
probe=$output/sh-job-control-probe
pty_runner=$output/sh-job-control-pty
compile_log=$output/compile.log
: >"$compile_log"

shell_sources=(
	"$shell_source/main.c"
	"$shell_source/builtins.c"
	"$shell_source/lexer.c"
	"$shell_source/expand.c"
	"$shell_source/glob.c"
	"$shell_source/vars.c"
	"$shell_source/arithmetic.c"
	"$shell_source/alias.c"
	"$libedit_source/readline.c"
)
cc -D_GNU_SOURCE -std=c11 -Wall -Wextra -Werror \
	-Wno-int-conversion -I"$repo" -I"$repo/include/uapi" \
	-I"$libedit_source" \
	"${shell_sources[@]}" "$script_dir/sh-job-control-hooks.c" \
	-Wl,--wrap=fork -Wl,--wrap=read -Wl,--wrap=write \
	-Wl,--wrap=posix_spawn -Wl,--wrap=tcsetpgrp -Wl,--wrap=kill \
	-Wl,--wrap=waitpid \
	-lm -o "$instrumented_shell" >>"$compile_log" 2>&1
cc -std=c11 -Wall -Wextra -Werror \
	"$script_dir/sh-job-control-probe.c" -o "$probe" \
	>>"$compile_log" 2>&1
cc -D_GNU_SOURCE -std=c11 -Wall -Wextra -Werror \
	"$script_dir/sh-job-control-pty.c" -lutil -o "$pty_runner" \
	>>"$compile_log" 2>&1

pipeline_events=$output/pipeline-events.log
pipeline_checkpoints=$output/pipeline-checkpoints.log
pipeline_transcript=$output/pipeline-transcript.log
: >"$pipeline_events"
: >"$pipeline_checkpoints"
P014_EVENT_LOG=$pipeline_events "$pty_runner" $'pipeline-input\n' \
	"$instrumented_shell" -c \
	"$probe pipeline $pipeline_checkpoints | /bin/cat" \
	>"$pipeline_transcript" 2>&1

awk '
function fields(    field_number, pair) {
	delete value
	for (field_number = 2; field_number <= NF; field_number++) {
		split($field_number, pair, "=")
		value[pair[1]] = pair[2]
	}
}
$1 == "FORK_CHECKPOINT" {
	fields()
	if (value["kind"] == "gate") {
		gate_count++
		gate_last = NR
		if (first_child == "")
			first_child = value["child"]
	} else {
		bad_checkpoint++
	}
}
$1 == "TCSET" {
	fields()
	if (handoff == 0 && value["target"] == first_child &&
	    value["result"] == 0)
		handoff = NR
}
$1 == "GATE_RELEASE" {
	fields()
	if (value["count"] == 2 && value["result"] == 2)
		release = NR
}
END {
	exit !(gate_count == 2 && bad_checkpoint == 0 &&
	       gate_last < handoff && handoff < release)
}
' "$pipeline_events" || {
	echo "foreground pipeline barrier ordering failed" >&2
	cat "$pipeline_events" >&2
	exit 1
}
awk '
$1 == "PIPELINE_CHECK" {
	count++
	for (field_number = 2; field_number <= NF; field_number++) {
		split($field_number, pair, "=")
		if (pair[1] == "equal" && pair[2] == 1)
			equal++
	}
}
END { exit !(count == 1 && equal == 1) }
' "$pipeline_checkpoints"
for marker in P014-PIPELINE-READY P014-PIPELINE-DONE; do
	[[ $(rg -c "$marker" "$pipeline_transcript" || true) -eq 1 ]] || {
		echo "missing or duplicate pipeline marker: $marker" >&2
		exit 1
	}
done

fg_events=$output/fg-events.log
fg_checkpoints=$output/fg-checkpoints.log
fg_transcript=$output/fg-transcript.log
: >"$fg_events"
: >"$fg_checkpoints"
fg_input=$(printf '%s fg %s\nfg\nfg-input\nexit\n' \
	"$probe" "$fg_checkpoints")
P014_EVENT_LOG=$fg_events "$pty_runner" "$fg_input"$'\n' \
	"$instrumented_shell" >"$fg_transcript" 2>&1

awk '
function fields(    field_number, pair) {
	delete value
	for (field_number = 2; field_number <= NF; field_number++) {
		split($field_number, pair, "=")
		value[pair[1]] = pair[2]
	}
}
$1 == "FORK_CHECKPOINT" {
	fields()
	if (value["kind"] == "gate")
		gate_count++
	else
		bad_checkpoint++
}
$1 == "TCSET" {
	fields()
	last_target = value["target"]
	last_result = value["result"]
	last_tcset_line = NR
}
$1 == "KILL_CONT" {
	fields()
	kill_count++
	shell = value["caller"]
	job = -value["target"]
	if (value["result"] != 0 || last_result != 0 ||
	    last_target != job || last_tcset_line >= NR)
		bad_order++
	kill_line = NR
}
END {
	exit !(gate_count == 1 && bad_checkpoint == 0 && kill_count == 1 &&
	       bad_order == 0 && kill_line < last_tcset_line &&
	       last_result == 0 && last_target == shell)
}
' "$fg_events" || {
	echo "fg did not foreground the job before SIGCONT or restore the shell" >&2
	cat "$fg_events" >&2
	exit 1
}
awk '
$1 == "FG_INITIAL_CHECK" || $1 == "FG_RESUMED_CHECK" {
	count++
	for (field_number = 2; field_number <= NF; field_number++) {
		split($field_number, pair, "=")
		if (pair[1] == "equal" && pair[2] == 1)
			equal++
	}
}
END { exit !(count == 2 && equal == 2) }
' "$fg_checkpoints"
for marker in P014-FG-READY P014-FG-DONE; do
	[[ $(rg -c "$marker" "$fg_transcript" || true) -eq 1 ]] || {
		echo "missing or duplicate fg marker: $marker" >&2
		exit 1
	}
done

fg_retry_events=$output/fg-retry-events.log
fg_retry_checkpoints=$output/fg-retry-checkpoints.log
fg_retry_transcript=$output/fg-retry-transcript.log
: >"$fg_retry_events"
: >"$fg_retry_checkpoints"
fg_retry_input=$(printf '%s fg %s\nfg\nfg\nfg-retry-input\nexit\n' \
	"$probe" "$fg_retry_checkpoints")
P014_EVENT_LOG=$fg_retry_events P014_FAIL_TCSETPGRP_AT=3 \
	"$pty_runner" "$fg_retry_input"$'\n' "$instrumented_shell" \
	>"$fg_retry_transcript" 2>&1
awk '
function fields(    field_number, pair) {
	delete value
	for (field_number = 2; field_number <= NF; field_number++) {
		split($field_number, pair, "=")
		value[pair[1]] = pair[2]
	}
}
$1 == "FORK_CHECKPOINT" {
	fields()
	if (value["kind"] == "gate")
		gate_count++
	else
		bad_checkpoint++
}
$1 == "TCSET" {
	fields()
	call = value["call"]
	if (call == 1 && value["result"] == 0) {
		job = value["target"]
		shell = value["caller"]
		call1++
	} else if (call == 2 && value["result"] == 0 &&
		   value["target"] == shell) {
		call2++
	} else if (call == 3 && value["result"] == -1 &&
		   value["injected"] == 1 && value["target"] == job) {
		call3++
	} else if (call == 4 && value["result"] == 0 &&
		   value["target"] == job) {
		call4++
		handoff_line = NR
	} else if (call == 5 && value["result"] == 0 &&
		   value["target"] == shell) {
		call5++
		restore_line = NR
	} else {
		unexpected_tcset++
	}
}
$1 == "KILL_CONT" {
	fields()
	kill_count++
	if (value["result"] != 0 || value["target"] != -job ||
	    handoff_line == 0 || NR <= handoff_line)
		bad_cont++
	cont_line = NR
}
END {
	exit !(gate_count == 1 && bad_checkpoint == 0 &&
	       call1 == 1 && call2 == 1 && call3 == 1 && call4 == 1 &&
	       call5 == 1 && unexpected_tcset == 0 && kill_count == 1 &&
	       bad_cont == 0 && handoff_line < cont_line &&
	       cont_line < restore_line)
}
' "$fg_retry_events" || {
	echo "failed fg handoff did not preserve a retryable job" >&2
	cat "$fg_retry_events" >&2
	exit 1
}
awk '
$1 == "FG_INITIAL_CHECK" || $1 == "FG_RESUMED_CHECK" {
	name = $1
	count++
	for (field_number = 2; field_number <= NF; field_number++) {
		split($field_number, pair, "=")
		if (pair[1] == "pgrp")
			group[name] = pair[2]
		if (pair[1] == "equal")
			equality[name] = pair[2]
	}
}
END {
	exit !(count == 2 && equality["FG_INITIAL_CHECK"] == 1 &&
	       equality["FG_RESUMED_CHECK"] == 1 &&
	       group["FG_INITIAL_CHECK"] == group["FG_RESUMED_CHECK"])
}
' "$fg_retry_checkpoints" || {
	echo "fg retry did not resume the original stopped process group" >&2
	cat "$fg_retry_checkpoints" >&2
	exit 1
}
[[ $(rg -c '^fg: cannot foreground process .*Input/output error' \
	"$fg_retry_transcript" || true) -eq 1 ]] || {
	echo "first fg handoff failure was not reported exactly once" >&2
	exit 1
}
for marker in P014-FG-READY P014-FG-DONE; do
	[[ $(rg -c "$marker" "$fg_retry_transcript" || true) -eq 1 ]] || {
		echo "missing or duplicate fg retry marker: $marker" >&2
		exit 1
	}
done

background_events=$output/background-events.log
background_checkpoints=$output/background-checkpoints.log
background_transcript=$output/background-transcript.log
: >"$background_events"
: >"$background_checkpoints"
background_initial=$(printf '%s background %s &' \
	"$probe" "$background_checkpoints")
background_after=$'jobs\nfg\nbackground-input\nexit\n'
P014_EVENT_LOG=$background_events \
	P014_STOP_CHECKPOINT=$background_checkpoints \
	P014_AFTER_STOP_INPUT=$background_after \
	"$pty_runner" "$background_initial"$'\n' "$instrumented_shell" \
	>"$background_transcript" 2>&1
awk '
$1 == "BACKGROUND_INITIAL_CHECK" ||
$1 == "BACKGROUND_RESUMED_CHECK" {
	for (field_number = 2; field_number <= NF; field_number++) {
		split($field_number, pair, "=")
		if (pair[1] == "equal")
			equality[$1] = pair[2]
	}
}
END {
	exit !(equality["BACKGROUND_INITIAL_CHECK"] == 0 &&
	       equality["BACKGROUND_RESUMED_CHECK"] == 1)
}
' "$background_checkpoints" || {
	echo "background reader foreground checkpoints failed" >&2
	cat "$background_checkpoints" >&2
	exit 1
}
awk '
function fields(    field_number, pair) {
	delete value
	for (field_number = 2; field_number <= NF; field_number++) {
		split($field_number, pair, "=")
		value[pair[1]] = pair[2]
	}
}
$1 == "FORK_CHECKPOINT" {
	fields()
	if (value["kind"] == "exec")
		exec_checkpoint++
	else
		bad_checkpoint++
}
$1 == "TCSET" {
	fields()
	last_target = value["target"]
	last_result = value["result"]
}
$1 == "KILL_CONT" {
	fields()
	kill_count++
	shell = value["caller"]
	job = -value["target"]
	if (last_result != 0 || last_target != job)
		bad_order++
}
END {
	exit !(exec_checkpoint == 1 && bad_checkpoint == 0 &&
	       kill_count == 1 && bad_order == 0 &&
	       last_result == 0 && last_target == shell)
}
' "$background_events" || {
	echo "background reader stole the TTY or failed fg recovery" >&2
	cat "$background_events" >&2
	exit 1
}
for marker in P014-BACKGROUND-READY P014-CONTROLLER-STOPPED \
	P014-BACKGROUND-DONE; do
	[[ $(rg -c "$marker" "$background_transcript" || true) -eq 1 ]] || {
		echo "missing or duplicate background marker: $marker" >&2
		exit 1
	}
done

nontty_events=$output/nontty-events.log
nontty_output=$output/nontty-output.txt
: >"$nontty_events"
P014_EVENT_LOG=$nontty_events "$instrumented_shell" -c \
	'/bin/printf P014-NONTTY | /bin/cat' </dev/null >"$nontty_output" 2>&1
[[ $(<"$nontty_output") == P014-NONTTY ]] || {
	echo "non-TTY pipeline output mismatch" >&2
	exit 1
}
awk '
function fields(    field_number, pair) {
	delete value
	for (field_number = 2; field_number <= NF; field_number++) {
		split($field_number, pair, "=")
		value[pair[1]] = pair[2]
	}
}
$1 == "FORK_CHECKPOINT" {
	fields()
	if (value["kind"] == "exec")
		exec_checkpoint++
	else
		bad_checkpoint++
}
$1 == "TCSET" || $1 == "GATE_RELEASE" { tty_operation++ }
END {
	exit !(exec_checkpoint == 2 && bad_checkpoint == 0 && tty_operation == 0)
}
' "$nontty_events" || {
	echo "non-TTY pipeline used an interactive gate or TTY handoff" >&2
	cat "$nontty_events" >&2
	exit 1
}

assert_no_fixture_processes()
{
	local events=$1 process
	while IFS= read -r process; do
		[[ -z $process ]] && continue
		if kill -0 "$process" 2>/dev/null; then
			echo "fixture child remains after cleanup: $process" >&2
			return 1
		fi
	done < <(awk '
		$1 == "FORK_CHECKPOINT" || $1 == "SPAWN" {
			for (field_number = 2; field_number <= NF; field_number++) {
				split($field_number, pair, "=")
				if (pair[1] == "child" && pair[2] > 0)
					print pair[2]
			}
		}
	' "$events" | sort -nu)
}

run_failure_case()
{
	local name=$1 variable=$2
	local events=$output/failure-$name-events.log
	local transcript=$output/failure-$name-transcript.log
	local input

	: >"$events"
	input=$'/bin/echo P014-INJECTED | /bin/cat\n/bin/echo P014-RECOVERY | /bin/cat\nexit\n'
	env P014_EVENT_LOG="$events" "$variable=1" \
		"$pty_runner" "$input" "$instrumented_shell" \
		>"$transcript" 2>&1
	awk '{ sub(/\r$/, "") } $0 == "P014-RECOVERY" { count++ }
	     END { exit !(count == 1) }' "$transcript" || {
		echo "shell did not recover after $name failure" >&2
		cat "$transcript" >&2
		return 1
	}
	[[ $(rg -c 'kind=gate' "$events" || true) -eq 4 ]] || {
		echo "unexpected child checkpoint during $name failure" >&2
		return 1
	}
	if rg -q 'kind=(exec|timeout|pipe-error)' "$events"; then
		echo "child escaped the gate during $name failure" >&2
		return 1
	fi
	awk '
	function fields(    field_number, pair) {
		delete value
		for (field_number = 2; field_number <= NF; field_number++) {
			split($field_number, pair, "=")
			value[pair[1]] = pair[2]
		}
	}
	$1 == "TCSET" {
		fields()
		if (value["result"] == 0) {
			last_target = value["target"]
			shell = value["caller"]
		}
	}
	END { exit !(last_target == shell) }
	' "$events" || {
		echo "shell did not recover TTY ownership after $name failure" >&2
		return 1
	}
	assert_no_fixture_processes "$events"
}

run_failure_case tcset P014_FAIL_TCSETPGRP_AT
rg -q '^TCSET .*call=1 result=-1 .*injected=1$' \
	"$output/failure-tcset-events.log"
run_failure_case gate P014_FAIL_GATE_WRITE_AT
rg -q '^GATE_RELEASE .*result=-1 injected=1$' \
	"$output/failure-gate-events.log"
run_failure_case wait P014_FAIL_WAITPID_AT
rg -q '^WAITPID .*call=1 result=-1 .*injected=1$' \
	"$output/failure-wait-events.log"

{
	printf 'test\tresult\tevidence\n'
	printf 'foreground-pipeline-barrier\tpass\tpipeline-events.log, pipeline-checkpoints.log\n'
	printf 'fg-handoff-before-cont\tpass\tfg-events.log, fg-checkpoints.log\n'
	printf 'fg-handoff-retry\tpass\tfg-retry-events.log, fg-retry-checkpoints.log\n'
	printf 'background-terminal-reader\tpass\tbackground-events.log, background-checkpoints.log\n'
	printf 'non-tty-no-handoff\tpass\tnontty-events.log\n'
	printf 'failure-cleanup\tpass\tfailure-*-events.log, failure-*-transcript.log\n'
} >"$output/results.tsv"
echo "WS001-p014 deterministic job-control acceptance: PASS ($output)"
