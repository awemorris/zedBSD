/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DMA coherency procedure (see vf2-dmatest-core.h).
 *
 * Each run writes a source through the cached view without flushing it,
 * reads the whole destination through the cached view so that clean lines
 * of its old contents sit in the cache, has the bus master copy, and
 * compares the destination read through the cached view with the source.
 * A coherent bus master reads the dirty source lines and replaces the
 * stale destination lines; a non-coherent one does neither.  The alias
 * only classifies a mismatch, and only when both controls passed.
 */

#include <drivers/platform/visionfive2/vf2-dmatest-core.h>

/* The words of one buffer. */
#define DMATEST_WORDS		(DRV_VF2_DMATEST_BUFFER / 8U)

/* What a pattern word is for: tags that keep every pattern distinct. */
#define DMATEST_TAG_ALIAS_A	0x0aU
#define DMATEST_TAG_ALIAS_B	0x0bU
#define DMATEST_TAG_CONTROL	0x0cU
#define DMATEST_TAG_FORWARD	0x10U
#define DMATEST_TAG_REVERSE	0x20U

/* The digits of a hexadecimal number. */
static const char dmatest_digits[] = "0123456789abcdef";

/*
 * Where a text being formatted stands.
 *
 * Length counts every byte asked for, even past the capacity, so the
 * caller can tell the text was cut.
 */
struct dmatest_text {
	char *text;
	size_t capacity;
	size_t length;
};

static uint64_t pattern(size_t offset, unsigned run, unsigned tag);
static void fill(const struct drv_vf2_dmatest_ops *ops, int view, size_t base, unsigned run, unsigned tag);
static void touch(const struct drv_vf2_dmatest_ops *ops, size_t base);
static int find_mismatch(const struct drv_vf2_dmatest_ops *ops, int view, size_t base, unsigned run, unsigned tag, size_t *offset);
static int check_alias(const struct drv_vf2_dmatest_ops *ops, size_t base);
static int check_control(const struct drv_vf2_dmatest_ops *ops, size_t base);
static int copy_and_compare(const struct drv_vf2_dmatest_ops *ops, struct drv_vf2_dmatest_result *result, unsigned run, int reverse, size_t destination, size_t source, unsigned tag);
static void put_string(struct dmatest_text *text, const char *string);
static void put_number(struct dmatest_text *text, uint64_t value, unsigned base);

/*
 * Runs the controls and the eight runs of both directions.
 */
void
drv_vf2_dmatest_run(
	const struct drv_vf2_dmatest_ops *ops,
	struct drv_vf2_dmatest_result *result)
{
	size_t source;
	size_t destination;
	size_t reverse;
	unsigned run;
	int error;

	/* Starts from a coherent answer with no mismatch. */
	result->alias = DRV_VF2_DMATEST_ALIAS_MISMATCH;
	result->control = DRV_VF2_DMATEST_CONTROL_SKIPPED;
	result->verdict = DRV_VF2_DMATEST_COHERENT;
	result->direction = DRV_VF2_DMATEST_UNCLASSIFIED;
	result->mismatches = 0;
	result->run = 0;
	result->reverse = 0;
	result->offset = 0;
	result->copy_error = 0;

	/* Starts with nothing of the region in the cache. */
	ops->flush(ops->context, 0, DRV_VF2_DMATEST_REGION);

	/* The controls use the first run's destination. */
	result->alias = check_alias(ops, DRV_VF2_DMATEST_BUFFER);
	if (result->alias == DRV_VF2_DMATEST_ALIAS_OK)
		result->control = check_control(ops, DRV_VF2_DMATEST_BUFFER);

	/* Drops the lines the controls left in the cache. */
	ops->flush(ops->context, 0, DRV_VF2_DMATEST_REGION);

