/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The register model of the xd host tests (see xd-fake-regs.h).
 *
 * The registers are kept in a small table searched in order; a register
 * nobody wrote reads 0.  Time only passes when the code under test waits
 * through the window's wait hook, so a test fully decides when a state
 * bit appears.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../intel/display-regs-xd.h"
#include "../../intel/display-regs-xd-south.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * The control word's bits an AUX transaction owns: busy, done, both
 * errors and the reply size.  The rest is what the driver wrote.
 */
#define I915_XD_FAKE_AUX_STATUS		(I915_XD_AUX_CTL_BUSY | \
					 I915_XD_AUX_CTL_DONE | \
					 I915_XD_AUX_CTL_TIMEOUT_ERROR | \
					 I915_XD_AUX_CTL_RECEIVE_ERROR | \
					 I915_XD_AUX_CTL_SIZE)

/*
 * The AUX request codes the sink tells apart (the top four bits of the
 * first byte): a native command has bit 3 set, an I2C one does not; a
 * native read is 9, a native write 8; an I2C command's low two bits say
 * write (0), read (1) or write status update (2).  The reply codes of a
 * refusal and of a request to wait, native and I2C, are in the top
 * nibble of the reply's first byte.
 *
 * Values referenced: VESA DisplayPort Standard (the AUX request and
 * reply command codes).
 */
#define I915_XD_FAKE_AUX_NATIVE		0x8U
#define I915_XD_FAKE_AUX_NATIVE_READ	0x9U
#define I915_XD_FAKE_AUX_I2C_KIND	0x3U
#define I915_XD_FAKE_AUX_I2C_READ	0x1U
#define I915_XD_FAKE_AUX_NATIVE_NACK	0x10U
#define I915_XD_FAKE_AUX_NATIVE_DEFER	0x20U
#define I915_XD_FAKE_AUX_I2C_NACK	0x40U
#define I915_XD_FAKE_AUX_I2C_DEFER	0x80U

/* How many registers, rules, later changes and held locks the model keeps. */
#define I915_XD_FAKE_REGISTERS		2048U
#define I915_XD_FAKE_RULES		128U
#define I915_XD_FAKE_EVENTS		128U
#define I915_XD_FAKE_LOCKS		16U

/* One register of the model and its value. */
struct i915_xd_fake_register {
	uint32_t offset;
	uint32_t value;
};

/*
 * How the model answers a write of one register: by mirroring bits into
 * a target register after a delay, or by calling the test's function;
 * or how it answers a read of one register: by the test's read function.
 */
struct i915_xd_fake_rule {
	/* The register whose write triggers the rule. */
	uint32_t offset;

	/* The mirror: the bits taken from the written value, where they go, how far they move, and when. */
	uint32_t source_mask;
	uint32_t target;
	int shift;
	unsigned delay_us;

	/* The test's own answer to a write; NULL for a mirror and a read rule. */
	i915_xd_fake_write_fn function;

	/* The test's own answer to a read; NULL for a write rule. */
	i915_xd_fake_read_fn reader;
	void *context;
};

/* A change of a register the model makes when its time comes. */
struct i915_xd_fake_event {
	/* The time (model microseconds) the change happens. */
	uint64_t due;

	/* The register, and the bits cleared and set in it. */
	uint32_t offset;
	uint32_t clear;
	uint32_t set;
};

/*
 * The model's state, one per test process, emptied by i915_xd_fake_reset().
 *
 * The registers, the rules and the pending changes are each a used
 * prefix of their table; the clock only moves forward.
 */
static struct {
	struct i915_xd_fake_register registers[I915_XD_FAKE_REGISTERS];
	unsigned register_count;
	struct i915_xd_fake_rule rules[I915_XD_FAKE_RULES];
	unsigned rule_count;
	struct i915_xd_fake_event events[I915_XD_FAKE_EVENTS];
	unsigned event_count;
	uint64_t now;
	unsigned reads;
	unsigned writes;
	uint32_t previous;
	unsigned held[I915_XD_FAKE_LOCKS];
	unsigned held_count;
	unsigned lock_violations;
} i915_xd_fake;

