/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The System Monitor's sources of frames (design.md section 1.5): the
 * simulation, which makes plausible values that move (section 1.6), and a
 * recording played back for the tests (plan/ws134/tests/replay/).  The
 * system itself, through libkeiland, comes later (ws134-p013).
 *
 * The simulation is a function of its seed and the times it is asked
 * for: the same seed and the same times give the same frames, which the
 * tests depend on.
 *
 * A recording is text, one record a line:
 *
 *	# a comment
 *	info host=NAME cpus=N gpus=N memory=BYTES swap=BYTES gpu0=NAME gpu0memory=BYTES
 *	frame t=MS cpu=SHARE cores=S,S,... used=BYTES cache=BYTES available=BYTES swap=BYTES
 *	      rx=B/S tx=B/S read=B/S write=B/S latency=MS gpu0=SHARE gpu0used=BYTES
 *	      gpu0temp=C gpu0power=W simulated=BITS
 *
 * A field left out of a frame is not valid in it.
 */

#include "monitor.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The machine the simulation pretends to be. */
#define SIM_MEMORY		(8ULL << 30)
#define SIM_SWAP		(2ULL << 30)
#define SIM_GPU_MEMORY		(4ULL << 30)

/* A spike on average every 40 seconds, an excursion over a threshold every 3 minutes. */
#define SIM_SPIKE_MEAN_MS	40000U
#define SIM_EXCURSION_MEAN_MS	180000U

/* The longest recording read, in bytes. */
#define REPLAY_MAX		(4U << 20)

/* The simulation's targets: what a spike or an excursion lifts. */
enum sim_target {
	SIM_CPU,
	SIM_NETWORK,
	SIM_DISK,
	SIM_GPU,
	SIM_MEMORY_PRESSURE,
	SIM_TARGETS
};

static uint64_t sim_random(struct sm_sim *sim);
static double sim_uniform(struct sm_sim *sim);
static double sim_wave(const struct sm_sim *sim, unsigned index, double seconds, double period);
static double sim_noise(struct sm_sim *sim, unsigned index);
static double sim_spike(const struct sm_sim *sim, unsigned target, uint64_t time_ms);
static int sim_excursion(const struct sm_sim *sim, unsigned target, uint64_t time_ms);
static void sim_schedule(struct sm_sim *sim, uint64_t time_ms);
static double clamp(double value, double low, double high);
static int replay_line(struct sm_replay *replay, char *line, size_t size);
static int replay_info(char *line, struct sm_info *info);
static int replay_frame(char *line, struct sm_frame *frame);
static void replay_value(struct sm_frame *frame, unsigned field, const char *value);
static void replay_gpu(struct sm_frame *frame, unsigned gpu, unsigned field, const char *value);
static int replay_cores(struct sm_frame *frame, char *value);
static int name_index(const char *name, const char *const *names, unsigned count);
static int gpu_field(const char *name, const char *const *suffixes, unsigned count, unsigned *gpu);
static char *field_next(char **cursor, char **value);
static uint64_t field_u64(const char *value);

/*
 * Opens the simulation: a machine of a number of CPUs and GPUs, frames
 * every period_ms, values from the seed; calm leaves out the excursions
 * over a threshold.
 */
