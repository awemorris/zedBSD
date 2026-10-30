#!/bin/sh
# Host test of the kernel's latency diagnostic (ws213-p007 K-D, kern.perf=1).
# The accounting functions are cut out of src/kern/sched.c and src/kern/vm/metadata-lock.c
# and compiled with the real struct thread and struct process against a fake
# counter, a fake tick and a fake mutex:
#  - a woken thread's wait to run, and how many threads its CPU ran first;
#  - a preempted thread's wait, kept apart from the woken ones;
#  - a switch that leaves a woken thread of at least the new thread's priority
#    waiting is counted as dropped, a lower one is not;
#  - the longest wait is kept per window and merged across CPUs;
#  - a thread taken by another CPU reports ahead -1;
#  - counts convert to microseconds without overflow at several gigahertz;
#  - the metadata lock counts entries and waits, keeps the longest wait and the
#    longest hold with the caller and how often the holder lost the CPU, and a
#    held lock leaves the window to the next report.
# Usage: plan/tools/latency/kd-host-test.sh [build-dir]  (from the tree root)
# Last line: kd-host-test: PASS.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
out=${1:-build/ws213-p007-host}
mkdir -p "$out"

# Cuts a function (its return-type line and its body) out of a source file.
cut_function() {
	awk -v name="$2" '$0 ~ "^"name"\\(" {p=1; print previous} p{print} p&&/^}/{exit} {previous=$0}' "$1"
}

# Cuts a structure definition out of a source file.
cut_struct() {
	awk -v name="$2" '$0 == "struct "name" {" {p=1} p{print} p&&/^};/{exit}' "$1"
}

grep -E '^#define SCHED_PERF_(NONE|WOKEN|PREEMPTED)[[:space:]]' src/kern/sched.c > "$out/sched-defines.inc"
{
	cut_struct src/kern/sched.c sched_perf_longest
	cut_struct src/kern/sched.c sched_perf_window
} > "$out/sched-types.inc"
for name in sched_perf_counts_to_us sched_perf_identify perf_mark_runnable_locked perf_switch_locked \
	perf_note_longest perf_merge_longest; do
	cut_function src/kern/sched.c "$name"
done > "$out/sched-functions.inc"
for name in vm_metadata_perf_take metadata_lock_measured metadata_hold_end; do
	cut_function src/kern/vm/metadata-lock.c "$name"
done > "$out/vm-functions.inc"

cat > "$out/main.c" <<'EOT'
#include "kern/sched.h"
#include "kern/thread.h"
#include "kern/process.h"
#include "kern/vm-lock.h"
#include "kern/clock.h"
#include <kern/kcrt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sched-defines.inc"
#include "sched-types.inc"

/* The part of a CPU's scheduler state the diagnostic reads. */
struct sched_cpu {
	struct sched_queue woken[SCHED_PRIOR_LEVELS];
	struct sched_perf_window perf;
	uint64_t perf_switch_count;
};

static struct sched_cpu cpus[2];
static struct sched_cpu *scheduler_cpus = cpus;
int sched_perf_active = 1;
static uint64_t sched_perf_frequency = 1000000U;
static uint64_t fake_counter = 1000U;
static uint64_t fake_ticks = 5000U;

uint64_t
sched_perf_counter(void)
{
	return fake_counter;
}

uint64_t
clock_ticks(void)
{
	return fake_ticks;
}

/* The fake tick is a millisecond. */
uint64_t
kern_ticks_to_ms(uint64_t ticks)
{
	return ticks;
}

static void perf_note_longest(struct sched_perf_longest *longest, uint64_t counts, const struct thread *thread, int ahead);
#include "sched-functions.inc"

/* The fake metadata mutex: whether it is free, and how long a wait for it takes. */
static struct mutex metadata_lock;
static int metadata_initialized = 1;
static int fake_held;
static int fake_owned;
static uint64_t fake_wait;
static struct thread *fake_current;
static struct vm_metadata_perf metadata_perf;
static uint64_t metadata_taken_at;
static const void *metadata_taken_from;
static uint64_t metadata_taken_switch_outs;

int
mutex_trylock(struct mutex *mutex)
{
	(void)mutex;
	if (fake_held)
		return 0;
	fake_owned = 1;
	return 1;
}

void
mutex_lock(struct mutex *mutex)
{
	(void)mutex;
	fake_counter += fake_wait;
	fake_held = 0;
	fake_owned = 1;
}

