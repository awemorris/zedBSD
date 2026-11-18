/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The System Monitor (plan/ws134/design.md): the data the screen shows,
 * where it comes from (the source: a simulation, a recorded replay, later
 * the system through libkeiland), the history the graphs draw, the rules
 * that grade the system's state, and the formatting of values.
 *
 * Nothing here touches Wayland or Vulkan, so that the host tests build
 * these parts alone (plan/ws134/tests/host/).
 */

#ifndef MONITOR_H
#define MONITOR_H

#include <stddef.h>
#include <stdint.h>

/* The most CPUs, GPUs, network links and disks one frame describes. */
#define SM_CPU_MAX		64U
#define SM_GPU_MAX		4U
#define SM_LINK_MAX		16U
#define SM_DISK_MAX		8U

/* The longest name of a machine, a device or a link, with its NUL. */
#define SM_NAME_MAX		48U

/* The samples one history series keeps: an hour of one-second samples. */
#define SM_HISTORY_MAX		3600U

/*
 * The fields of a frame, as bits: which ones the source gave (valid) and
 * which of those are the simulation's stand-ins (simulated).  A simulated
 * field is drawn like any other but never grades the system's state or
 * makes an event (design.md section 1.5).
 */
#define SM_HAVE_CPU		0x0001U
#define SM_HAVE_MEMORY		0x0002U
#define SM_HAVE_SWAP		0x0004U
#define SM_HAVE_NETWORK		0x0008U
#define SM_HAVE_DISK		0x0010U
#define SM_HAVE_DISK_LATENCY	0x0020U
#define SM_HAVE_GPU_BUSY	0x0040U
#define SM_HAVE_GPU_MEMORY	0x0080U
#define SM_HAVE_GPU_TEMPERATURE	0x0100U
#define SM_HAVE_GPU_POWER	0x0200U
#define SM_HAVE_CPU_FREQUENCY	0x0400U

/* The system's state, from calm to critical (design.md section 3.7). */
enum sm_level {
	SM_LEVEL_NORMAL,
	SM_LEVEL_ELEVATED,
	SM_LEVEL_WARNING,
	SM_LEVEL_CRITICAL
};

/* The sources a frame comes from. */
enum sm_source_kind {
	SM_SOURCE_SIM,
	SM_SOURCE_REPLAY,
	SM_SOURCE_SYSTEM
};

/* The series the history keeps, one per summary plate's graph. */
enum sm_series {
	SM_SERIES_CPU,
	SM_SERIES_GPU,
	SM_SERIES_MEMORY,
	SM_SERIES_RX,
	SM_SERIES_TX,
	SM_SERIES_READ,
	SM_SERIES_WRITE,
	SM_SERIES_COUNT
};

/*
 * What does not change from frame to frame: the machine's name, how many
 * CPUs it has, and its GPUs.
 */
struct sm_info {
	char host[SM_NAME_MAX];
	unsigned cpu_count;
	unsigned gpu_count;
	char gpu_name[SM_GPU_MAX][SM_NAME_MAX];
	uint64_t memory_total;
	uint64_t swap_total;
	uint64_t gpu_memory_total[SM_GPU_MAX];
};

/*
 * One frame of values, already made into rates and shares: the CPU's use
 * (0 to 1, the whole and each CPU), the memory in bytes, the network's and
 * the disks' rates in bytes a second (the sums over the links and the
 * disks), the disks' mean latency in milliseconds, and each GPU's use,
 * memory, temperature and power.  time_ms is the source's clock (the
 * monitor's time since it started, or the recording's).
 */
struct sm_frame {
	uint64_t time_ms;
	unsigned valid;
	unsigned simulated;
	double cpu;
	double cpu_core[SM_CPU_MAX];
	double cpu_mhz;
	uint64_t memory_used;
	uint64_t memory_cache;
	uint64_t memory_available;
	uint64_t swap_used;
	double rx_rate;
	double tx_rate;
	double read_rate;
	double write_rate;
	double disk_latency_ms;
	double gpu_busy[SM_GPU_MAX];
	uint64_t gpu_memory_used[SM_GPU_MAX];
	double gpu_celsius[SM_GPU_MAX];
	double gpu_watts[SM_GPU_MAX];
};