static uint32_t i915_xd_fake_read_hook(void *context, uint32_t offset);
static void i915_xd_fake_write_hook(void *context, uint32_t offset, uint32_t value);
static void i915_xd_fake_wait_hook(void *context, unsigned microseconds);
static void i915_xd_fake_lock_hook(void *context, unsigned lock, int taken);
static struct i915_xd_fake_register *i915_xd_fake_find(uint32_t offset, int create);
static void i915_xd_fake_run_events(void);
static void i915_xd_fake_apply_rules(uint32_t offset, uint32_t value);
static uint32_t i915_xd_fake_apply_readers(uint32_t offset, uint32_t value);
static uint32_t i915_xd_fake_move(uint32_t bits, int shift);
static void i915_xd_fake_aux_write(uint32_t offset, uint32_t value, void *context);
static void i915_xd_fake_aux_run(struct i915_xd_fake_aux *sink, uint32_t offset, uint32_t value, uint32_t word);
static unsigned i915_xd_fake_aux_serve(struct i915_xd_fake_aux *sink, const uint8_t *request, unsigned size, uint8_t *reply);
static void i915_xd_fake_gmbus_command(uint32_t offset, uint32_t value, void *context);
static void i915_xd_fake_gmbus_index(uint32_t offset, uint32_t value, void *context);
static void i915_xd_fake_gmbus_feed(uint32_t offset, uint32_t value, void *context);
static uint32_t i915_xd_fake_gmbus_status(uint32_t offset, uint32_t value, void *context);
static uint32_t i915_xd_fake_gmbus_data(uint32_t offset, uint32_t value, void *context);

/*
 * Empties the model: no registers, no rules, no pending changes, the
 * clock at 0 and no locks held.
 */
void
i915_xd_fake_reset(void)
{
	/* Forgets every register, rule and pending change. */
	i915_xd_fake.register_count = 0U;
	i915_xd_fake.rule_count = 0U;
	i915_xd_fake.event_count = 0U;

	/* Restarts the clock and the counters. */
	i915_xd_fake.now = 0U;
	i915_xd_fake.reads = 0U;
	i915_xd_fake.writes = 0U;

	/* Starts with no locks held and no broken order seen. */
	i915_xd_fake.held_count = 0U;
	i915_xd_fake.lock_violations = 0U;
}

/*
 * Fills the hooks a struct i915_xd_io takes so that it reaches the model.
 */
void
i915_xd_fake_hooks(
	struct i915_xd_io_hooks *hooks)
{
	/* Points every hook at the model. */
	hooks->read = i915_xd_fake_read_hook;
	hooks->write = i915_xd_fake_write_hook;
	hooks->wait = i915_xd_fake_wait_hook;
	hooks->lock = i915_xd_fake_lock_hook;
	hooks->context = NULL;
}

/*
 * Sets a register of the model directly, as the hardware or the
 * firmware would have left it; no rule runs.
 */
void
i915_xd_fake_set(
	uint32_t offset,
	uint32_t value)
{
	struct i915_xd_fake_register *entry;

	/* Finds or adds the register and stores the value. */
	entry = i915_xd_fake_find(offset, 1);
	entry->value = value;
}

/*
 * Reads a register of the model without counting the read.
 */
uint32_t
i915_xd_fake_get(
	uint32_t offset)
{
	struct i915_xd_fake_register *entry;

	/* A register nobody wrote reads 0. */
	entry = i915_xd_fake_find(offset, 0);
	if (entry == NULL)
		return 0U;

	/* Succeeded: reports the register's value. */
	return entry->value;
}

/*
 * Reports the model's clock in microseconds.
 */
uint64_t
i915_xd_fake_now(void)
{
	/* Reports the time that has passed through the wait hook. */
	return i915_xd_fake.now;
}

/*
 * Schedules a change of a register: after delay_us of model time the
 * bits clear are cleared and the bits set are set.
 */
void
i915_xd_fake_later(
	unsigned delay_us,
	uint32_t offset,
	uint32_t clear,
	uint32_t set)
{
	struct i915_xd_fake_event *event;

	/* A change that is due now happens at once. */
	if (delay_us == 0U) {
		i915_xd_fake_set(offset, (i915_xd_fake_get(offset) & ~clear) | set);
		return;
	}

	/* A full table is a mistake in the test. */
	if (i915_xd_fake.event_count == I915_XD_FAKE_EVENTS) {
		fprintf(stderr, "xd-fake-regs: too many pending changes\n");
		abort();
	}

	/* Adds the change to the pending ones. */
	event = &i915_xd_fake.events[i915_xd_fake.event_count];
	event->due = i915_xd_fake.now + delay_us;
	event->offset = offset;
	event->clear = clear;
	event->set = set;
	i915_xd_fake.event_count++;
}

/*
 * Adds a rule: when the register at offset is written, the written bits
 * under source_mask appear in the register target, moved by shift
 * (left when positive, right when negative), delay_us later.  The bits
 * the mask covers in the target are replaced, so a cleared request bit
 * clears the state bit.
 */