int
sm_source_open_sim(
	struct sm_source *source,
	uint64_t seed,
	unsigned period_ms,
	unsigned cpus,
	unsigned gpus,
	int calm)
{
	unsigned index;
	char host[SM_NAME_MAX];
	int error;

	/* Nothing yet. */
	memset(source, 0, sizeof(*source));
	source->kind = SM_SOURCE_SIM;
	source->period_ms = period_ms;

	/* The machine: its name, CPUs, memory and GPUs. */
	error = gethostname(host, sizeof(host));
	if (error != 0 || host[0] == '\0')
		strcpy(host, "zedbsd");
	host[sizeof(host) - 1U] = '\0';
	strcpy(source->info.host, host);
	if (cpus == 0U)
		cpus = 4U;
	if (cpus > SM_CPU_MAX)
		cpus = SM_CPU_MAX;
	if (gpus > SM_GPU_MAX)
		gpus = SM_GPU_MAX;
	source->info.cpu_count = cpus;
	source->info.gpu_count = gpus;
	source->info.memory_total = SIM_MEMORY;
	source->info.swap_total = SIM_SWAP;
	/*
	 * The GPUs' names are generic (2026-10-03 user: no "sim" in what is
	 * shown): the first is the integrated one (the drawing device's name
	 * replaces it, main.c), the others discrete.  No real model is named.
	 */
	for (index = 0; index < gpus; index++) {
		if (index == 0U)
			(void)snprintf(source->info.gpu_name[index], SM_NAME_MAX, "Integrated Graphics");
		else if (gpus == 2U)
			(void)snprintf(source->info.gpu_name[index], SM_NAME_MAX, "Discrete Graphics");
		else
			(void)snprintf(source->info.gpu_name[index], SM_NAME_MAX, "Discrete Graphics %u", index);
		source->info.gpu_memory_total[index] = SIM_GPU_MEMORY;
	}

	/* The generator and the waves' phases from the seed. */
	source->sim.seed = seed;
	source->sim.random = seed * 2862933555777941757ULL + 3037000493ULL;
	if (source->sim.random == 0U)
		source->sim.random = 1;
	for (index = 0; index < 8U; index++)
		source->sim.phase[index] = sim_uniform(&source->sim) * 6.283185307179586;
	source->sim.calm = calm != 0;
	source->sim.gpu_celsius = 42.0;

	/* The first spike, excursion and hot core. */
	sim_schedule(&source->sim, 0);
	source->next_ms = 0;

	/* Succeeded: the simulation runs. */
	return 0;
}

/*
 * Opens a recording to play, a frame each period_ms of its own times.
 */
int
sm_source_open_replay(
	struct sm_source *source,
	const char *path,
	unsigned period_ms)
{
	FILE *file;
	size_t length;
	struct sm_frame frame;
	int status;

	/* Nothing yet. */
	memset(source, 0, sizeof(*source));
	source->kind = SM_SOURCE_REPLAY;
	source->period_ms = period_ms;

	/* The whole file. */
	file = fopen(path, "r");
	if (file == NULL)
		return errno;
	source->replay.text = malloc(REPLAY_MAX + 1U);
	if (source->replay.text == NULL) {
		fclose(file);
		return ENOMEM;
	}

	/* Its text, as much as the buffer holds. */
	length = fread(source->replay.text, 1, REPLAY_MAX, file);
	fclose(file);
	source->replay.text[length] = '\0';
	source->replay.length = length;

	/* The info line comes before the frames; the first frame is kept for take. */
	status = sm_replay_parse(&source->replay, &source->info, &frame);
	if (status < 0) {
		sm_source_close(source);
		return EINVAL;
	}

	/* A frame read is the first to play. */
	if (status > 0) {
		source->replay.pending = frame;
		source->replay.have_pending = 1;
	}

	/* Succeeded: frames can be taken. */
	return 0;
}

/*
 * Releases what a source holds.
 */
void
sm_source_close(
	struct sm_source *source)
{
	/* The recording's text. */
	free(source->replay.text);
	source->replay.text = NULL;
}

/*
 * Gives the next frame when it is due at a time (milliseconds since the
 * monitor started); returns 1 with a frame, 0 when none is due yet.
 */
int
sm_source_take(
	struct sm_source *source,
	uint64_t now_ms,
	struct sm_frame *frame)
{
	struct sm_frame next;
	int status;

	/* The simulation: one frame each period, at the period's time. */
	if (source->kind == SM_SOURCE_SIM) {
		if (now_ms < source->next_ms)
			return 0;
		sm_sim_frame(&source->sim, &source->info, source->next_ms, frame);
		source->next_ms += source->period_ms;
		return 1;
	}

	/* A recording: its next frame once its time has come. */
	if (!source->replay.have_pending || now_ms < source->replay.pending.time_ms)
		return 0;
	*frame = source->replay.pending;
	source->replay.have_pending = 0;

	/* The one after it, read ahead. */
	status = sm_replay_parse(&source->replay, &source->info, &next);
	if (status > 0) {
		source->replay.pending = next;
		source->replay.have_pending = 1;
	}

	/* Succeeded: a frame. */
	return 1;
}

/*
 * Reads a recording up to its next frame, taking the info lines on the
 * way; returns 1 with a frame, 0 at the end, -1 on a malformed line.
 */
int
sm_replay_parse(
	struct sm_replay *replay,
	struct sm_info *info,
	struct sm_frame *frame)
{
	char line[4096];
	int status;
	int match;