/*
 * The simulation's state: the random generator, the slow waves' phases,
 * the spike in progress, and the injected excursion over a threshold
 * (design.md section 1.6).
 */
struct sm_sim {
	uint64_t seed;
	uint64_t random;
	double phase[8];
	double noise[8];
	uint64_t spike_start_ms;
	uint64_t spike_length_ms;
	double spike_height;
	unsigned spike_target;
	uint64_t next_spike_ms;
	uint64_t excursion_start_ms;
	uint64_t excursion_length_ms;
	unsigned excursion_target;
	uint64_t next_excursion_ms;
	unsigned calm;
	unsigned hot_core;
	uint64_t next_hot_core_ms;
	double gpu_celsius;
};

/*
 * A recording being played: the file's lines, read one frame at a time
 * (plan/ws134/tests/replay/), and whether it ran out.
 */
struct sm_replay {
	char *text;
	size_t length;
	size_t offset;
	int ended;
	struct sm_frame pending;
	int have_pending;
};

/* A source of frames: the simulation or a recording. */
struct sm_source {
	enum sm_source_kind kind;
	unsigned period_ms;
	uint64_t next_ms;
	struct sm_info info;
	struct sm_sim sim;
	struct sm_replay replay;
};

/* One history series: a ring of samples, the newest at head - 1. */
struct sm_ring {
	float values[SM_HISTORY_MAX];
	unsigned head;
	unsigned count;
};

/* The history of every series. */
struct sm_history {
	struct sm_ring series[SM_SERIES_COUNT];
};

/*
 * The rules' state: how long each condition has held, and the level each
 * reached, so that a level rises after its condition held and falls one
 * step at a time (design.md section 3.7).
 */
/* The rules (rules.c), in the order of their levels in struct sm_rules. */
enum sm_rule {
	SM_RULE_CPU,
	SM_RULE_MEMORY,
	SM_RULE_SWAP,
	SM_RULE_LATENCY,
	SM_RULE_GPU,
	SM_RULE_TEMPERATURE
};
#define SM_RULE_COUNT		6U
struct sm_rules {
	enum sm_level level;
	enum sm_level rule_levels[SM_RULE_COUNT];
	uint64_t since_ms[SM_RULE_COUNT][2];
	uint64_t changed_ms;
	int cause;
	char summary[96];
};

/* source.c */
int sm_source_open_sim(struct sm_source *source, uint64_t seed, unsigned period_ms, unsigned cpus, unsigned gpus, int calm);
int sm_source_open_replay(struct sm_source *source, const char *path, unsigned period_ms);
void sm_source_close(struct sm_source *source);
int sm_source_take(struct sm_source *source, uint64_t now_ms, struct sm_frame *frame);
int sm_replay_parse(struct sm_replay *replay, struct sm_info *info, struct sm_frame *frame);
void sm_sim_frame(struct sm_sim *sim, const struct sm_info *info, uint64_t time_ms, struct sm_frame *frame);

/* history.c */
void sm_history_init(struct sm_history *history);
void sm_history_add(struct sm_history *history, const struct sm_info *info, const struct sm_frame *frame);
unsigned sm_history_read(const struct sm_history *history, enum sm_series series, unsigned count, float *values);
float sm_history_peak(const struct sm_history *history, enum sm_series series, unsigned count);

/* rules.c */
void sm_rules_init(struct sm_rules *rules);
enum sm_level sm_rules_update(struct sm_rules *rules, const struct sm_info *info, const struct sm_frame *frame, int count_simulated);
const char *sm_level_name(enum sm_level level);

/* format.c */
size_t sm_format_percent(char *out, size_t size, double share);
size_t sm_format_bytes(char *out, size_t size, uint64_t bytes);
size_t sm_format_rate(char *out, size_t size, double bytes_per_second, int bits);
size_t sm_format_pair(char *out, size_t size, uint64_t used, uint64_t total);
size_t sm_format_uptime(char *out, size_t size, uint64_t seconds);

#endif