void
i915_xd_fake_rule_mirror(
	uint32_t offset,
	uint32_t source_mask,
	uint32_t target,
	int shift,
	unsigned delay_us)
{
	struct i915_xd_fake_rule *rule;

	/* A full table is a mistake in the test. */
	if (i915_xd_fake.rule_count == I915_XD_FAKE_RULES) {
		fprintf(stderr, "xd-fake-regs: too many rules\n");
		abort();
	}

	/* Adds the mirror. */
	rule = &i915_xd_fake.rules[i915_xd_fake.rule_count];
	rule->offset = offset;
	rule->source_mask = source_mask;
	rule->target = target;
	rule->shift = shift;
	rule->delay_us = delay_us;
	rule->function = NULL;
	rule->reader = NULL;
	rule->context = NULL;
	i915_xd_fake.rule_count++;
}

/*
 * Adds a rule that calls the test's function after every write of the
 * register at offset; the written value is already stored.
 */
void
i915_xd_fake_rule_call(
	uint32_t offset,
	i915_xd_fake_write_fn function,
	void *context)
{
	struct i915_xd_fake_rule *rule;

	/* A full table is a mistake in the test. */
	if (i915_xd_fake.rule_count == I915_XD_FAKE_RULES) {
		fprintf(stderr, "xd-fake-regs: too many rules\n");
		abort();
	}

	/* Adds the call. */
	rule = &i915_xd_fake.rules[i915_xd_fake.rule_count];
	rule->offset = offset;
	rule->source_mask = 0U;
	rule->target = 0U;
	rule->shift = 0;
	rule->delay_us = 0U;
	rule->function = function;
	rule->reader = NULL;
	rule->context = context;
	i915_xd_fake.rule_count++;
}

/*
 * Adds a rule that answers every read of the register at offset with
 * the test's function, which is given the value the register holds.
 */
void
i915_xd_fake_rule_read(
	uint32_t offset,
	i915_xd_fake_read_fn function,
	void *context)
{
	struct i915_xd_fake_rule *rule;

	/* A full table is a mistake in the test. */
	if (i915_xd_fake.rule_count == I915_XD_FAKE_RULES) {
		fprintf(stderr, "xd-fake-regs: too many rules\n");
		abort();
	}

	/* Adds the read answer. */
	rule = &i915_xd_fake.rules[i915_xd_fake.rule_count];
	rule->offset = offset;
	rule->source_mask = 0U;
	rule->target = 0U;
	rule->shift = 0;
	rule->delay_us = 0U;
	rule->function = NULL;
	rule->reader = function;
	rule->context = context;
	i915_xd_fake.rule_count++;
}

/*
 * Reports, to a rule that runs for a write, the value the register held
 * before that write.
 */
uint32_t
i915_xd_fake_previous(void)
{
	/* Reports the value the write hook kept. */
	return i915_xd_fake.previous;
}

/*
 * Puts an AUX sink behind a channel of the model: from now on a write of
 * the channel's control word is answered by the sink.  The sink's
 * memories are left as the test filled them; its script, its I2C offset
 * and its record start empty.
 */
void
i915_xd_fake_aux_attach(
	struct i915_xd_fake_aux *sink,
	unsigned channel)
{
	/* Starts the sink with no script and nothing seen. */
	sink->channel = channel;
	sink->i2c_offset = 0U;
	sink->script_length = 0U;
	sink->script_next = 0U;
	sink->transactions = 0U;
	sink->last_request_bytes = 0U;

	/* Answers every write of the channel's control word. */
	i915_xd_fake_rule_call(I915_XD_DDI_AUX_CTL(channel), i915_xd_fake_aux_write, sink);
}

/*
 * Scripts the sink's answer to the next count transactions after those
 * already scripted.
 */
void
i915_xd_fake_aux_script(
	struct i915_xd_fake_aux *sink,
	unsigned answer,
	unsigned param,
	unsigned count)
{
	unsigned index;

	/* Appends the answer count times. */
	for (index = 0U; index < count; index++) {
		/* A full script is a mistake in the test. */
		if (sink->script_length == I915_XD_FAKE_AUX_SCRIPT) {
			fprintf(stderr, "xd-fake-regs: AUX script too long\n");
			abort();
		}

		/* Appends the answer and its parameter. */
		sink->answers[sink->script_length] = answer;
		sink->params[sink->script_length] = param;
		sink->script_length++;
	}
}

/*
 * Puts a DDC target behind the GMBUS controller of the model: from now on
 * the controller's command, index, status and data registers are
 * answered by the target.  The memory is left as the test filled it; the
 * script, the segment, the offset and the record start empty.
 */
