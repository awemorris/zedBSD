/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CPU performance framework: the table of the CPUs' topology and core
 * kinds, the frequency domains, the latest intent for each CPU and the
 * notification path that applies it on that CPU, the relative performance
 * of the CPUs, the idle residency counters, and the one interface a
 * processor driver registers to do the hardware's part.
 *
 * The framework owns the policy: it turns the thermal strategy, a root's
 * direct setting and the thermal ceiling into one request per CPU, on
 * that CPU, against the capabilities the CPU reports at that moment.  The
 * driver owns the hardware: it reads the CPU's identity and capabilities
 * and writes the request where the processor reads it.  The driver never
 * sees the machine layer: the operations that touch a CPU's registers are
 * called on that CPU, and the others read what those operations kept.
 */

#ifndef KERN_KERN_CPUPERF_H
#define KERN_KERN_CPUPERF_H

#include <kern/types.h>
#include <stdbool.h>
#include <stdint.h>

/* The value of a field nobody knows, where a field says so. */
#define KERN_CPUPERF_UNKNOWN		0xffffffffU

/* The domain argument that stands for every domain. */
#define KERN_CPUPERF_ALL		0xffffffffU

/* The most steps a STEPS CPU offers; a provider with more thins them, keeping both ends. */
#define KERN_CPUPERF_STEP_MAX		16U

/* The most idle states a residency report carries. */
#define KERN_CPUPERF_IDLE_STATE_MAX	8U

/*
 * The kinds of core, as the framework tells them apart.  A processor of
 * one kind reports PERFORMANCE for every core; UNKNOWN (0) means nobody
 * knows, as on a machine without a provider.
 */
#define KERN_CPUPERF_CORE_UNKNOWN	0U
#define KERN_CPUPERF_CORE_PERFORMANCE	1U
#define KERN_CPUPERF_CORE_EFFICIENCY	2U

/*
 * How far a CPU shares one frequency: alone, with the other threads of
 * its core, with the other cores of its module, or with the whole
 * package.  A firmware grouping that names other sets is reported as
 * PACKAGE, the safe side.  CPUs that report different sharing are never
 * in one domain.
 */
#define KERN_CPUPERF_SHARE_THREAD	1U
#define KERN_CPUPERF_SHARE_CORE		2U
#define KERN_CPUPERF_SHARE_MODULE	3U
#define KERN_CPUPERF_SHARE_PACKAGE	4U

/*
 * The shape of a CPU's performance levels.  RANGE: every whole unit from
 * lowest to highest can be asked for and the processor picks within the
 * bounds it is given; the step list is unused.  STEPS: only the listed
 * steps exist and the operating system picks one; the four anchors are
 * step values and every bound the framework writes is a step value.
 */
#define KERN_CPUPERF_LEVELS_RANGE	1U
#define KERN_CPUPERF_LEVELS_STEPS	2U

/* The ways of measuring relative performance, as bits. */
#define KERN_CPUPERF_RELATIVE_FREQUENCY	0x1U	/* the highest rate the framework measured */
#define KERN_CPUPERF_RELATIVE_FEEDBACK	0x2U	/* the hardware feedback's performance */
#define KERN_CPUPERF_RELATIVE_PROVIDER	0x4U	/* the provider's own table */

/* Which fields of a direct setting are to change, as bits. */
#define KERN_CPUPERF_SET_MINIMUM	0x1U
#define KERN_CPUPERF_SET_MAXIMUM	0x2U
#define KERN_CPUPERF_SET_DESIRED	0x4U
#define KERN_CPUPERF_SET_PREFERENCE	0x8U

/* What a CPU's report says beyond its numbers, as bits. */
#define KERN_CPUPERF_CPU_HAS_CONTROL	0x1U	/* a request can be written */
#define KERN_CPUPERF_CPU_ESTIMATED_MHZ	0x2U	/* the MHz fields are estimates */
#define KERN_CPUPERF_CPU_LIMITED	0x4U	/* the thermal ceiling cuts the maximum */
#define KERN_CPUPERF_CPU_PLATFORM_LIMIT	0x8U	/* the platform holds the CPU below the request */
#define KERN_CPUPERF_CPU_CUSTOM		0x10U	/* a root's direct setting is in force */

/* What a domain's report says beyond its numbers, as bits. */
#define KERN_CPUPERF_DOMAIN_CUSTOM	0x1U
#define KERN_CPUPERF_DOMAIN_LIMITED	0x2U
#define KERN_CPUPERF_DOMAIN_PENDING	0x4U	/* a member has not confirmed the request */