	/* Line after line until a frame or the end. */
	for (;;) {
		status = replay_line(replay, line, sizeof(line));
		if (status == 0) {
			replay->ended = 1;
			return 0;
		}

		/* An info line describes the machine. */
		match = strncmp(line, "info ", 5);
		if (match == 0) {
			status = replay_info(line + 5, info);
			if (status != 0)
				return -1;
			continue;
		}

		/* A frame line is the frame. */
		match = strncmp(line, "frame ", 6);
		if (match == 0) {
			status = replay_frame(line + 6, frame);
			if (status != 0)
				return -1;
			return 1;
		}

		/* Blank lines and comments are passed; anything else is an error. */
		if (line[0] != '\0' && line[0] != '#')
			return -1;
	}
}

/*
 * Makes the simulation's frame at a time (design.md section 1.6): slow
 * waves, low-passed noise, spikes now and then, and (unless calm) an
 * excursion over a threshold every few minutes.  Every field is marked
 * simulated.
 */
void
sm_sim_frame(
	struct sm_sim *sim,
	const struct sm_info *info,
	uint64_t time_ms,
	struct sm_frame *frame)
{
	double seconds;
	double cpu;
	double core;
	double used;
	double cache;
	double network;
	double disk;
	double gpu;
	double target_celsius;
	unsigned index;
	int excursion_cpu;
	int excursion_memory;
	int excursion_disk;
	int excursion_gpu;

	/* A new frame at the time, every field simulated. */
	memset(frame, 0, sizeof(*frame));
	frame->time_ms = time_ms;
	frame->valid = SM_HAVE_CPU | SM_HAVE_MEMORY | SM_HAVE_SWAP | SM_HAVE_NETWORK | SM_HAVE_DISK | SM_HAVE_DISK_LATENCY |
	    SM_HAVE_CPU_FREQUENCY;
	if (info->gpu_count != 0U)
		frame->valid |= SM_HAVE_GPU_BUSY | SM_HAVE_GPU_MEMORY | SM_HAVE_GPU_TEMPERATURE | SM_HAVE_GPU_POWER;
	frame->simulated = frame->valid;
	seconds = (double)time_ms / 1000.0;

	/* The next spike, excursion and hot core once the last ones are over, and the excursions holding now. */
	sim_schedule(sim, time_ms);
	excursion_cpu = sim_excursion(sim, SIM_CPU, time_ms);
	excursion_memory = sim_excursion(sim, SIM_MEMORY_PRESSURE, time_ms);
	excursion_disk = sim_excursion(sim, SIM_DISK, time_ms);
	excursion_gpu = sim_excursion(sim, SIM_GPU, time_ms);

	/* The CPU: a base, two slow waves, noise and the spikes. */
	cpu = 0.24 + 0.08 * sim_wave(sim, 0, seconds, 47.0) + 0.05 * sim_wave(sim, 1, seconds, 13.0) + 0.04 * sim_noise(sim, 0);
	cpu += 0.45 * sim_spike(sim, SIM_CPU, time_ms);
	if (excursion_cpu)
		cpu = 0.84 + 0.05 * sim_noise(sim, 1);
	frame->cpu = clamp(cpu, 0.01, 1.0);
	frame->cpu_mhz = 1800.0 + 1600.0 * frame->cpu;

	/* Each CPU: the whole, a bias of its own, and the hot core's extra. */
	for (index = 0; index < info->cpu_count; index++) {
		core = frame->cpu + 0.12 * sin(seconds / 9.0 + (double)index * 1.7 + sim->phase[2]) + 0.05 * sim_noise(sim, 2);
		if (index == sim->hot_core % info->cpu_count)
			core += 0.35;
		frame->cpu_core[index] = clamp(core, 0.0, 1.0);
	}

	/* The memory: in use 25 to 45%, the cache against it, a little swap now and then. */
	used = 0.35 + 0.08 * sim_wave(sim, 3, seconds, 83.0) + 0.02 * sim_noise(sim, 3) + 0.15 * sim_spike(sim, SIM_CPU, time_ms);
	if (excursion_memory)
		used = 0.82;
	used = clamp(used, 0.05, 0.92);
	cache = clamp(0.62 - used, 0.04, 0.4) * 0.8;
	frame->memory_used = (uint64_t)(used * (double)info->memory_total);
	frame->memory_cache = (uint64_t)(cache * (double)info->memory_total);
	frame->memory_available = info->memory_total - frame->memory_used - frame->memory_cache / 4U;
	frame->swap_used = (uint64_t)(clamp(0.02 + 0.03 * sim_wave(sim, 4, seconds, 120.0), 0.0, 1.0) * (double)info->swap_total);