void
i915_xd_fake_gmbus_attach(
	struct i915_xd_fake_gmbus *target,
	unsigned address)
{
	/* Starts the target idle, with no script and nothing seen. */
	target->address = address;
	target->segment = 0U;
	target->offset = 0U;
	target->left = 0U;
	target->writing = 0;
	target->active = 0;
	target->refused = 0;
	target->stuck = 0;
	target->script_length = 0U;
	target->script_next = 0U;
	target->cycles = 0U;
	target->written_bytes = 0U;

	/* Answers the controller's registers. */
	i915_xd_fake_rule_call(I915_XD_GMBUS1, i915_xd_fake_gmbus_command, target);
	i915_xd_fake_rule_call(I915_XD_GMBUS5, i915_xd_fake_gmbus_index, target);
	i915_xd_fake_rule_call(I915_XD_GMBUS3, i915_xd_fake_gmbus_feed, target);
	i915_xd_fake_rule_read(I915_XD_GMBUS2, i915_xd_fake_gmbus_status, target);
	i915_xd_fake_rule_read(I915_XD_GMBUS3, i915_xd_fake_gmbus_data, target);
}

/*
 * Scripts the target's answer to the next count data cycles after those
 * already scripted.
 */
void
i915_xd_fake_gmbus_script(
	struct i915_xd_fake_gmbus *target,
	unsigned answer,
	unsigned count)
{
	unsigned index;

	/* Appends the answer count times. */
	for (index = 0U; index < count; index++) {
		/* A full script is a mistake in the test. */
		if (target->script_length == I915_XD_FAKE_GMBUS_SCRIPT) {
			fprintf(stderr, "xd-fake-regs: GMBUS script too long\n");
			abort();
		}

		/* Appends the answer. */
		target->answers[target->script_length] = answer;
		target->script_length++;
	}
}

/*
 * Reports how many register reads went through the hooks.
 */
unsigned
i915_xd_fake_reads(void)
{
	/* Reports the count since the last reset. */
	return i915_xd_fake.reads;
}

/*
 * Reports how many register writes went through the hooks.
 */
unsigned
i915_xd_fake_writes(void)
{
	/* Reports the count since the last reset. */
	return i915_xd_fake.writes;
}

/*
 * Reports how many times a lock was taken while a lock that must come
 * after it in the order was held.
 */
unsigned
i915_xd_fake_lock_violations(void)
{
	/* Reports the count since the last reset. */
	return i915_xd_fake.lock_violations;
}

/*
 * Reports how many xd locks are held now.
 */
unsigned
i915_xd_fake_locks_held(void)
{
	/* Reports the depth of the held locks. */
	return i915_xd_fake.held_count;
}

/* Answers a read of the window from the model. */
static uint32_t
i915_xd_fake_read_hook(
	void *context,
	uint32_t offset)
{
	uint32_t value;

	UNUSED_PARAMETER(context);

	/* Brings in every change that is due, then reads. */
	i915_xd_fake_run_events();
	i915_xd_fake.reads++;
	value = i915_xd_fake_get(offset);

	/* Lets the test answer a register it models itself. */
	value = i915_xd_fake_apply_readers(offset, value);

	/* Succeeded: reports the register's value. */
	return value;
}

/* Takes a write of the window into the model and runs the register's rules. */
static void
i915_xd_fake_write_hook(
	void *context,
	uint32_t offset,
	uint32_t value)
{
	(void)context;

	/* Keeps the value before the write for the rules, stores the value, then lets the hardware answer it. */
	i915_xd_fake.writes++;
	i915_xd_fake.previous = i915_xd_fake_get(offset);
	i915_xd_fake_set(offset, value);
	i915_xd_fake_apply_rules(offset, value);
}

/* Lets model time pass and brings in the changes that became due. */
static void
i915_xd_fake_wait_hook(
	void *context,
	unsigned microseconds)
{
	(void)context;

	/* Moves the clock and applies what is due. */
	i915_xd_fake.now += microseconds;
	i915_xd_fake_run_events();
}

/* Records a lock taken or released and counts a taking out of order. */
static void
i915_xd_fake_lock_hook(
	void *context,
	unsigned lock,
	int taken)
{
	unsigned index;

	UNUSED_PARAMETER(context);

	/* A release removes the newest holding of that lock. */
	if (taken == 0) {
		for (index = i915_xd_fake.held_count; index > 0U; index--) {
			if (i915_xd_fake.held[index - 1U] == lock) {
				i915_xd_fake.held[index - 1U] = i915_xd_fake.held[i915_xd_fake.held_count - 1U];
				i915_xd_fake.held_count--;
				return;
			}
		}

		/* Releasing a lock that is not held breaks the order too. */
		i915_xd_fake.lock_violations++;
		return;
	}

	/* A taking is out of order when a lock of the same or a later rank is held. */
	for (index = 0U; index < i915_xd_fake.held_count; index++) {
		if (i915_xd_fake.held[index] >= lock)
			i915_xd_fake.lock_violations++;
	}

	/* A full stack is a mistake in the test. */
	if (i915_xd_fake.held_count == I915_XD_FAKE_LOCKS) {
		fprintf(stderr, "xd-fake-regs: too many locks held\n");
		abort();
	}

	/* Records the lock as held. */
	i915_xd_fake.held[i915_xd_fake.held_count] = lock;
	i915_xd_fake.held_count++;
}