	/* Runs both directions with the buffers moved a step further each time. */
	for (run = 0; run < DRV_VF2_DMATEST_RUNS; run++) {
		/* Places the run's source, destination and reverse destination side by side. */
		source = run * DRV_VF2_DMATEST_STEP;
		destination = source + DRV_VF2_DMATEST_BUFFER;
		reverse = destination + DRV_VF2_DMATEST_BUFFER;

		/* The CPU's dirty source, then the device's write over stale clean lines. */
		error = copy_and_compare(ops, result, run, 0, destination, source, DMATEST_TAG_FORWARD);
		if (error != 0)
			break;

		/* The CPU writes the device's destination back, and the device copies it on. */
		error = copy_and_compare(ops, result, run, 1, reverse, destination, DMATEST_TAG_REVERSE);
		if (error != 0)
			break;
	}

	/* Leaves no stale line of the region for the next owner of the memory. */
	ops->flush(ops->context, 0, DRV_VF2_DMATEST_REGION);
}

/*
 * Writes the result as text lines naming the bus master.
 */
size_t
drv_vf2_dmatest_format(
	const struct drv_vf2_dmatest_result *result,
	const char *master,
	char *text,
	size_t capacity)
{
	struct dmatest_text out;

	/* Starts an empty text. */
	out.text = text;
	out.capacity = capacity;
	out.length = 0;

	/* The first control. */
	put_string(&out, "DMATEST ");
	put_string(&out, master);
	if (result->alias == DRV_VF2_DMATEST_ALIAS_OK)
		put_string(&out, " ALIAS OK\n");
	else
		put_string(&out, " ALIAS MISMATCH\n");

	/* The second control. */
	put_string(&out, "DMATEST ");
	put_string(&out, master);

	/* Chooses the word of the second control. */
	switch (result->control) {
	case DRV_VF2_DMATEST_CONTROL_OK:
		put_string(&out, " CONTROL OK\n");
		break;
	case DRV_VF2_DMATEST_CONTROL_INCONCLUSIVE:
		put_string(&out, " CONTROL INCONCLUSIVE\n");
		break;
	default:
		put_string(&out, " CONTROL SKIPPED\n");
		break;
	}

	/* The answer line. */
	put_string(&out, "DMATEST ");
	put_string(&out, master);

	/* Chooses the answer's words. */
	switch (result->verdict) {
	case DRV_VF2_DMATEST_COHERENT:
		put_string(&out, " PASS COHERENT runs=");
		put_number(&out, DRV_VF2_DMATEST_RUNS, 10U);
		put_string(&out, " bytes=");
		put_number(&out, DRV_VF2_DMATEST_BUFFER, 10U);
		break;
	case DRV_VF2_DMATEST_COPY_FAILED:
		put_string(&out, " FAIL copy run=");
		put_number(&out, result->run, 10U);
		put_string(&out, " error=");
		put_number(&out, (uint64_t)result->copy_error, 10U);
		break;
	default:
		put_string(&out, " FAIL run=");
		put_number(&out, result->run, 10U);
		if (result->reverse)
			put_string(&out, " reverse");
		put_string(&out, " offset=0x");
		put_number(&out, result->offset, 16U);
		put_string(&out, " view=cached NONCOHERENT direction=");
		if (result->direction == DRV_VF2_DMATEST_CPU_TO_DEVICE)
			put_string(&out, "cpu-to-device");
		else if (result->direction == DRV_VF2_DMATEST_DEVICE_TO_CPU)
			put_string(&out, "device-to-cpu");
		else
			put_string(&out, "unclassified");
		put_string(&out, " mismatches=");
		put_number(&out, result->mismatches, 10U);
		break;
	}

	/* Ends the line. */
	put_string(&out, "\n");

	/* Terminates the text, cut to the capacity. */
	if (capacity != 0) {
		if (out.length < capacity)
			text[out.length] = '\0';
		else
			text[capacity - 1U] = '\0';
	}

	/* Succeeded: the length the whole text needs. */
	return out.length;
}

/*
 * Makes one pattern word from its byte offset in a buffer, the run and the
 * tag, so that no two writes of the test leave the same word.
 */