void
mutex_unlock(struct mutex *mutex)
{
	(void)mutex;
	fake_owned = 0;
}

int
mutex_owned(struct mutex *mutex)
{
	(void)mutex;
	return fake_owned;
}

struct thread *
thread_current(void)
{
	return fake_current;
}

#include "vm-functions.inc"

static int failures;

static void
check(int condition, const char *what)
{
	if (!condition) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

static void
make_thread(struct thread *thread, struct process *process, int tid, int pid, const char *command, int priority)
{
	memset(thread, 0, sizeof(*thread));
	memset(process, 0, sizeof(*process));
	thread->tid = tid;
	thread->proc = process;
	thread->sched.priority = priority;
	process->pid = pid;
	snprintf(process->command, sizeof(process->command), "%s", command);
}

int
main(void)
{
	static struct thread input, render, worker, idle;
	static struct process compositor, client, kernel;
	struct sched_perf_longest total;
	struct vm_metadata_perf window;
	uint64_t us;
	int taken;

	make_thread(&input, &compositor, 41, 40, "wayland-compositor-long-name", SCHED_PRIORITY_DEFAULT);
	make_thread(&render, &compositor, 42, 40, "wayland-compositor-long-name", SCHED_PRIORITY_DEFAULT);
	make_thread(&worker, &kernel, 7, 0, "", SCHED_PRIORITY_DEFAULT);
	make_thread(&idle, &kernel, 1, 0, "", SCHED_PRIOR_LOW);
	idle.flags = THREAD_FLAG_IDLE;
	(void)client;

	/* A woken thread waits while two others run first. */
	perf_mark_runnable_locked(&cpus[0], &input, SCHED_PERF_WOKEN);
	fake_counter += 3000U;
	perf_switch_locked(&cpus[0], &idle, &render);
	fake_counter += 10000U;
	perf_switch_locked(&cpus[0], &render, &worker);
	fake_counter += 17000U;
	fake_ticks = 5030U;
	perf_switch_locked(&cpus[0], &worker, &input);
	check(cpus[0].perf.woken == 1, "one woken run");
	check(cpus[0].perf.woken_longest.counts == 30000U, "woken wait is 30 ms");
	check(cpus[0].perf.woken_longest.ahead == 2, "two threads ran first");
	check(cpus[0].perf.woken_longest.who.pid == 40 && cpus[0].perf.woken_longest.who.tid == 41, "woken thread named");
	check(strcmp(cpus[0].perf.woken_longest.who.name, "wayland-composi") == 0, "name cut to 15");
	check(cpus[0].perf.woken_longest.end_ms == kern_ticks_to_ms(5030U), "end time kept");
	check(input.sched.perf_runnable_at == 0, "stamp cleared after the run");
	check(cpus[0].perf.switches == 3, "three switches");
	check(render.sched.perf_switch_outs == 1 && worker.sched.perf_switch_outs == 1, "switch-outs counted");

	/* A shorter woken wait does not replace the longest. */
	perf_mark_runnable_locked(&cpus[0], &render, SCHED_PERF_WOKEN);
	fake_counter += 1000U;
	perf_switch_locked(&cpus[0], &input, &render);
	check(cpus[0].perf.woken == 2 && cpus[0].perf.woken_longest.counts == 30000U, "shorter wait kept out");

	/* A preempted thread is measured apart. */
	perf_mark_runnable_locked(&cpus[0], &render, SCHED_PERF_PREEMPTED);
	fake_counter += 12000U;
	perf_switch_locked(&cpus[0], &worker, &render);
	check(cpus[0].perf.preempted == 1 && cpus[0].perf.preempted_longest.counts == 12000U, "preempted wait");
	check(cpus[0].perf.preempted_longest.ahead == 0, "nothing ran before it");

	/* A woken thread of the same priority left waiting is a dropped mark; a lower one is not. */
	cpus[0].woken[SCHED_PRIORITY_DEFAULT].head = &worker;
	perf_switch_locked(&cpus[0], &render, &input);
	check(cpus[0].perf.dropped == 1, "same priority left waiting is dropped");
	cpus[0].woken[SCHED_PRIORITY_DEFAULT].head = NULL;
	cpus[0].woken[SCHED_PRIOR_LOW].head = &worker;
	perf_switch_locked(&cpus[0], &input, &render);
	check(cpus[0].perf.dropped == 1, "lower priority is not dropped");
	cpus[0].woken[SCHED_PRIOR_LOW].head = NULL;

	/* Keeping the same thread is not a switch. */
	perf_switch_locked(&cpus[0], &render, &render);
	check(cpus[0].perf.switches == 7, "no switch to itself");

	/* A thread taken by another CPU reports ahead -1, and the longest merges across CPUs. */
	perf_mark_runnable_locked(&cpus[0], &worker, SCHED_PERF_WOKEN);
	fake_counter += 45000U;
	perf_switch_locked(&cpus[1], &idle, &worker);
	check(cpus[1].perf.woken_longest.ahead == -1, "stolen thread ahead -1");
	memset(&total, 0, sizeof(total));
	perf_merge_longest(&total, &cpus[0].perf.woken_longest);
	perf_merge_longest(&total, &cpus[1].perf.woken_longest);
	check(total.counts == 45000U && total.who.tid == 7 && total.who.pid == 0, "merged longest");

	/* A cleared stamp is not measured. */
	perf_mark_runnable_locked(&cpus[0], &input, SCHED_PERF_WOKEN);
	perf_mark_runnable_locked(&cpus[0], &input, SCHED_PERF_NONE);
	perf_switch_locked(&cpus[0], &render, &input);
	check(cpus[0].perf.woken == 2, "cleared stamp not measured");

	/* Counts convert without overflow at 3 GHz. */
	sched_perf_frequency = 3000000000ULL;
	us = sched_perf_counts_to_us(3000000000ULL * 3600ULL + 1500000000ULL);
	check(us == 3600500000ULL, "an hour and a half second at 3 GHz");
	sched_perf_frequency = 1000000U;

	/* The metadata lock: a free entry and a hold of 5 ms over two switch-outs. */
	fake_current = &input;
	metadata_lock_measured(&input, (const void *)0x1000);
	input.sched.perf_switch_outs += 2;
	fake_counter += 5000U;
	metadata_hold_end();
	mutex_unlock(&metadata_lock);
	check(metadata_perf.takes == 1 && metadata_perf.waits == 0, "free entry counts no wait");
	check(metadata_perf.hold_counts == 5000U && metadata_perf.hold_switched == 2, "hold and switch-outs");
	check(metadata_perf.hold_from == (const void *)0x1000 && metadata_perf.holder.tid == 41, "holder and caller");

	/* A held entry waits 7 ms. */
	fake_current = &render;
	fake_held = 1;
	fake_wait = 7000U;
	metadata_lock_measured(&render, (const void *)0x2000);
	fake_counter += 100U;
	metadata_hold_end();
	mutex_unlock(&metadata_lock);
	check(metadata_perf.takes == 2 && metadata_perf.waits == 1, "held entry counts a wait");
	check(metadata_perf.wait_counts == 7000U && metadata_perf.wait_from == (const void *)0x2000, "wait and caller");
	check(metadata_perf.waiter.tid == 42, "waiter named");
	check(metadata_perf.hold_counts == 5000U, "short hold kept out");

	/* A held lock leaves the window to the next report; a free one hands it over and starts anew. */
	fake_held = 1;
	taken = vm_metadata_perf_take(&window);
	check(taken == 0 && metadata_perf.takes == 2, "held lock keeps the window");
	fake_held = 0;
	taken = vm_metadata_perf_take(&window);
	check(taken == 1 && window.takes == 2 && metadata_perf.takes == 0, "free lock hands the window over");
	check(fake_owned == 0, "lock released after the take");

	if (failures != 0) {
		printf("kd-host-test: FAIL (%d)\n", failures);
		return 1;
	}
	printf("kd-host-test: PASS\n");
	return 0;
}
EOT

# The kernel's own ABI headers (KERN_UAPI_NATIVE), plainly and with ASan and UBSan.
for mode in plain sanitize; do
	extra=
	if [ "$mode" = sanitize ]; then
		extra='-fsanitize=address,undefined -fno-omit-frame-pointer'
	fi
	# shellcheck disable=SC2086
	cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -DKERN_USER_ABI_LP64 -DKERN_UAPI_NATIVE \
		-I. -Iinclude -Iinclude/libc -Iinclude/uapi -Isrc -I"$out" $extra "$out/main.c" -o "$out/kd-host-test-$mode"
	UBSAN_OPTIONS=halt_on_error=1 timeout 60 "$out/kd-host-test-$mode"
done