/* Finds a register of the model, adding it when create is nonzero. */
static struct i915_xd_fake_register *
i915_xd_fake_find(
	uint32_t offset,
	int create)
{
	struct i915_xd_fake_register *entry;
	unsigned index;

	/* Looks for the register among those the model has. */
	for (index = 0U; index < i915_xd_fake.register_count; index++) {
		if (i915_xd_fake.registers[index].offset == offset)
			return &i915_xd_fake.registers[index];
	}

	/* A register that is only read is not added. */
	if (create == 0)
		return NULL;

	/* A full table is a mistake in the test. */
	if (i915_xd_fake.register_count == I915_XD_FAKE_REGISTERS) {
		fprintf(stderr, "xd-fake-regs: too many registers\n");
		abort();
	}

	/* Adds the register with the value 0. */
	entry = &i915_xd_fake.registers[i915_xd_fake.register_count];
	entry->offset = offset;
	entry->value = 0U;
	i915_xd_fake.register_count++;

	/* Succeeded: reports the new register. */
	return entry;
}

/* Applies, in the order they were scheduled, the changes whose time has come. */
static void
i915_xd_fake_run_events(void)
{
	struct i915_xd_fake_event *event;
	uint32_t value;
	unsigned index;
	unsigned kept;

	/* Applies the due changes and keeps the others, in order. */
	kept = 0U;
	for (index = 0U; index < i915_xd_fake.event_count; index++) {
		event = &i915_xd_fake.events[index];

		/* A change still in the future stays pending. */
		if (event->due > i915_xd_fake.now) {
			i915_xd_fake.events[kept] = *event;
			kept++;
			continue;
		}

		/* Makes the change. */
		value = i915_xd_fake_get(event->offset);
		value = (value & ~event->clear) | event->set;
		i915_xd_fake_set(event->offset, value);
	}

	/* The events left. */
	i915_xd_fake.event_count = kept;
}

/* Runs the rules of a register just written. */
static void
i915_xd_fake_apply_rules(
	uint32_t offset,
	uint32_t value)
{
	struct i915_xd_fake_rule *rule;
	uint32_t covered;
	uint32_t moved;
	unsigned index;

	/* Runs every rule of the register, in the order they were added. */
	for (index = 0U; index < i915_xd_fake.rule_count; index++) {
		rule = &i915_xd_fake.rules[index];

		/* A rule of another register, or a read rule, does not answer a write. */
		if (rule->offset != offset)
			continue;
		if (rule->reader != NULL)
			continue;

		/* A test's function answers by itself. */
		if (rule->function != NULL) {
			rule->function(offset, value, rule->context);
			continue;
		}

		/* A mirror replaces the bits it covers in the target. */
		covered = i915_xd_fake_move(rule->source_mask, rule->shift);
		moved = i915_xd_fake_move(value & rule->source_mask, rule->shift);
		i915_xd_fake_later(rule->delay_us, rule->target, covered, moved);
	}
}

/*
 * Answers a write of an AUX control word as the channel does: a busy
 * channel ignores it; otherwise the done flag and the errors written as
 * 1 are cleared, the driver's own bits are kept, and the busy bit starts
 * a transaction.
 */
static void
i915_xd_fake_aux_write(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	struct i915_xd_fake_aux *sink;
	uint32_t previous;
	uint32_t word;

	/* A busy channel keeps the word it had. */
	sink = context;
	previous = i915_xd_fake_previous();
	if ((previous & I915_XD_AUX_CTL_BUSY) != 0U) {
		i915_xd_fake_set(offset, previous);
		return;
	}

	/* Clears the status bits written as 1 and takes the driver's bits. */
	word = previous & ~(value & (I915_XD_AUX_CTL_DONE | I915_XD_AUX_CTL_TIMEOUT_ERROR | I915_XD_AUX_CTL_RECEIVE_ERROR));
	word = (word & I915_XD_FAKE_AUX_STATUS) | (value & ~I915_XD_FAKE_AUX_STATUS);

	/* Without the busy bit nothing is sent. */
	if ((value & I915_XD_AUX_CTL_BUSY) == 0U) {
		i915_xd_fake_set(offset, word);
		return;
	}

	/* Sends the request. */
	i915_xd_fake_aux_run(sink, offset, value, word);
}

/*
 * Runs one transaction: reads the request out of the data words, takes
 * the scripted answer, and leaves the status and the reply.
 */