	/* The network: a few hundred KB/s, spikes to some MB/s, sending an eighth of it. */
	network = 380000.0 * exp(0.9 * sim_wave(sim, 5, seconds, 31.0) + 0.3 * sim_noise(sim, 4));
	network += 3200000.0 * sim_spike(sim, SIM_NETWORK, time_ms);
	frame->rx_rate = network;
	frame->tx_rate = network * (0.12 + 0.04 * sim_wave(sim, 6, seconds, 17.0));

	/* The disks: a few MB/s, write bursts to tens of MB/s with the latency rising. */
	disk = 1600000.0 * exp(0.8 * sim_wave(sim, 7, seconds, 23.0) + 0.4 * sim_noise(sim, 5));
	frame->read_rate = disk;
	frame->write_rate = disk * 0.4 + 28000000.0 * sim_spike(sim, SIM_DISK, time_ms);
	frame->disk_latency_ms = 0.6 + 0.3 * sim_wave(sim, 6, seconds, 29.0) + 0.1 * sim_noise(sim, 6) + 5.0 * sim_spike(sim, SIM_DISK, time_ms);
	if (excursion_disk)
		frame->disk_latency_ms = 7.5 + sim_noise(sim, 7);
	frame->disk_latency_ms = clamp(frame->disk_latency_ms, 0.1, 200.0);

	/* The GPUs: use that follows the CPU's spikes a little, the temperature lagging behind it. */
	for (index = 0; index < info->gpu_count; index++) {
		gpu = 0.18 + 0.1 * sin(seconds / 21.0 + sim->phase[index % 8U]) + 0.04 * sim_noise(sim, 7) +
		    0.3 * sim_spike(sim, SIM_CPU, time_ms) + 0.55 * sim_spike(sim, SIM_GPU, time_ms);
		if (index > 0U)
			gpu *= 0.6;
		frame->gpu_busy[index] = clamp(gpu, 0.0, 1.0);
		frame->gpu_memory_used[index] = (uint64_t)((0.35 + 0.3 * frame->gpu_busy[index]) * (double)info->gpu_memory_total[index]);
		frame->gpu_watts[index] = 6.0 + 42.0 * frame->gpu_busy[index];
	}

	/* The first GPU's temperature: a first-order lag (20 s) towards what its use asks for. */
	if (info->gpu_count != 0U) {
		target_celsius = 38.0 + 34.0 * frame->gpu_busy[0];
		if (excursion_gpu)
			target_celsius = 84.0;
		sim->gpu_celsius += (target_celsius - sim->gpu_celsius) * 0.05;
		frame->gpu_celsius[0] = sim->gpu_celsius;
		for (index = 1; index < info->gpu_count; index++)
			frame->gpu_celsius[index] = 36.0 + 20.0 * frame->gpu_busy[index];
	}
}

/* The next number of the generator (xorshift64*). */
static uint64_t
sim_random(
	struct sm_sim *sim)
{
	uint64_t value;

	/* Shifts and the multiplier. */
	value = sim->random;
	value ^= value >> 12;
	value ^= value << 25;
	value ^= value >> 27;
	sim->random = value;

	/* Succeeded: the number. */
	return value * 2685821657736338717ULL;
}

/* A number between 0 and 1. */
static double
sim_uniform(
	struct sm_sim *sim)
{
	/* The top 53 bits. */
	return (double)(sim_random(sim) >> 11) / 9007199254740992.0;
}

/* A slow wave between -1 and 1 of a period in seconds, with a phase of its own. */
static double
sim_wave(
	const struct sm_sim *sim,
	unsigned index,
	double seconds,
	double period)
{
	/* Two sines of near periods, so that it does not repeat soon. */
	return 0.7 * sin(seconds * 6.283185307179586 / period + sim->phase[index]) +
	    0.3 * sin(seconds * 6.283185307179586 / (period * 2.37) + sim->phase[(index + 3U) % 8U]);
}

