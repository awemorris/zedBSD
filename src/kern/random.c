/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel random number generator (plan/ws034/phase055/design.md).
 *
 * Inputs are hashed into a BLAKE2s pool: the platform's entropy source when
 * it has one (hal_entropy_fill), the RTC, and the high-frequency counter read
 * at every timer tick on every CPU, whose low bits jitter against the tick.
 * A reseed replaces the ChaCha20 key with BLAKE2s(key || pool digest).  The
 * generator counts as seeded once the platform gave a full key or enough
 * ticks were sampled; until then a waiting reader sleeps.
 *
 * Output is ChaCha20 with fast key erasure: every request makes its blocks,
 * takes the first 32 bytes as the next key and hands out the rest, so a
 * later compromise of the state reveals nothing already handed out.
 */

#include "kern/random.h"
#include "kern/random-crypto.h"

#include <kern/kcrt.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/clock.h>
#include <kern/signal.h>
#include <kern/thread.h>
#include <kern/waitq.h>

#include <uapi/errno.h>

/* Output made under one hold of the lock; longer requests loop. */
#define RANDOM_CHUNK		256U

/* Tick samples that count as a seed without a platform source. */
#define RANDOM_SEED_SAMPLES	256U

/* How often the key is replaced from the pool once seeded. */
#define RANDOM_RESEED_MS	(5U * 60U * 1000U)

struct random_sample {
	uint64_t counter;
	uint32_t cpu;
	uint32_t tick;
};

static struct spinlock random_lock;
static struct wait_queue random_waitq;
static struct blake2s_state random_pool;
static uint8_t random_key[CHACHA20_KEY_BYTES];
static unsigned random_samples;
static uint64_t random_reseed_tick;
static volatile unsigned random_ready;
static volatile unsigned random_is_seeded;

static void reseed_locked(void);
static void generate_locked(uint8_t *out, size_t length);

/*
 * Seeds the generator from what the platform offers at boot.
 */
void
kern_random_init(
	void)
{
	uint8_t entropy[CHACHA20_KEY_BYTES];
	uint64_t counter;
	uint64_t frequency;
	uint64_t seconds;
	unsigned long irq;
	int platform;

	spin_init(&random_lock, LOCK_RANK_RANDOM, "random");
	waitq_init(&random_waitq, "random seed");
	blake2s_init(&random_pool);

	/* The platform source, the wall clock and the counter. */
	platform = hal_entropy_fill(entropy, sizeof(entropy));
	if (platform)
		blake2s_update(&random_pool, entropy, sizeof(entropy));
	if (hal_rtc_read_epoch_time(&seconds))
		blake2s_update(&random_pool, &seconds, sizeof(seconds));
	if (hal_rtc_read_counter(&counter, &frequency))
		blake2s_update(&random_pool, &counter, sizeof(counter));
	kern_memset_explicit(entropy, 0, sizeof(entropy));

	irq = spin_lock_irqsave(&random_lock);
	reseed_locked();
	if (platform)
		random_is_seeded = 1;
	atomic_raw_store_release(&random_ready, 1U);
	spin_unlock_irqrestore(&random_lock, irq);
}

/*
 * Mixes the counter's timing of one timer tick into the pool.
 */
void
kern_random_tick(
	hal_cpu_id_t cpu)
{
	struct random_sample sample;
	uint64_t frequency;
	unsigned long irq;

	/* A tick before initialization has nowhere to go. */
	if (atomic_raw_load_acquire(&random_ready) == 0)
		return;
	if (!hal_rtc_read_counter(&sample.counter, &frequency))
		return;
	sample.cpu = (uint32_t)cpu;
	sample.tick = (uint32_t)sched_ticks();

	irq = spin_lock_irqsave(&random_lock);
	blake2s_update(&random_pool, &sample, sizeof(sample));
	if (random_samples < RANDOM_SEED_SAMPLES)
		random_samples++;

	/* Enough jitter makes the first seed; readers waiting for it wake. */
	if (!random_is_seeded && random_samples >= RANDOM_SEED_SAMPLES) {
		reseed_locked();
		random_is_seeded = 1;
		waitq_wake_all(&random_waitq);
	}
	spin_unlock_irqrestore(&random_lock, irq);
}