static void
i915_xd_fake_aux_run(
	struct i915_xd_fake_aux *sink,
	uint32_t offset,
	uint32_t value,
	uint32_t word)
{
	uint8_t request[20];
	uint8_t reply[20];
	uint32_t data;
	unsigned size;
	unsigned answer;
	unsigned param;
	unsigned count;
	unsigned index;
	int i2c;

	/* Reads the request the size names out of the data words, first byte highest. */
	size = i915_xd_field_get(I915_XD_AUX_CTL_SIZE, value);
	if (size > 20U)
		size = 20U;
	for (index = 0U; index < size; index++) {
		data = i915_xd_fake_get(I915_XD_DDI_AUX_DATA(sink->channel, index / 4U));
		request[index] = (uint8_t)(data >> (24U - 8U * (index % 4U)));
	}

	/* Records the request. */
	for (index = 0U; index < size; index++)
		sink->last_request[index] = request[index];
	sink->last_request_bytes = size;
	sink->transactions++;

	/* Takes the scripted answer; past the script the sink acknowledges. */
	answer = I915_XD_FAKE_AUX_ACK;
	param = 0U;
	if (sink->script_next < sink->script_length) {
		answer = sink->answers[sink->script_next];
		param = sink->params[sink->script_next];
		sink->script_next++;
	}

	/* Starts from a word with no status. */
	word &= ~I915_XD_FAKE_AUX_STATUS;

	/* Answers without a reply when the script says so. */
	switch (answer) {
	case I915_XD_FAKE_AUX_TIMEOUT:
		i915_xd_fake_set(offset, word | I915_XD_AUX_CTL_DONE | I915_XD_AUX_CTL_TIMEOUT_ERROR);
		return;
	case I915_XD_FAKE_AUX_DAMAGED:
		i915_xd_fake_set(offset, word | I915_XD_AUX_CTL_DONE | I915_XD_AUX_CTL_RECEIVE_ERROR);
		return;
	case I915_XD_FAKE_AUX_STUCK:
		i915_xd_fake_set(offset, word | I915_XD_AUX_CTL_BUSY);
		i915_xd_fake_later(param, offset, I915_XD_AUX_CTL_BUSY, I915_XD_AUX_CTL_DONE | I915_XD_AUX_CTL_TIMEOUT_ERROR);
		return;
	case I915_XD_FAKE_AUX_SIZE:
		i915_xd_fake_set(offset, word | I915_XD_AUX_CTL_DONE | i915_xd_field_put(I915_XD_AUX_CTL_SIZE, param));
		return;
	default:
		break;
	}

	/* Builds the reply: a refusal, a request to wait, or the sink's answer. */
	i2c = 0;
	if (size != 0U && ((request[0] >> 4) & I915_XD_FAKE_AUX_NATIVE) == 0U)
		i2c = 1;
	if (answer == I915_XD_FAKE_AUX_NACK) {
		reply[0] = I915_XD_FAKE_AUX_NATIVE_NACK;
		if (i2c != 0)
			reply[0] = I915_XD_FAKE_AUX_I2C_NACK;
		count = 1U;
	} else if (answer == I915_XD_FAKE_AUX_DEFER) {
		reply[0] = I915_XD_FAKE_AUX_NATIVE_DEFER;
		if (i2c != 0)
			reply[0] = I915_XD_FAKE_AUX_I2C_DEFER;
		count = 1U;
	} else {
		count = i915_xd_fake_aux_serve(sink, request, size, reply);
	}

	/* Loads the reply into the data words, first byte highest, the rest 0. */
	for (index = 0U; index < 5U; index++)
		i915_xd_fake_set(I915_XD_DDI_AUX_DATA(sink->channel, index), 0U);
	for (index = 0U; index < count; index++) {
		data = i915_xd_fake_get(I915_XD_DDI_AUX_DATA(sink->channel, index / 4U));
		data |= (uint32_t)reply[index] << (24U - 8U * (index % 4U));
		i915_xd_fake_set(I915_XD_DDI_AUX_DATA(sink->channel, index / 4U), data);
	}

	/* Ends the transaction done, with the reply's size. */
	i915_xd_fake_set(offset, word | I915_XD_AUX_CTL_DONE | i915_xd_field_put(I915_XD_AUX_CTL_SIZE, count));
}

/*
 * Serves an acknowledged request from the sink's memories and builds
 * the reply; returns the reply's length.  A native read answers the
 * bytes asked for, a native write stores them; an I2C read answers the
 * EEPROM's bytes from its offset on, a one-byte I2C write sets the
 * offset, and a bare address is answered with the acknowledgement only.
 */