/* Noise between about -1 and 1, low-passed so that it wanders rather than jumps. */
static double
sim_noise(
	struct sm_sim *sim,
	unsigned index)
{
	/* The last value moves a little towards a new random one. */
	sim->noise[index] += (sim_uniform(sim) * 2.0 - 1.0 - sim->noise[index]) * 0.35;

	/* Succeeded: the noise's new value. */
	return sim->noise[index];
}

/* The share (0 to 1) of a spike on a target at a time: rising in about a second, falling over its length. */
static double
sim_spike(
	const struct sm_sim *sim,
	unsigned target,
	uint64_t time_ms)
{
	double age;
	double length;

	/* No spike on this target now. */
	if (sim->spike_target != target ||
	    time_ms < sim->spike_start_ms ||
	    time_ms >= sim->spike_start_ms + sim->spike_length_ms)
		return 0.0;

	/* Up in the first second, then down over the rest. */
	age = (double)(time_ms - sim->spike_start_ms);
	length = (double)sim->spike_length_ms;
	if (age < 1200.0)
		return sim->spike_height * age / 1200.0;

	/* Succeeded: the falling part. */
	return sim->spike_height * (1.0 - (age - 1200.0) / (length - 1200.0));
}

/* Whether an excursion over a threshold holds a target at a time. */
static int
sim_excursion(
	const struct sm_sim *sim,
	unsigned target,
	uint64_t time_ms)
{
	/* A calm simulation has none. */
	if (sim->calm)
		return 0;

	/* The excursion in progress on this target. */
	if (sim->excursion_target != target || time_ms < sim->excursion_start_ms)
		return 0;
	if (time_ms >= sim->excursion_start_ms + sim->excursion_length_ms)
		return 0;

	/* Succeeded: it holds. */
	return 1;
}

/* Plans the next spike, excursion and hot core once the last ones are over. */
static void
sim_schedule(
	struct sm_sim *sim,
	uint64_t time_ms)
{
	/* A new spike: when, on what, how high and how long. */
	if (time_ms >= sim->next_spike_ms) {
		sim->spike_start_ms = time_ms + (uint64_t)(sim_uniform(sim) * 4000.0);
		sim->spike_length_ms = 6000U + (uint64_t)(sim_uniform(sim) * 10000.0);
		sim->spike_height = 0.5 + 0.5 * sim_uniform(sim);
		sim->spike_target = (unsigned)(sim_random(sim) % SIM_MEMORY_PRESSURE);
		sim->next_spike_ms = sim->spike_start_ms + sim->spike_length_ms +
		    (uint64_t)(sim_uniform(sim) * 2.0 * SIM_SPIKE_MEAN_MS);
	}

	/* A new excursion: 20 to 40 seconds over a threshold, a few minutes from now. */
	if (time_ms >= sim->next_excursion_ms) {
		sim->excursion_start_ms = time_ms + 60000U + (uint64_t)(sim_uniform(sim) * 2.0 * SIM_EXCURSION_MEAN_MS);
		sim->excursion_length_ms = 20000U + (uint64_t)(sim_uniform(sim) * 20000.0);
		sim->excursion_target = (unsigned)(sim_random(sim) % SIM_TARGETS);
		if (sim->excursion_target == SIM_NETWORK)
			sim->excursion_target = SIM_CPU;
		sim->next_excursion_ms = sim->excursion_start_ms + sim->excursion_length_ms;
	}

	/* The hot core moves every 20 to 60 seconds. */
	if (time_ms >= sim->next_hot_core_ms) {
		sim->hot_core = (unsigned)(sim_random(sim) % SM_CPU_MAX);
		sim->next_hot_core_ms = time_ms + 20000U + (uint64_t)(sim_uniform(sim) * 40000.0);
	}
}

/* A value kept between two bounds. */
static double
clamp(
	double value,
	double low,
	double high)
{
	/* Below, above, or as it is. */
	if (value < low)
		return low;
	if (value > high)
		return high;

	/* Succeeded: the value within its bounds. */
	return value;
}

/* Copies the next line of a recording (without its end); returns 0 at the end. */
static int
replay_line(
	struct sm_replay *replay,
	char *line,
	size_t size)
{
	size_t length;
	char character;

	/* The end of the text. */
	if (replay->offset >= replay->length)
		return 0;

	/* The characters up to the line's end, as many as fit. */
	length = 0;
	while (replay->offset < replay->length) {
		character = replay->text[replay->offset++];
		if (character == '\n')
			break;
		if (character != '\r' && length + 1U < size)
			line[length++] = character;
	}

	/* The line ends there. */
	line[length] = '\0';

	/* Succeeded: a line. */
	return 1;
}