/*
 * Mixes caller-supplied bytes into the pool without crediting them.
 */
void
kern_random_add(
	const void *data,
	size_t length)
{
	unsigned long irq;

	if (atomic_raw_load_acquire(&random_ready) == 0 || length == 0)
		return;
	irq = spin_lock_irqsave(&random_lock);
	blake2s_update(&random_pool, data, length);
	spin_unlock_irqrestore(&random_lock, irq);
}

/*
 * Reports whether the generator has been seeded.
 */
int
kern_random_seeded(
	void)
{
	/* Succeeded: the seeded state. */
	return atomic_raw_load_acquire(&random_is_seeded) != 0;
}

/*
 * Fills a kernel buffer with random bytes.
 */
int
kern_random_read(
	void *buffer,
	size_t length,
	unsigned flags)
{
	uint8_t *out;
	uint64_t observed;
	unsigned long irq;
	size_t chunk;
	int error;

	if (atomic_raw_load_acquire(&random_ready) == 0)
		return EAGAIN;
	out = buffer;
	irq = spin_lock_irqsave(&random_lock);

	/* Waits for the first seed when the caller asked to. */
	while ((flags & KERN_RANDOM_WAIT) != 0 && !random_is_seeded) {
		observed = waitq_sequence(&random_waitq);
		error = waitq_sleep(&random_waitq, &random_lock, observed, 0,
		    WAITQ_INTERRUPTIBLE);
		if (error == EAGAIN)
			continue;
		if (error != 0) {
			spin_unlock_irqrestore(&random_lock, irq);

			/* Failed: interrupted before the seed. */
			return error;
		}
	}

	/* Replaces the key from the pool now and then. */
	if (random_is_seeded &&
	    sched_ticks() - random_reseed_tick >= KERN_MS_TO_TICKS(RANDOM_RESEED_MS))
		reseed_locked();

	/* Makes the output a chunk at a time so other callers can interleave. */
	while (length != 0) {
		chunk = length < RANDOM_CHUNK ? length : RANDOM_CHUNK;
		generate_locked(out, chunk);
		out += chunk;
		length -= chunk;
		if (length != 0) {
			spin_unlock_irqrestore(&random_lock, irq);
			irq = spin_lock_irqsave(&random_lock);
		}
	}
	spin_unlock_irqrestore(&random_lock, irq);

	/* Succeeded. */
	return 0;
}

/* Replaces the key with BLAKE2s(key || pool digest); the pool restarts. */
static void
reseed_locked(
	void)
{
	struct blake2s_state hash;
	uint8_t digest[BLAKE2S_DIGEST_BYTES];

	blake2s_final(&random_pool, digest);
	blake2s_init(&random_pool);
	blake2s_update(&random_pool, digest, sizeof(digest));

	blake2s_init(&hash);
	blake2s_update(&hash, random_key, sizeof(random_key));
	blake2s_update(&hash, digest, sizeof(digest));
	blake2s_final(&hash, random_key);
	kern_memset_explicit(digest, 0, sizeof(digest));
	kern_memset_explicit(&hash, 0, sizeof(hash));
	random_reseed_tick = sched_ticks();
}

/* Makes length (at most RANDOM_CHUNK) bytes and replaces the key. */
static void
generate_locked(
	uint8_t *out,
	size_t length)
{
	static const uint8_t nonce[12];
	uint8_t blocks[CHACHA20_KEY_BYTES + RANDOM_CHUNK + CHACHA20_BLOCK_BYTES];
	uint32_t counter;
	size_t made;

	/* Enough blocks for the next key and the output. */
	made = 0;
	for (counter = 0; made < CHACHA20_KEY_BYTES + length; counter++) {
		chacha20_block(random_key, counter, nonce, blocks + made);
		made += CHACHA20_BLOCK_BYTES;
	}
	kern_memcpy(random_key, blocks, CHACHA20_KEY_BYTES);
	kern_memcpy(out, blocks + CHACHA20_KEY_BYTES, length);
	kern_memset_explicit(blocks, 0, sizeof(blocks));
}