static uint64_t
pattern(
	size_t offset,
	unsigned run,
	unsigned tag)
{
	uint64_t value;

	/* Mixes the three into one word (a 64-bit multiply-xorshift). */
	value = (uint64_t)offset;
	value ^= (uint64_t)run << 40;
	value ^= (uint64_t)tag << 52;
	value *= 0x9e3779b97f4a7c15ULL;
	value ^= value >> 29;

	/* Succeeded: the word. */
	return value;
}

/*
 * Writes a pattern over one buffer through a view.
 */
static void
fill(
	const struct drv_vf2_dmatest_ops *ops,
	int view,
	size_t base,
	unsigned run,
	unsigned tag)
{
	uint64_t value;
	size_t word;

	/* Writes every word of the buffer. */
	for (word = 0; word < DMATEST_WORDS; word++) {
		value = pattern(word * 8U, run, tag);
		ops->store(ops->context, view, base + word * 8U, value);
	}
}

/*
 * Reads every word of a buffer through the cached view, so that its lines
 * sit clean in the cache.
 */
static void
touch(
	const struct drv_vf2_dmatest_ops *ops,
	size_t base)
{
	size_t word;

	/* Reads every word and throws the value away. */
	for (word = 0; word < DMATEST_WORDS; word++)
		(void)ops->load(ops->context, DRV_VF2_DMATEST_CACHED, base + word * 8U);
}

/*
 * Compares one buffer read through a view with a pattern.  Reports
 * whether a word differs and the byte offset of the first.
 */
static int
find_mismatch(
	const struct drv_vf2_dmatest_ops *ops,
	int view,
	size_t base,
	unsigned run,
	unsigned tag,
	size_t *offset)
{
	uint64_t expected;
	uint64_t value;
	size_t word;

	/* Stops at the first word that differs. */
	for (word = 0; word < DMATEST_WORDS; word++) {
		value = ops->load(ops->context, view, base + word * 8U);
		expected = pattern(word * 8U, run, tag);
		if (value != expected) {
			*offset = word * 8U;
			return 1;
		}
	}

	/* Every word matched. */
	return 0;
}

/*
 * The first control: a pattern written through the cached view and
 * flushed reads back through the alias, and one written through the alias
 * reads back through the cached view after a flush.
 */
static int
check_alias(
	const struct drv_vf2_dmatest_ops *ops,
	size_t base)
{
	size_t offset;
	int differs;

	/* Writes A through the cache and puts it in memory. */
	fill(ops, DRV_VF2_DMATEST_CACHED, base, 0, DMATEST_TAG_ALIAS_A);
	ops->flush(ops->context, base, DRV_VF2_DMATEST_BUFFER);

	/* The alias must read A. */
	differs = find_mismatch(ops, DRV_VF2_DMATEST_ALIAS, base, 0, DMATEST_TAG_ALIAS_A, &offset);
	if (differs)
		return DRV_VF2_DMATEST_ALIAS_MISMATCH;

	/* Writes B through the alias and drops lines a speculative read may have brought in. */
	fill(ops, DRV_VF2_DMATEST_ALIAS, base, 0, DMATEST_TAG_ALIAS_B);
	ops->flush(ops->context, base, DRV_VF2_DMATEST_BUFFER);

	/* The cached view must read B. */
	differs = find_mismatch(ops, DRV_VF2_DMATEST_CACHED, base, 0, DMATEST_TAG_ALIAS_B, &offset);
	if (differs)
		return DRV_VF2_DMATEST_ALIAS_MISMATCH;

	/* Succeeded: the alias is the same memory. */
	return DRV_VF2_DMATEST_ALIAS_OK;
}

/*
 * The second control: with B's lines clean in the cache, a write through
 * the alias must leave the cached view reading the old B.  That shows the
 * test can see a stale line at all.
 */