/* Reads an info line's fields into the machine's description. */
static int
replay_info(
	char *line,
	struct sm_info *info)
{
	static const char *const names[] = { "host", "cpus", "gpus", "memory", "swap" };
	static const char *const gpu_names[] = { "", "memory" };
	char *cursor;
	char *name;
	char *value;
	unsigned gpu;
	int field;

	/* Each name=value. */
	cursor = line;
	for (;;) {
		name = field_next(&cursor, &value);
		if (name == NULL)
			break;

		/* A GPU's name or memory. */
		field = gpu_field(name, gpu_names, 2U, &gpu);
		if (field == 0) {
			(void)snprintf(info->gpu_name[gpu], SM_NAME_MAX, "%s", value);
			continue;
		}

		/* Its memory. */
		if (field == 1) {
			info->gpu_memory_total[gpu] = field_u64(value);
			continue;
		}

		/* The machine's fields. */
		field = name_index(name, names, 5U);
		switch (field) {
		case 0:
			(void)snprintf(info->host, sizeof(info->host), "%s", value);
			break;
		case 1:
			info->cpu_count = (unsigned)field_u64(value);
			if (info->cpu_count > SM_CPU_MAX)
				info->cpu_count = SM_CPU_MAX;
			break;
		case 2:
			info->gpu_count = (unsigned)field_u64(value);
			if (info->gpu_count > SM_GPU_MAX)
				info->gpu_count = SM_GPU_MAX;
			break;
		case 3:
			info->memory_total = field_u64(value);
			break;
		case 4:
			info->swap_total = field_u64(value);
			break;
		default:
			return -1;
		}
	}

	/* Succeeded: the machine. */
	return 0;
}

/* Reads a frame line's fields into a frame. */
static int
replay_frame(
	char *line,
	struct sm_frame *frame)
{
	static const char *const names[] = {
		"t", "cpu", "cores", "mhz", "used", "cache", "available", "swap", "rx", "tx", "read", "write", "latency", "simulated"
	};
	static const char *const gpu_names[] = { "", "used", "temp", "power" };
	char *cursor;
	char *name;
	char *value;
	int field;
	int status;
	unsigned gpu;

	/* An empty frame. */
	memset(frame, 0, sizeof(*frame));

	/* Each name=value. */
	cursor = line;
	for (;;) {
		name = field_next(&cursor, &value);
		if (name == NULL)
			break;

		/* A GPU's fields. */
		field = gpu_field(name, gpu_names, 4U, &gpu);
		if (field >= 0) {
			replay_gpu(frame, gpu, (unsigned)field, value);
			continue;
		}

		/* The frame's other fields. */
		field = name_index(name, names, 14U);
		if (field < 0)
			return -1;
		if (field == 2) {
			status = replay_cores(frame, value);
			if (status != 0)
				return -1;
			continue;
		}

		/* A plain field. */
		replay_value(frame, (unsigned)field, value);
	}

	/* The simulated fields are only ones the frame has. */
	frame->simulated &= frame->valid;

	/* Succeeded: the frame is read. */
	return 0;
}

/* Sets one of a frame's plain fields (its index in replay_frame's names) from its text. */
static void
replay_value(
	struct sm_frame *frame,
	unsigned field,
	const char *value)
{
	/* Each field and the part of the frame it makes valid. */
	switch (field) {
	case 0:
		frame->time_ms = field_u64(value);
		break;
	case 1:
		frame->cpu = strtod(value, NULL);
		frame->valid |= SM_HAVE_CPU;
		break;
	case 3:
		frame->cpu_mhz = strtod(value, NULL);
		frame->valid |= SM_HAVE_CPU_FREQUENCY;
		break;
	case 4:
		frame->memory_used = field_u64(value);
		frame->valid |= SM_HAVE_MEMORY;
		break;
	case 5:
		frame->memory_cache = field_u64(value);
		break;
	case 6:
		frame->memory_available = field_u64(value);
		break;
	case 7:
		frame->swap_used = field_u64(value);
		frame->valid |= SM_HAVE_SWAP;
		break;
	case 8:
		frame->rx_rate = strtod(value, NULL);
		frame->valid |= SM_HAVE_NETWORK;
		break;
	case 9:
		frame->tx_rate = strtod(value, NULL);
		break;
	case 10:
		frame->read_rate = strtod(value, NULL);
		frame->valid |= SM_HAVE_DISK;
		break;
	case 11:
		frame->write_rate = strtod(value, NULL);
		break;
	case 12:
		frame->disk_latency_ms = strtod(value, NULL);
		frame->valid |= SM_HAVE_DISK_LATENCY;
		break;
	default:
		frame->simulated = (unsigned)strtoul(value, NULL, 0);
		break;
	}
}