static unsigned
i915_xd_fake_aux_serve(
	struct i915_xd_fake_aux *sink,
	const uint8_t *request,
	unsigned size,
	uint8_t *reply)
{
	unsigned command;
	unsigned address;
	unsigned length;
	unsigned index;

	/* Every answer starts with the acknowledgement. */
	reply[0] = 0x00U;

	/* A bare address (no length byte) only opens or closes an I2C transfer. */
	if (size < 4U)
		return 1U;

	/* Decodes the command, the address and the length less one. */
	command = request[0] >> 4;
	address = ((request[0] & 0x0fU) << 16) | ((unsigned)request[1] << 8) | request[2];
	length = (unsigned)request[3] + 1U;

	/* A native read answers the bytes asked for. */
	if (command == I915_XD_FAKE_AUX_NATIVE_READ) {
		for (index = 0U; index < length && index < 19U; index++)
			reply[1U + index] = sink->dpcd[(address + index) % I915_XD_FAKE_AUX_DPCD_BYTES];
		return 1U + index;
	}

	/* A native write stores the bytes it carries. */
	if ((command & I915_XD_FAKE_AUX_NATIVE) != 0U) {
		for (index = 0U; index < length && 4U + index < size; index++)
			sink->dpcd[(address + index) % I915_XD_FAKE_AUX_DPCD_BYTES] = request[4U + index];
		return 1U;
	}

	/* An I2C read answers the EEPROM's bytes from its offset on. */
	if ((command & I915_XD_FAKE_AUX_I2C_KIND) == I915_XD_FAKE_AUX_I2C_READ) {
		for (index = 0U; index < length && index < 19U; index++) {
			reply[1U + index] = sink->i2c[sink->i2c_offset % I915_XD_FAKE_AUX_I2C_BYTES];
			sink->i2c_offset++;
		}

		/* Succeeded: the bytes moved. */
		return 1U + index;
	}

	/* A one-byte I2C write sets the EEPROM's offset. */
	if (size > 4U)
		sink->i2c_offset = request[4];

	/* Succeeded: the write was taken. */
	return 1U;
}

/* Runs the read rules of a register being read and reports what the read sees. */
static uint32_t
i915_xd_fake_apply_readers(
	uint32_t offset,
	uint32_t value)
{
	struct i915_xd_fake_rule *rule;
	unsigned index;

	/* Lets every read rule of the register answer, in the order they were added. */
	for (index = 0U; index < i915_xd_fake.rule_count; index++) {
		rule = &i915_xd_fake.rules[index];
		if (rule->offset != offset || rule->reader == NULL)
			continue;

		/* The reader answers in place of the stored value. */
		value = rule->reader(offset, value, rule->context);
	}

	/* Succeeded: the value the read sees. */
	return value;
}

/*
 * Answers a write of the GMBUS command register as the controller and
 * the target do: a software clear ends a refusal and the cycle; a stop
 * ends the transfer (the bus stays active when the target lingers); a
 * ready command starts a data cycle, which the target refuses, or takes
 * as a read of its bytes from the offset, or as a write (the segment
 * pointer, an offset, or bytes it keeps).
 */
static void
i915_xd_fake_gmbus_command(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	struct i915_xd_fake_gmbus *target;
	uint32_t data;
	unsigned cycle;
	unsigned address;
	unsigned count;
	unsigned answer;
	unsigned index;

	UNUSED_PARAMETER(offset);
	target = context;

	/* The software clear resets the controller. */
	if ((value & I915_XD_GMBUS1_SW_CLEAR) != 0U) {
		target->refused = 0;
		target->active = 0;
		target->left = 0U;
		return;
	}

	/* Without software ready nothing starts. */
	if ((value & I915_XD_GMBUS1_SW_READY) == 0U)
		return;

	/* A stop ends the transfer. */
	cycle = i915_xd_field_get(I915_XD_GMBUS1_CYCLE, value);
	if ((cycle & I915_XD_GMBUS1_CYCLE_STOP) != 0U) {
		target->active = target->lingers;
		return;
	}

	/* A data cycle: the script decides how the target answers it. */
	if (target->cycles < I915_XD_FAKE_GMBUS_SCRIPT)
		target->commands[target->cycles] = value;
	target->cycles++;
	target->active = 1;
	target->writing = 0;
	target->left = 0U;
	answer = I915_XD_FAKE_GMBUS_ACK;
	if (target->script_next < target->script_length) {
		answer = target->answers[target->script_next];
		target->script_next++;
	}

	/* A stuck target holds the bus. */
	if (answer == I915_XD_FAKE_GMBUS_STUCK) {
		target->stuck = 1;
		return;
	}

	/* A refused cycle, or one for an address nobody answers. */
	address = i915_xd_field_get(I915_XD_GMBUS1_ADDRESS, value);
	if (answer == I915_XD_FAKE_GMBUS_NAK ||
	    (address != target->address &&
	     address != 0x30U)) {
		target->refused = 1;
		return;
	}

	/* An index cycle sets the offset first. */
	if ((cycle & I915_XD_GMBUS1_CYCLE_INDEX) != 0U)
		target->offset = i915_xd_field_get(I915_XD_GMBUS1_INDEX, value);

	/* A read has its bytes to deliver. */
	count = i915_xd_field_get(I915_XD_GMBUS1_BYTE_COUNT, value);
	if ((value & I915_XD_GMBUS1_READ) != 0U) {
		target->left = count;
		return;
	}

	/* A write's first bytes are in the data register already. */
	data = i915_xd_fake_get(I915_XD_GMBUS3);
	if (address == 0x30U) {
		target->segment = data & 0xffU;
		return;
	}

	/* A single byte sets the offset. */
	if (count == 1U) {
		target->offset = data & 0xffU;
		return;
	}

	/* A longer write is kept; the bytes after the first four come later. */
	target->written_bytes = 0U;
	for (index = 0U; index < 4U && index < count; index++) {
		target->written[target->written_bytes] = (uint8_t)(data >> (8U * index));
		target->written_bytes++;
	}

	/* The bytes left to write. */
	target->left = count - index;
	target->writing = 1;
}