static int
check_control(
	const struct drv_vf2_dmatest_ops *ops,
	size_t base)
{
	size_t offset;
	int differs;

	/* Brings B's lines in clean, then changes memory behind them. */
	touch(ops, base);
	fill(ops, DRV_VF2_DMATEST_ALIAS, base, 0, DMATEST_TAG_CONTROL);

	/* A stale view still reads B everywhere. */
	differs = find_mismatch(ops, DRV_VF2_DMATEST_CACHED, base, 0, DMATEST_TAG_ALIAS_B, &offset);
	if (differs)
		return DRV_VF2_DMATEST_CONTROL_INCONCLUSIVE;

	/* Succeeded: a stale line is visible. */
	return DRV_VF2_DMATEST_CONTROL_OK;
}

/*
 * One copy of a run: the CPU writes the source through the cache without
 * flushing, brings the destination's old lines in clean, the device
 * copies, and the destination read through the cache must be the source.
 * Reports nonzero only when the copy failed, which ends the test.
 */
static int
copy_and_compare(
	const struct drv_vf2_dmatest_ops *ops,
	struct drv_vf2_dmatest_result *result,
	unsigned run,
	int reverse,
	size_t destination,
	size_t source,
	unsigned tag)
{
	uint64_t expected;
	uint64_t aliased;
	size_t offset;
	int differs;
	int error;

	/* Dirty source lines, and clean stale destination lines. */
	fill(ops, DRV_VF2_DMATEST_CACHED, source, run, tag);
	touch(ops, destination);

	/* The device copies at once. */
	error = ops->copy(ops->context, destination, source, DRV_VF2_DMATEST_BUFFER);
	if (error != 0) {
		result->verdict = DRV_VF2_DMATEST_COPY_FAILED;
		result->run = run;
		result->reverse = reverse;
		result->copy_error = error;
		return error;
	}

	/* The cached view is the judge. */
	differs = find_mismatch(ops, DRV_VF2_DMATEST_CACHED, destination, run, tag, &offset);
	if (!differs)
		return 0;

	/* Counts the mismatch. */
	result->mismatches++;

	/* Only the first mismatch names the answer. */
	if (result->verdict == DRV_VF2_DMATEST_NONCOHERENT)
		return 0;
	result->verdict = DRV_VF2_DMATEST_NONCOHERENT;
	result->run = run;
	result->reverse = reverse;
	result->offset = offset;

	/* Without both controls the alias says nothing about the direction. */
	if (result->alias != DRV_VF2_DMATEST_ALIAS_OK || result->control != DRV_VF2_DMATEST_CONTROL_OK)
		return 0;

	/* Memory holding the source means the CPU's cache kept the device's write from the cached view. */
	aliased = ops->load(ops->context, DRV_VF2_DMATEST_ALIAS, destination + offset);
	expected = pattern(offset, run, tag);
	if (aliased == expected)
		result->direction = DRV_VF2_DMATEST_DEVICE_TO_CPU;
	else
		result->direction = DRV_VF2_DMATEST_CPU_TO_DEVICE;

	/* The run goes on; only a failed copy ends the test. */
	return 0;
}

/*
 * Appends a string to the text.
 */
static void
put_string(
	struct dmatest_text *text,
	const char *string)
{
	/* Copies the bytes that fit and counts every byte. */
	while (*string != '\0') {
		if (text->length + 1U < text->capacity)
			text->text[text->length] = *string;
		text->length++;
		string++;
	}
}

/*
 * Appends a number in base 10 or 16 to the text.
 */
static void
put_number(
	struct dmatest_text *text,
	uint64_t value,
	unsigned base)
{
	char digits[24];
	unsigned count;

	/* Writes the digits from the lowest. */
	count = 0;
	do {
		digits[count] = dmatest_digits[value % base];
		count++;
		value /= base;
	} while (value != 0);

	/* Appends them from the highest. */
	while (count > 0) {
		count--;
		if (text->length + 1U < text->capacity)
			text->text[text->length] = digits[count];
		text->length++;
	}
}