/* Sets one of a GPU's fields (busy, used, temp, power) from its text. */
static void
replay_gpu(
	struct sm_frame *frame,
	unsigned gpu,
	unsigned field,
	const char *value)
{
	/* Each field and the part of the frame it makes valid. */
	switch (field) {
	case 0:
		frame->gpu_busy[gpu] = strtod(value, NULL);
		frame->valid |= SM_HAVE_GPU_BUSY;
		break;
	case 1:
		frame->gpu_memory_used[gpu] = field_u64(value);
		frame->valid |= SM_HAVE_GPU_MEMORY;
		break;
	case 2:
		frame->gpu_celsius[gpu] = strtod(value, NULL);
		frame->valid |= SM_HAVE_GPU_TEMPERATURE;
		break;
	default:
		frame->gpu_watts[gpu] = strtod(value, NULL);
		frame->valid |= SM_HAVE_GPU_POWER;
		break;
	}
}

/* Reads the CPUs' shares, separated by commas; returns 0 or -1 on a malformed list. */
static int
replay_cores(
	struct sm_frame *frame,
	char *value)
{
	char *end;
	unsigned core;

	/* Each share in turn. */
	core = 0;
	while (*value != '\0' && core < SM_CPU_MAX) {
		frame->cpu_core[core++] = strtod(value, &end);
		if (end == value)
			return -1;
		value = end;
		if (*value == ',')
			value++;
	}

	/* Succeeded: the shares. */
	return 0;
}

/* The index of a name in a table, or -1. */
static int
name_index(
	const char *name,
	const char *const *names,
	unsigned count)
{
	unsigned index;
	int match;

	/* Each name in turn. */
	for (index = 0; index < count; index++) {
		match = strcmp(name, names[index]);
		if (match == 0)
			return (int)index;
	}

	/* None. */
	return -1;
}

/* For a GPU's field ("gpu0", "gpu1used"...): the GPU's number and the suffix's index in a table, or -1. */
static int
gpu_field(
	const char *name,
	const char *const *suffixes,
	unsigned count,
	unsigned *gpu)
{
	int match;

	/* "gpu" and a digit of a GPU there may be. */
	match = strncmp(name, "gpu", 3);
	if (match != 0)
		return -1;
	if (name[3] < '0' || name[3] >= (char)('0' + SM_GPU_MAX))
		return -1;
	*gpu = (unsigned)(name[3] - '0');

	/* The rest names the field. */
	return name_index(name + 4, suffixes, count);
}

/* Takes the next name=value of a line (the value may be quoted); NULL at the end. */
static char *
field_next(
	char **cursor,
	char **value)
{
	char *name;
	char *end;

	/* The spaces before it. */
	name = *cursor;
	while (*name == ' ' || *name == '\t')
		name++;
	if (*name == '\0')
		return NULL;

	/* The name ends at the equals sign. */
	end = strchr(name, '=');
	if (end == NULL)
		return NULL;
	*end = '\0';
	*value = end + 1;

	/* A quoted value ends at the next quote, a plain one at a space. */
	if (**value == '"') {
		(*value)++;
		end = strchr(*value, '"');
	} else {
		end = strpbrk(*value, " \t");
	}

	/* The rest of the line follows. */
	if (end == NULL) {
		*cursor = *value + strlen(*value);
	} else {
		*end = '\0';
		*cursor = end + 1;
	}

	/* Succeeded: the name. */
	return name;
}

/* A field's whole number (decimal). */
static uint64_t
field_u64(
	const char *value)
{
	/* An unsigned long long is at least 64 bits. */
	return (uint64_t)strtoull(value, NULL, 10);
}
