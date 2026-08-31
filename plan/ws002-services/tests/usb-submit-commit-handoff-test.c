/*
 * Deterministic regression for the USB submit/terminal-publication handoff.
 *
 * The executable model proves the single-CPU interrupt cycle which formerly
 * deadlocked.  The source-order gate binds that model to the production USB
 * core without adding a test callback to the interrupt-sensitive path.
 *
 * SPDX-License-Identifier: Zlib
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct handoff {
	bool irq_enabled;
	bool irq_pending;
	bool commit_present;
	bool submit_pending;
	bool finished;
	bool deadlocked;
};

static void
finish_commit(struct handoff *state)
{
	state->submit_pending = false;
	state->finished = true;
}

static void
terminal_interrupt(struct handoff *state)
{
	if (!state->irq_enabled) {
		state->irq_pending = true;
		return;
	}
	if (state->commit_present) {
		state->commit_present = false;
		finish_commit(state);
		return;
	}
	/* The interrupt cannot yield back to the submitter it preempted. */
	if (state->submit_pending)
		state->deadlocked = true;
}

static struct handoff
old_handoff(void)
{
	struct handoff state = { true, false, true, true, false, false };

	state.commit_present = false; /* submitter's atomic exchange */
	terminal_interrupt(&state);   /* local completion preempts here */
	if (!state.deadlocked)
		finish_commit(&state);
	return state;
}

static struct handoff
irq_masked_handoff(void)
{
	struct handoff state = { true, false, true, true, false, false };

	state.irq_enabled = false;
	state.commit_present = false; /* submitter's atomic exchange */
	terminal_interrupt(&state);   /* latched, not dispatched */
	finish_commit(&state);
	state.irq_enabled = true;
	if (state.irq_pending) {
		state.irq_pending = false;
		terminal_interrupt(&state);
	}
	return state;
}

static char *
read_source(const char *path)
{
	FILE *file;
	long length;
	char *text;

	file = fopen(path, "rb");
	if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
	    (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
		if (file != NULL)
			fclose(file);
		return NULL;
	}
	text = malloc((size_t)length + 1U);
	if (text == NULL || fread(text, 1, (size_t)length, file) !=
	    (size_t)length) {
		free(text);
		fclose(file);
		return NULL;
	}
	text[length] = '\0';
	fclose(file);
	return text;
}

static const char *
ordered(const char *cursor, const char *needle, const char *description)
{
	const char *match = strstr(cursor, needle);

	if (match == NULL) {
		fprintf(stderr, "missing or out-of-order production step: %s\n",
		    description);
		exit(1);
	}
	return match + strlen(needle);
}

int
main(int argc, char **argv)
{
	struct handoff old = old_handoff();
	struct handoff fixed = irq_masked_handoff();
	char *source;
	const char *cursor, *end;

	if (argc != 2) {
		fprintf(stderr, "usage: %s src/drivers/usb.c\n", argv[0]);
		return 2;
	}
	if (!old.deadlocked || old.finished || !old.submit_pending) {
		fprintf(stderr, "old handoff did not reproduce self-wait\n");
		return 1;
	}
	if (fixed.deadlocked || !fixed.finished || fixed.submit_pending ||
	    fixed.irq_pending) {
		fprintf(stderr, "IRQ-masked handoff did not close self-wait\n");
		return 1;
	}
	source = read_source(argv[1]);
	if (source == NULL) {
		fprintf(stderr, "cannot read production source: %s\n", argv[1]);
		return 1;
	}
	cursor = ordered(source, "drv_usb_urb_submit(struct drv_usb_urb *urb)",
	    "production submit function");
	end = strstr(cursor, "static int urb_cancel_to(");
	if (end == NULL) {
		fprintf(stderr, "cannot bound production submit function\n");
		free(source);
		return 1;
	}
	cursor = ordered(cursor, "irq_enabled = hal_irq_disable();",
	    "mask local IRQ before claiming commit");
	if (cursor >= end)
		goto outside;
	cursor = ordered(cursor,
	    "claimed = __atomic_exchange_n(&urb->submit_commit, NULL,",
	    "claim commit ownership");
	if (cursor >= end)
		goto outside;
	cursor = ordered(cursor, "submit_commit_finish(urb, claimed);",
	    "finish claimed commit");
	if (cursor >= end)
		goto outside;
	cursor = ordered(cursor, "if (irq_enabled)\n\t\thal_irq_enable();",
	    "restore IRQ after commit finish");
	if (cursor >= end)
		goto outside;
	free(source);
	puts("USB submit commit IRQ handoff: PASS");
	return 0;

outside:
	fprintf(stderr, "production handoff step escaped submit function\n");
	free(source);
	return 1;
}
