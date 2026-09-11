/* Focused model for ws004-p006 URB completion publication. */
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

enum model_status {
	MODEL_PENDING = 1,
	MODEL_COMPLETE = 2,
	MODEL_TIMEOUT = 5
};

struct model_urb {
	atomic_uint status;
	atomic_uint terminal_claimed;
	size_t actual_length;
};

struct legacy_context {
	struct model_urb urb;
	atomic_uint observed_status;
	atomic_uint consumer_done;
	size_t observed_actual;
};

static void *
legacy_producer(void *argument)
{
	struct legacy_context *context = argument;

	/* This is the generated pre-fix order: terminal state, then payload. */
	atomic_store_explicit(&context->urb.status, MODEL_COMPLETE,
	    memory_order_release);
	while (atomic_load_explicit(&context->consumer_done,
	    memory_order_acquire) == 0)
		;
	context->urb.actual_length = 4096;
	return NULL;
}

static void *
legacy_consumer(void *argument)
{
	struct legacy_context *context = argument;

	while (atomic_load_explicit(&context->urb.status,
	    memory_order_acquire) == MODEL_PENDING)
		;
	context->observed_actual = context->urb.actual_length;
	atomic_store_explicit(&context->observed_status, MODEL_COMPLETE,
	    memory_order_release);
	atomic_store_explicit(&context->consumer_done, 1U, memory_order_release);
	return NULL;
}

struct corrected_context {
	struct model_urb urb;
	unsigned iterations;
	atomic_uint acknowledged;
};

static void *
corrected_producer(void *argument)
{
	struct corrected_context *context = argument;
	unsigned iteration;

	for (iteration = 1; iteration <= context->iterations; iteration++) {
		while (atomic_load_explicit(&context->acknowledged,
		    memory_order_acquire) != iteration - 1U)
			;
		context->urb.actual_length = (size_t)iteration;
		atomic_store_explicit(&context->urb.status, MODEL_COMPLETE,
		    memory_order_release);
	}
	return NULL;
}

static void *
corrected_consumer(void *argument)
{
	struct corrected_context *context = argument;
	unsigned iteration;

	for (iteration = 1; iteration <= context->iterations; iteration++) {
		while (atomic_load_explicit(&context->urb.status,
		    memory_order_acquire) != MODEL_COMPLETE)
			;
		if (context->urb.actual_length != (size_t)iteration)
			return (void *)1;
		atomic_store_explicit(&context->urb.status, MODEL_PENDING,
		    memory_order_relaxed);
		atomic_store_explicit(&context->acknowledged, iteration,
		    memory_order_release);
	}
	return NULL;
}

struct claimant {
	struct model_urb *urb;
	unsigned status;
	size_t actual;
};

static void *
claim_terminal(void *argument)
{
	struct claimant *claimant = argument;
	unsigned expected = 0;

	if (atomic_compare_exchange_strong_explicit(
	    &claimant->urb->terminal_claimed, &expected, 1U,
	    memory_order_acq_rel, memory_order_acquire)) {
		claimant->urb->actual_length = claimant->actual;
		atomic_store_explicit(&claimant->urb->status, claimant->status,
		    memory_order_release);
	}
	return NULL;
}

static int
test_legacy_order(void)
{
	struct legacy_context context = { 0 };
	pthread_t producer, consumer;

	atomic_init(&context.urb.status, MODEL_PENDING);
	atomic_init(&context.urb.terminal_claimed, 0);
	atomic_init(&context.observed_status, MODEL_PENDING);
	atomic_init(&context.consumer_done, 0);
	if (pthread_create(&producer, NULL, legacy_producer, &context) != 0 ||
	    pthread_create(&consumer, NULL, legacy_consumer, &context) != 0)
		return 1;
	if (pthread_join(producer, NULL) != 0 ||
	    pthread_join(consumer, NULL) != 0)
		return 1;
	return atomic_load_explicit(&context.observed_status,
	    memory_order_acquire) != MODEL_COMPLETE ||
	    context.observed_actual != 0;
}

static int
test_corrected_order(void)
{
	struct corrected_context context = { 0 };
	pthread_t producer, consumer;
	void *consumer_result = NULL;

	context.iterations = 200000U;
	atomic_init(&context.urb.status, MODEL_PENDING);
	atomic_init(&context.urb.terminal_claimed, 0);
	atomic_init(&context.acknowledged, 0);
	if (pthread_create(&producer, NULL, corrected_producer, &context) != 0 ||
	    pthread_create(&consumer, NULL, corrected_consumer, &context) != 0)
		return 1;
	if (pthread_join(producer, NULL) != 0 ||
	    pthread_join(consumer, &consumer_result) != 0)
		return 1;
	return consumer_result != NULL;
}

static int
test_single_terminal_owner(void)
{
	unsigned iteration;

	for (iteration = 0; iteration < 2000U; iteration++) {
		struct model_urb urb = { 0 };
		struct claimant complete = { &urb, MODEL_COMPLETE, 4096U };
		struct claimant timeout = { &urb, MODEL_TIMEOUT, 0 };
		pthread_t first, second;
		unsigned status;

		atomic_init(&urb.status, MODEL_PENDING);
		atomic_init(&urb.terminal_claimed, 0);
		if (pthread_create(&first, NULL, claim_terminal, &complete) != 0 ||
		    pthread_create(&second, NULL, claim_terminal, &timeout) != 0)
			return 1;
		if (pthread_join(first, NULL) != 0 ||
		    pthread_join(second, NULL) != 0)
			return 1;
		status = atomic_load_explicit(&urb.status, memory_order_acquire);
		if (!((status == MODEL_COMPLETE && urb.actual_length == 4096U) ||
		    (status == MODEL_TIMEOUT && urb.actual_length == 0)))
			return 1;
	}
	return 0;
}

int
main(void)
{
	if (test_legacy_order() != 0) {
		fprintf(stderr, "legacy publication model did not expose stale zero\n");
		return EXIT_FAILURE;
	}
	if (test_corrected_order() != 0) {
		fprintf(stderr, "release/acquire publication model failed\n");
		return EXIT_FAILURE;
	}
	if (test_single_terminal_owner() != 0) {
		fprintf(stderr, "terminal ownership model failed\n");
		return EXIT_FAILURE;
	}
	puts("usb URB publication model: PASS");
	return EXIT_SUCCESS;
}