/* What a sample says beyond its numbers, as bits. */
#define KERN_CPUPERF_SAMPLE_PLATFORM_LIMIT	0x1U
#define KERN_CPUPERF_SAMPLE_CAPABILITIES	0x2U	/* the four anchors are fresh */

/* What a reader of a CPU asks for, as bits. */
#define KERN_CPUPERF_READ_SAMPLE	0x1U	/* wake the CPU to measure its delivered rate */

/* What changed, as a provider reports it. */
#define KERN_CPUPERF_CHANGED_CAPABILITIES	0x1U
#define KERN_CPUPERF_CHANGED_RELATIVE		0x2U

/*
 * Where one CPU sits in the machine.
 *
 * The numbers are the processor's own, unique within the enclosing level:
 * two CPUs with the same package, die and core numbers are threads of one
 * core.  A machine without a provider reports CPU n as package 0, die 0,
 * module n, core n, thread 0, of an unknown kind.
 */
struct kern_cpuperf_topology {
	uint32_t package;
	uint32_t die;
	uint32_t module;		/* cores that share a cache or a clock */
	uint32_t core;
	uint32_t thread;
	uint32_t core_class;		/* KERN_CPUPERF_CORE_* (0 unknown) */
	uint32_t core_type;		/* the processor's own code for the kind, 0 unknown */
	uint32_t firmware_id;		/* the firmware's processor number, KERN_CPUPERF_UNKNOWN */
};

/*
 * The performance levels one CPU offers and how far it shares them.
 *
 * Units are the provider's abstract performance units, not MHz.  The four
 * anchors of a RANGE CPU are KERN_CPUPERF_UNKNOWN until the provider can
 * read them (a processor may hide them until its control is started);
 * the framework then learns them from the CPU's samples.
 */
struct kern_cpuperf_levels {
	uint32_t kind;			/* KERN_CPUPERF_LEVELS_*, 0 for a CPU that cannot be controlled */
	uint32_t share;			/* KERN_CPUPERF_SHARE_* */
	uint32_t lowest;
	uint32_t efficient;		/* below it a request may not be honoured */
	uint32_t guaranteed;		/* the most the CPU sustains; above it are turbo levels */
	uint32_t highest;
	uint32_t step_count;		/* STEPS: 1..KERN_CPUPERF_STEP_MAX */
	uint32_t steps[KERN_CPUPERF_STEP_MAX];		/* STEPS: ascending */
	uint32_t step_mhz[KERN_CPUPERF_STEP_MAX];	/* estimate, 0 unknown */
	uint32_t highest_mhz;		/* estimate, 0 unknown */
	uint32_t voltage_group;		/* CPUs that share a voltage, KERN_CPUPERF_UNKNOWN */
	uint32_t firmware_id;		/* the firmware's processor number, KERN_CPUPERF_UNKNOWN */
};

/*
 * One request the framework hands the provider to write on one CPU.
 *
 * The bounds are in units and inside the anchors the CPU reported just
 * before, so lowest <= minimum <= maximum <= highest.  desired is 0 to let
 * a RANGE processor pick, otherwise a value in the bounds; for a STEPS CPU
 * every field but preference is a step value and desired is the step to
 * run at.  preference is the energy preference 0 (performance) .. 255
 * (energy), or KERN_CPUPERF_UNKNOWN to leave it.
 */
struct kern_cpuperf_request {
	uint32_t minimum;
	uint32_t maximum;
	uint32_t desired;
	uint32_t preference;
};

/*
 * What one CPU reports of itself when the framework samples it on that
 * CPU: its capabilities now and the rate it delivered since its previous
 * sample.
 */
struct kern_cpuperf_sample {
	uint32_t flags;			/* KERN_CPUPERF_SAMPLE_* */
	uint32_t lowest;
	uint32_t efficient;
	uint32_t guaranteed;
	uint32_t highest;
	uint32_t delivered_mhz;		/* estimate, 0 unknown */
};

/*
 * The idle residency of one CPU or one package.
 *
 * Each entry names the state as the processor numbers it (2 for C2 or
 * PC2) and the time spent in it since an unknown origin; a reader takes
 * two reports and subtracts.  time_ns is the kernel's tick time of the
 * read, to the tick.
 */
struct kern_cpuperf_idle {
	uint32_t count;
	uint32_t available;		/* the states the processor offers, bit n for state n */
	uint32_t state[KERN_CPUPERF_IDLE_STATE_MAX];
	uint64_t residency_ns[KERN_CPUPERF_IDLE_STATE_MAX];
	uint64_t time_ns;
};

/*
 * What the framework reports of one CPU: its place, its domain, its
 * relative performance and its levels.
 */
