/* ws004-p031 amd64 PC/AT console output serialization regression. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <hal/hal.h>

#define TEST_CPUS 4U
#define TEST_ROUNDS 400U
#define FRAMEBUFFER_WIDTH (HAL_CONS_COLUMNS * 8U)
#define FRAMEBUFFER_HEIGHT (HAL_CONS_ROWS * 16U)
#define FRAMEBUFFER_PIXELS (FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT)
#define GUARD_PIXELS 32U
#define GUARD_VALUE 0xa55aa55aU

void pcat_console_output_test_set_cpu(uint32_t);
void pcat_console_output_test_reset(uint32_t *, size_t);
void pcat_console_output_test_reentrant_transient(int);
int pcat_console_output_test_state(unsigned *, unsigned *);

/* The renderer only needs a stable, addressable glyph table in this test. */
const uint8_t pcat_vgafont16[256U * 16U] = { 0 };

void
asm_outb(uint16_t port, uint8_t value)
{
	(void)port;
	(void)value;
}

struct worker_argument {
	uint32_t cpu;
};

static void *
output_worker(void *opaque)
{
	const struct worker_argument *argument = opaque;
	unsigned round;

	pcat_console_output_test_set_cpu(argument->cpu);
	for (round = 0; round < TEST_ROUNDS; round++) {
		hal_cons_write("parallel-output");
		hal_cons_putc('\t');
		hal_cons_putc('0' + (int)argument->cpu);
		hal_cons_putc('\n');
		if ((round & 31U) == 0)
			hal_cons_clear_to_eol();
	}
	return NULL;
}

static void
assert_guards(const uint32_t *storage)
{
	unsigned index;

	for (index = 0; index < GUARD_PIXELS; index++) {
		assert(storage[index] == GUARD_VALUE);
		assert(storage[GUARD_PIXELS + FRAMEBUFFER_PIXELS + index] ==
		    GUARD_VALUE);
	}
}

int
main(void)
{
	struct worker_argument arguments[TEST_CPUS];
	pthread_t workers[TEST_CPUS];
	uint32_t *storage, *pixels;
	unsigned column, index, row;

	storage = malloc((FRAMEBUFFER_PIXELS + 2U * GUARD_PIXELS) *
	    sizeof(*storage));
	assert(storage != NULL);
	for (index = 0; index < FRAMEBUFFER_PIXELS + 2U * GUARD_PIXELS;
	    index++)
		storage[index] = GUARD_VALUE;
	pixels = storage + GUARD_PIXELS;

	pcat_console_output_test_set_cpu(0);
	pcat_console_output_test_reset(pixels, FRAMEBUFFER_PIXELS);
	assert(pcat_console_output_test_state(&row, &column));
	assert(row < HAL_CONS_ROWS && column < HAL_CONS_COLUMNS);
	assert_guards(storage);

	/* A same-CPU fault/NMI may re-enter while newline exposes row==ROWS. */
	pcat_console_output_test_reentrant_transient('x');
	assert(pcat_console_output_test_state(&row, &column));
	assert(row == HAL_CONS_ROWS - 1U && column == 0);
	assert_guards(storage);

	assert(hal_cons_write_n_at(HAL_CONS_ROWS, 0, "x", 1, 7) == -1);
	assert(hal_cons_write_n_at(0, HAL_CONS_COLUMNS, "x", 1, 7) == -1);
	assert(!hal_cons_set_cursor(HAL_CONS_ROWS, 0));
	assert(!hal_cons_set_cursor(0, HAL_CONS_COLUMNS));
	assert_guards(storage);

	for (index = 0; index < TEST_CPUS; index++) {
		arguments[index].cpu = index + 1U;
		assert(pthread_create(&workers[index], NULL, output_worker,
		    &arguments[index]) == 0);
	}
	for (index = 0; index < TEST_CPUS; index++)
		assert(pthread_join(workers[index], NULL) == 0);

	pcat_console_output_test_set_cpu(0);
	assert(pcat_console_output_test_state(&row, &column));
	assert(row < HAL_CONS_ROWS && column < HAL_CONS_COLUMNS);
	assert_guards(storage);
	free(storage);
	puts("HW-T27 amd64 console output serialization: PASS");
	return 0;
}