/* Takes the bytes of a write cycle after its first four, as the driver feeds them. */
static void
i915_xd_fake_gmbus_feed(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	struct i915_xd_fake_gmbus *target;
	unsigned index;

	UNUSED_PARAMETER(offset);
	target = context;

	/* Only a write cycle with bytes still to come takes them. */
	if (target->writing == 0)
		return;

	/* Keeps up to four bytes, lowest first. */
	for (index = 0U; index < 4U && target->left != 0U; index++) {
		if (target->written_bytes < sizeof(target->written)) {
			target->written[target->written_bytes] = (uint8_t)(value >> (8U * index));
			target->written_bytes++;
		}

		/* One byte fewer to read. */
		target->left--;
	}
}

/* Answers a write of the two-byte index register: the offset, when enabled. */
static void
i915_xd_fake_gmbus_index(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	struct i915_xd_fake_gmbus *target;

	UNUSED_PARAMETER(offset);
	target = context;

	/* An enabled index is the offset of the next cycle. */
	if ((value & I915_XD_GMBUS5_TWO_BYTE_INDEX) != 0U)
		target->offset = value & 0xffffU;
}

/*
 * Answers a read of the GMBUS status: nothing while stuck, a refusal,
 * or ready and in the wait phase, active while bytes remain or the bus
 * lingers.
 */
static uint32_t
i915_xd_fake_gmbus_status(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	struct i915_xd_fake_gmbus *target;
	uint32_t status;

	UNUSED_PARAMETER(offset);
	UNUSED_PARAMETER(value);
	target = context;

	/* A stuck controller is only busy. */
	if (target->stuck != 0)
		return I915_XD_GMBUS2_ACTIVE;

	/* A refusal, with the bus still active when the target holds it. */
	if (target->refused != 0) {
		status = I915_XD_GMBUS2_NAK;
		if (target->stays_active != 0)
			status |= I915_XD_GMBUS2_ACTIVE;
		return status;
	}

	/* Ready, at the wait phase, and active while the cycle has bytes left or the bus lingers. */
	status = I915_XD_GMBUS2_HW_READY | I915_XD_GMBUS2_WAIT_PHASE;
	if (target->active != 0 &&
	    (target->left != 0U ||
	     target->lingers != 0))
		status |= I915_XD_GMBUS2_ACTIVE;

	/* Succeeded: the status the controller shows. */
	return status;
}

/*
 * Answers a read of the GMBUS data during a read cycle: the next four
 * bytes of the target's memory, lowest first.
 */
static uint32_t
i915_xd_fake_gmbus_data(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	struct i915_xd_fake_gmbus *target;
	uint32_t word;
	unsigned index;

	UNUSED_PARAMETER(offset);
	target = context;

	/* Outside a read the register holds what was written. */
	if (target->left == 0U || target->writing != 0)
		return value;

	/* Takes up to four bytes from the segment and the offset. */
	word = 0U;
	for (index = 0U; index < 4U && target->left != 0U; index++) {
		word |= (uint32_t)target->memory[(target->segment * 256U + target->offset) % I915_XD_FAKE_GMBUS_BYTES] << (8U * index);
		target->offset++;
		target->left--;
	}

	/* Succeeded: the bytes of this read. */
	return word;
}

/* Moves bits left (positive shift) or right (negative shift). */
static uint32_t
i915_xd_fake_move(
	uint32_t bits,
	int shift)
{
	/* A negative shift moves the bits towards bit 0. */
	if (shift < 0)
		return bits >> (unsigned)(-shift);

	/* Succeeded: the bits moved up. */
	return bits << (unsigned)shift;
}