struct kern_cpuperf_cpu {
	struct kern_cpuperf_topology topology;
	uint32_t domain;
	uint32_t voltage_group;		/* KERN_CPUPERF_UNKNOWN when not known */
	uint32_t relative;		/* 0..100, the fastest is 100; KERN_CPUPERF_UNKNOWN */
	uint32_t available;		/* 0..255 from the hardware feedback; KERN_CPUPERF_UNKNOWN */
	uint32_t efficiency;		/* 0..255 from the hardware feedback; KERN_CPUPERF_UNKNOWN */
	uint32_t lowest;		/* the anchors, KERN_CPUPERF_UNKNOWN until known */
	uint32_t efficient;
	uint32_t guaranteed;
	uint32_t highest;
	uint32_t highest_mhz;		/* estimate, 0 unknown */
	uint32_t delivered_mhz;		/* the last sample's estimate, 0 unknown */
	uint32_t flags;			/* KERN_CPUPERF_CPU_* */
};

/*
 * What the framework reports of one domain: its members, its levels, the
 * request its intent asks for before the thermal ceiling, and the ceiling.
 */
struct kern_cpuperf_domain {
	uint32_t count;			/* the number of domains */
	uint32_t cpu_count;		/* the number of members */
	uint64_t cpus[KERN_CPU_MASK_WORDS];	/* CPU n is bit n % 64 of word n / 64 */
	struct kern_cpuperf_levels levels;
	struct kern_cpuperf_request request;
	uint32_t limit;			/* the ceiling in units, KERN_CPUPERF_UNKNOWN for none */
	uint32_t flags;			/* KERN_CPUPERF_DOMAIN_* */
};

/*
 * A root's direct setting of one domain or of every domain.  For one
 * domain the bounds and desired are units of that domain; for every
 * domain they are permille of each domain's range.  The preference is
 * 0..255 either way.
 */
struct kern_cpuperf_setting {
	uint32_t fields;		/* KERN_CPUPERF_SET_* */
	uint32_t minimum;
	uint32_t maximum;
	uint32_t desired;		/* 0 hands the choice back to the processor */
	uint32_t preference;
};

/*
 * The hardware's part, which one processor driver registers.
 *
 * On the target CPU, in the cross-CPU notification with interrupts
 * masked, called with that CPU's number: topology, start, apply, sample,
 * measure, feedback, idle_package and idle_core.  They only read and
 * write the processor's registers and the driver's own per-CPU copies;
 * they do not sleep, take locks or spin for long.  In a thread, with the
 * framework's mutex held, so never two at once: describe, relative and
 * feedback_read; they read what the notification operations kept and may
 * take the driver's own locks.  Any member but topology and describe may
 * be NULL; a provider without apply describes the machine and controls
 * nothing.
 */
struct kern_cpuperf_provider_ops {
	/* Reads the CPU's place in the machine. */
	int (*topology)(kern_cpu_id_t cpu, struct kern_cpuperf_topology *topology);

	/* Describes a CPU whose topology has been read, from what the driver kept. */
	int (*describe)(kern_cpu_id_t cpu, struct kern_cpuperf_levels *levels);

	/*
	 * Starts the hardware control of the CPU before its first request
	 * and reports its capabilities in a sample.  Called once per CPU; a
	 * failure leaves the CPU uncontrolled until the next request.
	 */
	int (*start)(kern_cpu_id_t cpu, struct kern_cpuperf_sample *sample);

	/* Writes a request; 0 or an errno, kept for whoever waits on the CPU. */
	int (*apply)(kern_cpu_id_t cpu, const struct kern_cpuperf_request *request);

	/* Samples the CPU: the anchors now and the rate since its previous sample. */
	void (*sample)(kern_cpu_id_t cpu, struct kern_cpuperf_sample *sample);

	/*
	 * Measures the delivered rate: begin true marks the start, begin
	 * false reports the MHz since the mark.  The framework runs the CPU
	 * busy at its highest level between the two calls.
	 */
	int (*measure)(kern_cpu_id_t cpu, bool begin, uint32_t *mhz);

	/* The KERN_CPUPERF_RELATIVE_FEEDBACK and _PROVIDER ways relative() can measure. */
	uint32_t relative_sources;

	/* Reports a CPU's raw performance by one way, before the scaling to 100. */
	int (*relative)(kern_cpu_id_t cpu, uint32_t source, uint32_t *value);

	/* Copies the hardware feedback of the CPU's package; *changed says whether it moved. */
	int (*feedback)(kern_cpu_id_t cpu, bool *changed);

	/* Reads a CPU's values from the last feedback copy. */
	int (*feedback_read)(kern_cpu_id_t cpu, uint32_t *available, uint32_t *efficiency);

	/* Reads the residency of the CPU's package. */
	int (*idle_package)(kern_cpu_id_t cpu, struct kern_cpuperf_idle *idle);

	/* Reads the residency of the CPU's core. */
	int (*idle_core)(kern_cpu_id_t cpu, struct kern_cpuperf_idle *idle);
};

/*
 * Brings the framework up once every CPU takes notifications.
 *
 * Thread context, once, before any driver registers a provider.
 */
void
kern_cpuperf_init(void);

/*
 * Registers the one provider.
 *
 * Thread context; sleeps while every CPU reads its topology (each bounded
 * by a second).  Returns 0, EINVAL for missing operations, ENODEV before
 * kern_cpuperf_init(), or EBUSY for a second provider.
 */
int
kern_cpuperf_provider_register(
	const struct kern_cpuperf_provider_ops *ops);

/*
 * Tells the framework that a provider's answers changed.
 *
 * Thread context; sleeps on the framework's mutex.
 */
void
kern_cpuperf_provider_changed(
	uint32_t what);

/*
 * Runs the framework's part of a cross-CPU notification on this CPU.
 *
 * The notification handler calls it with interrupts masked, for the CPU it
 * runs on.  With nothing posted for the CPU it returns without a lock.
 */
void
kern_cpuperf_notify(
	kern_cpu_id_t cpu);

/*
 * Reports one CPU.
 *
 * Without KERN_CPUPERF_READ_SAMPLE: any context where a spinlock may be
 * taken with interrupts masked; no CPU is woken.  With it: thread context
 * only, sleeps about 100 ms between two samples of the CPU and at most a
 * second for each.  Returns 0, EINVAL for a CPU that does not exist, or
 * ETIMEDOUT when the CPU did not answer (the rest of the report is still
 * filled).
 */
int
kern_cpuperf_cpu(
	kern_cpu_id_t cpu,
	uint32_t flags,
	struct kern_cpuperf_cpu *info);

/*
 * Reports one domain.
 *
 * Thread context; sleeps on the framework's mutex.  Returns 0 or EINVAL.
 */
int
kern_cpuperf_domain(
	uint32_t domain,
	struct kern_cpuperf_domain *info);

/*
 * Reports the number of domains.  Thread context.  Domains are grouped
 * once when the provider registers and keep their numbers after that.
 */
uint32_t
kern_cpuperf_domain_count(void);

/*
 * Reports the generation of the CPUs' answers.
 *
 * Any context, no lock.  It changes whenever the topology, the domains,
 * the core kinds, the capabilities, the relative performance, its way of
 * measuring or the feedback change; a reader compares it with the value
 * it remembers.
 */
uint32_t
kern_cpuperf_generation(void);

/*
 * Applies a root's direct setting to one domain or to every domain.
 *
 * Thread context; the caller checks the privilege.  Sleeps at most a
 * second for the CPUs, without the mutex.  Returns 0, EINVAL for a bad
 * domain or value, EOPNOTSUPP where nothing can be controlled, ETIMEDOUT
 * when a CPU did not confirm (the setting stays and takes effect at its
 * next notification), or the provider's refusal.
 */
int
kern_cpuperf_domain_set(
	uint32_t domain,
	const struct kern_cpuperf_setting *setting);

/*
 * Reports the relative ways that can be chosen and the one in force.
 * Any context.
 */
void
kern_cpuperf_relative_get(
	uint32_t *available,
	uint32_t *source);

/*
 * Chooses the way relative performance is measured, until the next boot.
 *
 * Thread context; the caller checks the privilege.  Returns 0, EINVAL for
 * an unknown way, or EOPNOTSUPP for a way that cannot be measured here.
 */
int
kern_cpuperf_relative_select(
	uint32_t source);

/*
 * Reads the idle residency of a CPU's core, on that CPU.
 *
 * Thread context; sleeps at most a second.  Returns 0, EINVAL,
 * EOPNOTSUPP, ETIMEDOUT or the provider's error.
 */
int
kern_cpuperf_idle_cpu(
	kern_cpu_id_t cpu,
	struct kern_cpuperf_idle *idle);

/*
 * Reads the idle residency of a package, on the calling CPU when it
 * belongs to the package and otherwise on the package's first CPU.
 *
 * Thread context; sleeps at most a second.  Returns as kern_cpuperf_idle_cpu().
 */
int
kern_cpuperf_idle_package(
	uint32_t package,
	struct kern_cpuperf_idle *idle);

#endif
