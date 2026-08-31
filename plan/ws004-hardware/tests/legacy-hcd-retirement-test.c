/* WS004-p016 checked UHCI/EHCI request-retirement model. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define UHCI_FRAME_COUNT 1024U
#define MODEL_TD_ACTIVE 0x01U
#define MODEL_TD_ERROR 0x02U
#define MODEL_UHCI_FRNUM_MASK 0x07ffU
#define MODEL_UHCI_CMD_RUN 0x0001U
#define MODEL_UHCI_STS_HOST_SYSTEM_ERROR 0x0008U
#define MODEL_UHCI_STS_PROCESS_ERROR 0x0010U
#define MODEL_UHCI_STS_HALTED 0x0020U
#define MODEL_UHCI_TD_ACTIVE 0x00800000U
#define MODEL_UHCI_TD_ERRORS 0x007e0000U
#define MODEL_UHCI_TOKEN_TOGGLE 0x00080000U
#define MODEL_EHCI_QH_TOGGLE 0x80000000U

enum model_dispatch {
	MODEL_DISPATCH_INLINE,
	MODEL_DISPATCH_DEFERRED
};

enum model_state {
	MODEL_ACTIVE,
	MODEL_DEACTIVATING_COMPLETE,
	MODEL_DEACTIVATING_CANCEL,
	MODEL_WAIT_COMPLETE,
	MODEL_WAIT_CANCEL,
	MODEL_RETIRED_CANCEL,
	MODEL_COMPLETING,
	MODEL_FAILED,
	MODEL_RELEASED
};

struct model_request {
	enum model_state state;
	unsigned active;
	unsigned hcd_data;
	unsigned schedule_dma;
	unsigned bounce_dma;
	unsigned unlinked;
	unsigned generation;
	unsigned completion_count;
	unsigned free_count;
	unsigned callback_under_lock;
	unsigned stale_ack_count;
	unsigned matching_ack_count;
	unsigned disconnect_count;
	unsigned completion_sequence;
	uint16_t unlink_frame;
};

struct model_controller {
	unsigned lock_held;
	unsigned quiescing;
	unsigned quarantined;
	unsigned async_enabled;
	unsigned async_status;
	unsigned running;
	unsigned halted;
	unsigned host_system_error;
	unsigned iaad;
	unsigned iaa;
	unsigned worker_context;
	unsigned retirement_stopping;
	unsigned retirement_pending;
	unsigned completion_inflight;
	unsigned retirement_wakeups;
	unsigned inline_progress_count;
	unsigned hardware_stop_count;
	unsigned halt_attempt_count;
	unsigned master_disable_count;
	unsigned irq_drain_count;
	unsigned root_stop_count;
	unsigned root_join_count;
	unsigned root_joined;
	unsigned worker_stop_count;
	unsigned worker_join_count;
	unsigned worker_joined;
	unsigned sequence;
	unsigned root_stop_sequence;
	unsigned root_join_sequence;
	unsigned worker_stop_sequence;
	unsigned worker_join_sequence;
	unsigned hardware_stop_sequence;
	unsigned dma_quiesced;
	unsigned frame_list[UHCI_FRAME_COUNT];
	unsigned generation;
};

struct claim_shared {
	atomic_uint start;
	atomic_uint owner;
	atomic_uint releases;
	atomic_uint hcd_completions;
	atomic_uint core_completions;
};

struct claim_race {
	struct claim_shared *shared;
	unsigned contender;
};

static unsigned checks;

#define CHECK(expression) do { assert(expression); checks++; } while (0)

static int model_uhci_frame_advance(uint16_t, uint16_t, uint16_t,
	uint16_t, int);

static void
model_init(struct model_controller *controller, struct model_request *request)
{
	unsigned index;

	memset(controller, 0, sizeof(*controller));
	memset(request, 0, sizeof(*request));
	controller->running = 1U;
	controller->async_enabled = 1U;
	controller->async_status = 1U;
	for (index = 0; index < UHCI_FRAME_COUNT; index++)
		controller->frame_list[index] = 0x1000U;
	request->state = MODEL_ACTIVE;
	request->active = 1U;
	request->hcd_data = 1U;
	request->schedule_dma = 1U;
	request->bounce_dma = 1U;
}

static int
model_quiesce(const struct model_request *request)
{
	return request->active ? EBUSY : 0;
}

static int
model_stop(struct model_request *request)
{
	/* A checked stop may release only controller-global state.  A request
	 * which failed its own retirement remains quarantined and owned. */
	return request->active ? EBUSY : 0;
}

static int
model_ehci_quiesce(struct model_controller *controller, int request_error,
	int hardware_error)
{
	controller->hardware_stop_count++;
	if (hardware_error != 0)
		return hardware_error;
	controller->running = 0U;
	controller->dma_quiesced = 1U;
	return request_error;
}

static void
model_release(struct model_controller *controller,
	struct model_request *request, int hcd_completion)
{
	controller->completion_inflight++;
	CHECK(controller->lock_held == 0U);
	CHECK(request->active != 0U);
	CHECK(request->hcd_data != 0U);
	CHECK(request->schedule_dma != 0U);
	CHECK(request->bounce_dma != 0U);
	request->active = 0U;
	request->hcd_data = 0U;
	request->schedule_dma = 0U;
	request->bounce_dma = 0U;
	request->free_count++;
	if (hcd_completion) {
		request->completion_count++;
		request->callback_under_lock += controller->lock_held != 0U;
		request->completion_sequence = ++controller->sequence;
	}
	request->state = MODEL_RELEASED;
	CHECK(controller->completion_inflight != 0U);
	controller->completion_inflight--;
}

static int
uhci_begin(struct model_controller *controller,
	struct model_request *request, int cancellation, uint16_t frame)
{
	unsigned index;

	if (request->state != MODEL_ACTIVE)
		return EBUSY;
	controller->lock_held = 1U;
	for (index = 0; index < UHCI_FRAME_COUNT; index++)
		controller->frame_list[index] = 1U;
	request->unlinked = 1U;
	request->unlink_frame = frame;
	request->state = cancellation ? MODEL_WAIT_CANCEL :
	    MODEL_WAIT_COMPLETE;
	controller->lock_held = 0U;
	return 0;
}

static int
uhci_progress(struct model_controller *controller,
	struct model_request *request, uint16_t frame, int timeout)
{
	uint16_t command = controller->running ? MODEL_UHCI_CMD_RUN : 0U;
	uint16_t status = 0U;
	int observation;

	if (request->state != MODEL_WAIT_COMPLETE &&
	    request->state != MODEL_WAIT_CANCEL)
		return EALREADY;
	if (controller->halted)
		status |= MODEL_UHCI_STS_HALTED;
	if (controller->host_system_error)
		status |= MODEL_UHCI_STS_HOST_SYSTEM_ERROR;
	observation = model_uhci_frame_advance(request->unlink_frame, frame,
	    status, command, controller->retirement_stopping);
	if (observation != 0 && observation != EAGAIN)
		timeout = 1;
	if (timeout) {
		request->state = MODEL_FAILED;
		controller->quarantined = 1U;
		controller->quiescing = 1U;
		return EBUSY;
	}
	if (observation == EAGAIN)
		return EAGAIN;
	if (request->state == MODEL_WAIT_CANCEL) {
		request->state = MODEL_RETIRED_CANCEL;
		return 0;
	}
	request->state = MODEL_COMPLETING;
	model_release(controller, request, 1);
	return 0;
}

static int
uhci_cancel_finish(struct model_controller *controller,
	struct model_request *request)
{
	if (request->state != MODEL_RETIRED_CANCEL)
		return EBUSY;
	model_release(controller, request, 0);
	return 0;
}

static int
model_uhci_hardware_stop(struct model_controller *controller, int error)
{
	controller->hardware_stop_count++;
	controller->halt_attempt_count++;
	controller->master_disable_count++;
	controller->irq_drain_count++;
	controller->hardware_stop_sequence = ++controller->sequence;
	if (error != 0)
		return error;
	controller->running = 0U;
	controller->halted = 1U;
	controller->dma_quiesced = 1U;
	return 0;
}

static int
model_uhci_quiesce_drain(struct model_controller *controller,
	struct model_request *requests, size_t request_count, uint16_t frame,
	int retirement_timeout, int root_join_error, int worker_join_error,
	int hardware_error)
{
	size_t index;
	int request_error = 0;
	int root_error = 0;
	int worker_error = 0;
	int stop_error = 0;
	int requests_active = controller->completion_inflight != 0U;
	int wake = 0;

	controller->quiescing = 1U;
	for (index = 0; index < request_count; index++) {
		struct model_request *request = &requests[index];
		int error;

		if (request->state == MODEL_ACTIVE) {
			request->disconnect_count++;
			error = uhci_begin(controller, request, 0, frame);
			if (error == 0)
				wake = 1;
			else if (request_error == 0)
				request_error = error;
		} else if (request->state == MODEL_FAILED && request_error == 0)
			request_error = EBUSY;
	}
	if (wake)
		controller->retirement_wakeups++;
	for (index = 0; index < request_count; index++) {
		struct model_request *request = &requests[index];
		int error;

		if (request->state != MODEL_WAIT_COMPLETE)
			continue;
		error = uhci_progress(controller, request,
		    (uint16_t)((frame + 1U) & MODEL_UHCI_FRNUM_MASK),
		    retirement_timeout || controller->quarantined);
		if (error != 0 && retirement_timeout)
			error = ETIMEDOUT;
		if (error == EAGAIN)
			error = EBUSY;
		if (error != 0 && request_error == 0)
			request_error = error;
	}
	for (index = 0; index < request_count; index++)
		requests_active |= requests[index].active != 0U;
	if (request_error == 0 && requests_active)
		request_error = EBUSY;
	if (!controller->root_joined) {
		controller->root_stop_count++;
		controller->root_stop_sequence = ++controller->sequence;
		root_error = root_join_error;
		if (root_error == 0) {
			controller->root_joined = 1U;
			controller->root_join_count++;
			controller->root_join_sequence = ++controller->sequence;
		}
	}
	if (request_error == 0 && !controller->worker_joined) {
		controller->worker_stop_count++;
		controller->worker_stop_sequence = ++controller->sequence;
		worker_error = worker_join_error;
		if (worker_error == 0) {
			controller->worker_joined = 1U;
			controller->worker_join_count++;
			controller->worker_join_sequence = ++controller->sequence;
		}
	}
	if (!controller->dma_quiesced)
		stop_error = model_uhci_hardware_stop(controller, hardware_error);
	if (request_error != 0)
		return request_error;
	if (root_error != 0)
		return root_error;
	if (worker_error != 0)
		return worker_error;
	return stop_error;
}

static int
ehci_begin(struct model_controller *controller,
	struct model_request *request, int cancellation)
{
	(void)controller;
	if (request->state != MODEL_ACTIVE)
		return EBUSY;
	request->state = cancellation ? MODEL_DEACTIVATING_CANCEL :
	    MODEL_DEACTIVATING_COMPLETE;
	return 0;
}

static int
ehci_unlink(struct model_controller *controller,
	struct model_request *request, int qh_active)
{
	int cancellation;

	if (request->state != MODEL_DEACTIVATING_COMPLETE &&
	    request->state != MODEL_DEACTIVATING_CANCEL)
		return EALREADY;
	if (qh_active)
		return EAGAIN;
	if (!controller->running || controller->halted ||
	    controller->host_system_error || !controller->async_enabled ||
	    !controller->async_status) {
		request->state = MODEL_FAILED;
		controller->quarantined = 1U;
		controller->quiescing = 1U;
		return EBUSY;
	}
	if (controller->iaad)
		return EAGAIN;
	/* A pre-existing IAA is acknowledged and read back before this request's
	 * generation is allowed to ring the doorbell. */
	if (controller->iaa) {
		controller->iaa = 0U;
		request->stale_ack_count++;
	}
	CHECK(controller->iaa == 0U);
	cancellation = request->state == MODEL_DEACTIVATING_CANCEL;
	request->unlinked = 1U;
	request->generation = ++controller->generation;
	controller->iaad = 1U;
	request->state = cancellation ? MODEL_WAIT_CANCEL :
	    MODEL_WAIT_COMPLETE;
	return 0;
}

static int
ehci_advance(struct model_controller *controller,
	struct model_request *request, int timeout)
{
	if (request->state != MODEL_WAIT_COMPLETE &&
	    request->state != MODEL_WAIT_CANCEL)
		return EALREADY;
	if (!controller->running || controller->halted ||
	    controller->host_system_error || timeout) {
		request->state = MODEL_FAILED;
		controller->quarantined = 1U;
		controller->quiescing = 1U;
		return EBUSY;
	}
	if (!controller->iaa || controller->iaad)
		return EAGAIN;
	controller->iaa = 0U;
	request->matching_ack_count++;
	if (request->state == MODEL_WAIT_CANCEL) {
		request->state = MODEL_RETIRED_CANCEL;
		return 0;
	}
	request->state = MODEL_COMPLETING;
	model_release(controller, request, 1);
	return 0;
}

static void
ehci_fatalize(struct model_controller *controller,
	struct model_request *request)
{
	controller->quarantined = 1U;
	controller->quiescing = 1U;
	if (request->state == MODEL_FAILED ||
	    request->state == MODEL_COMPLETING ||
	    request->state == MODEL_RETIRED_CANCEL)
		return;
	request->state = MODEL_FAILED;
}

static int
ehci_cancel_finish(struct model_controller *controller,
	struct model_request *request)
{
	if (request->state != MODEL_RETIRED_CANCEL)
		return EBUSY;
	model_release(controller, request, 0);
	return 0;
}

/* A completion callback executes on the retirement worker.  If that callback
 * enqueues and synchronously dequeues a replacement request, deferring the
 * replacement to the same worker would make the callback wait on itself. */
static enum model_dispatch
model_dispatch_retirement(struct model_controller *controller)
{
	if (controller->worker_context) {
		controller->inline_progress_count++;
		return MODEL_DISPATCH_INLINE;
	}
	controller->retirement_pending = 1U;
	controller->retirement_wakeups++;
	return MODEL_DISPATCH_DEFERRED;
}

static int
model_uhci_frame_advance(uint16_t unlink_frame, uint16_t frame,
	uint16_t status, uint16_t command, int stopping)
{
	/* Validate raw register values and controller health before accepting a
	 * changed frame number as the retirement boundary. */
	if (stopping || unlink_frame == UINT16_MAX || frame == UINT16_MAX ||
	    status == UINT16_MAX || command == UINT16_MAX ||
	    (unlink_frame & (uint16_t)~MODEL_UHCI_FRNUM_MASK) != 0 ||
	    (frame & (uint16_t)~MODEL_UHCI_FRNUM_MASK) != 0 ||
	    (command & MODEL_UHCI_CMD_RUN) == 0 ||
	    (status & (MODEL_UHCI_STS_HOST_SYSTEM_ERROR |
	    MODEL_UHCI_STS_PROCESS_ERROR | MODEL_UHCI_STS_HALTED)) != 0)
		return EIO;
	return frame != unlink_frame ? 0 : EAGAIN;
}

static unsigned
model_uhci_commit_toggle(unsigned initial, const uint32_t *statuses,
	const uint32_t *tokens, unsigned count)
{
	unsigned index;
	unsigned next = initial;

	for (index = 0; index < count; index++) {
		if ((statuses[index] &
		    (MODEL_UHCI_TD_ACTIVE | MODEL_UHCI_TD_ERRORS)) != 0)
			break;
		next = ((tokens[index] & MODEL_UHCI_TOKEN_TOGGLE) != 0) ^ 1U;
	}
	return next;
}

static unsigned
model_ehci_commit_toggle(unsigned initial, uint32_t retired_qh_token,
	int control, int retirement_proven)
{
	if (control || !retirement_proven)
		return initial;
	return (retired_qh_token & MODEL_EHCI_QH_TOGGLE) != 0;
}

static int
terminal_scan(const unsigned *tokens, const size_t *actuals, unsigned count,
	size_t capacity, size_t *actual, int *failed)
{
	unsigned index;
	size_t total = 0;

	*failed = 0;
	for (index = 0; index < count; index++) {
		if ((tokens[index] & MODEL_TD_ERROR) != 0) {
			*failed = 1;
			*actual = total;
			return 1;
		}
		if ((tokens[index] & MODEL_TD_ACTIVE) != 0)
			return 0;
		if (actuals[index] > capacity - total)
			return -1;
		total += actuals[index];
	}
	*actual = total;
	return 1;
}

static void *
claim_thread(void *argument)
{
	struct claim_race *race = argument;
	unsigned expected = 0U;

	while (atomic_load_explicit(&race->shared->start,
	    memory_order_acquire) == 0U)
		sched_yield();
	(void)atomic_compare_exchange_strong_explicit(&race->shared->owner,
	    &expected, race->contender, memory_order_acq_rel,
	    memory_order_acquire);
	if (expected == 0U) {
		(void)atomic_fetch_add_explicit(&race->shared->releases, 1U,
		    memory_order_relaxed);
		if (race->contender == 1U)
			(void)atomic_fetch_add_explicit(
			    &race->shared->hcd_completions, 1U,
			    memory_order_relaxed);
		else
			(void)atomic_fetch_add_explicit(
			    &race->shared->core_completions, 1U,
			    memory_order_relaxed);
	}
	return NULL;
}

static void
test_smp_terminal_claim(void)
{
	unsigned iteration;

	for (iteration = 0; iteration < 1000U; iteration++) {
		struct claim_shared shared;
		struct claim_race irq_race;
		struct claim_race dequeue_race;
		pthread_t irq_thread;
		pthread_t dequeue_thread;

		atomic_init(&shared.start, 0U);
		atomic_init(&shared.owner, 0U);
		atomic_init(&shared.releases, 0U);
		atomic_init(&shared.hcd_completions, 0U);
		atomic_init(&shared.core_completions, 0U);
		irq_race.shared = &shared;
		irq_race.contender = 1U;
		dequeue_race.shared = &shared;
		dequeue_race.contender = 2U;
		CHECK(pthread_create(&irq_thread, NULL, claim_thread, &irq_race) == 0);
		CHECK(pthread_create(&dequeue_thread, NULL, claim_thread,
		    &dequeue_race) == 0);
		atomic_store_explicit(&shared.start, 1U, memory_order_release);
		CHECK(pthread_join(irq_thread, NULL) == 0);
		CHECK(pthread_join(dequeue_thread, NULL) == 0);
		CHECK(atomic_load_explicit(&shared.owner,
		    memory_order_acquire) == 1U ||
		    atomic_load_explicit(&shared.owner,
		    memory_order_acquire) == 2U);
		CHECK(atomic_load_explicit(&shared.releases,
		    memory_order_acquire) == 1U);
		CHECK(atomic_load_explicit(&shared.hcd_completions,
		    memory_order_acquire) +
		    atomic_load_explicit(&shared.core_completions,
		    memory_order_acquire) == 1U);
	}
}

static void
test_uhci_normal_and_rollover(void)
{
	struct model_controller controller;
	struct model_request request;
	unsigned index;

	model_init(&controller, &request);
	CHECK(uhci_begin(&controller, &request, 0, 0x7ffU) == 0);
	CHECK(request.unlinked == 1U);
	for (index = 0; index < UHCI_FRAME_COUNT; index++)
		CHECK(controller.frame_list[index] == 1U);
	CHECK(uhci_progress(&controller, &request, 0x7ffU, 0) == EAGAIN);
	CHECK(request.free_count == 0U);
	CHECK(uhci_progress(&controller, &request, 0U, 0) == 0);
	CHECK(request.free_count == 1U);
	CHECK(request.completion_count == 1U);
	CHECK(request.callback_under_lock == 0U);
	CHECK(uhci_progress(&controller, &request, 1U, 0) == EALREADY);
	CHECK(request.free_count == 1U);
}

static void
test_uhci_terminal_races(void)
{
	struct model_controller controller;
	struct model_request request;

	model_init(&controller, &request);
	CHECK(uhci_begin(&controller, &request, 1, 41U) == 0);
	CHECK(uhci_begin(&controller, &request, 0, 41U) == EBUSY);
	CHECK(model_quiesce(&request) == EBUSY);
	CHECK(uhci_progress(&controller, &request, 42U, 0) == 0);
	CHECK(request.state == MODEL_RETIRED_CANCEL);
	CHECK(request.free_count == 0U);
	CHECK(uhci_cancel_finish(&controller, &request) == 0);
	CHECK(request.free_count == 1U);
	CHECK(request.completion_count == 0U);
	CHECK(model_quiesce(&request) == 0);

	model_init(&controller, &request);
	CHECK(uhci_begin(&controller, &request, 0, 51U) == 0);
	CHECK(uhci_begin(&controller, &request, 1, 51U) == EBUSY);
	CHECK(uhci_progress(&controller, &request, 52U, 0) == 0);
	CHECK(request.completion_count == 1U);
}

static void
test_uhci_failure_retains(void)
{
	struct model_controller controller;
	struct model_request request;

	model_init(&controller, &request);
	CHECK(uhci_begin(&controller, &request, 1, 9U) == 0);
	CHECK(uhci_progress(&controller, &request, 9U, 1) == EBUSY);
	CHECK(request.state == MODEL_FAILED);
	CHECK(request.active == 1U && request.hcd_data == 1U);
	CHECK(request.schedule_dma == 1U && request.bounce_dma == 1U);
	CHECK(request.free_count == 0U && request.completion_count == 0U);
	CHECK(controller.quarantined == 1U && controller.quiescing == 1U);
	CHECK(model_quiesce(&request) == EBUSY);
	CHECK(model_stop(&request) == EBUSY);
	CHECK(uhci_progress(&controller, &request, 10U, 0) == EALREADY);
	CHECK(request.free_count == 0U);
}

static void
test_uhci_quiesce_drain_and_fail_safe_stop(void)
{
	struct model_controller controller;
	struct model_request request;
	struct model_request requests[3];
	unsigned index;

	/* Clean teardown claims every active request as a disconnect, publishes each
	 * sole completion, joins the root and retirement workers, and only then
	 * crosses the controller-global hardware boundary.  A retry cannot publish
	 * twice. */
	model_init(&controller, &requests[0]);
	requests[1] = requests[0];
	requests[2] = requests[0];
	CHECK(model_uhci_quiesce_drain(&controller, requests, 3U, 300U, 0, 0, 0,
	    0) == 0);
	CHECK(controller.quiescing == 1U);
	for (index = 0; index < 3U; index++) {
		CHECK(requests[index].disconnect_count == 1U);
		CHECK(requests[index].state == MODEL_RELEASED &&
		    requests[index].active == 0U);
		CHECK(requests[index].free_count == 1U &&
		    requests[index].completion_count == 1U);
	}
	CHECK(controller.retirement_wakeups == 1U);
	CHECK(controller.root_stop_count == 1U &&
	    controller.root_join_count == 1U && controller.root_joined == 1U);
	CHECK(controller.worker_stop_count == 1U &&
	    controller.worker_join_count == 1U && controller.worker_joined == 1U);
	for (index = 0; index < 3U; index++)
		CHECK(requests[index].completion_sequence <
		    controller.root_stop_sequence);
	CHECK(controller.root_stop_sequence < controller.root_join_sequence);
	CHECK(controller.root_join_sequence < controller.worker_stop_sequence);
	CHECK(controller.worker_stop_sequence < controller.worker_join_sequence);
	CHECK(controller.worker_join_sequence < controller.hardware_stop_sequence);
	CHECK(controller.hardware_stop_count == 1U &&
	    controller.halt_attempt_count == 1U &&
	    controller.master_disable_count == 1U &&
	    controller.irq_drain_count == 1U);
	CHECK(controller.running == 0U && controller.halted == 1U &&
	    controller.dma_quiesced == 1U);
	CHECK(model_uhci_quiesce_drain(&controller, requests, 3U, 301U, 0, 0, 0,
	    0) == 0);
	for (index = 0; index < 3U; index++)
		CHECK(requests[index].disconnect_count == 1U &&
		    requests[index].free_count == 1U &&
		    requests[index].completion_count == 1U);
	CHECK(controller.root_stop_count == 1U &&
	    controller.worker_stop_count == 1U &&
	    controller.hardware_stop_count == 1U);

	/* A request-local timeout retains every request owner.  The same quiesce
	 * invocation must nevertheless attempt halt, BME-off, and IRQ drain, and a
	 * successful global proof does not authorize releasing the failed graph. */
	model_init(&controller, &requests[0]);
	requests[1] = requests[0];
	requests[2] = requests[0];
	CHECK(model_uhci_quiesce_drain(&controller, requests, 3U, 400U, 1, 0, 0,
	    0) == ETIMEDOUT);
	for (index = 0; index < 3U; index++) {
		CHECK(requests[index].state == MODEL_FAILED &&
		    requests[index].active == 1U &&
		    requests[index].hcd_data == 1U);
		CHECK(requests[index].schedule_dma == 1U &&
		    requests[index].bounce_dma == 1U);
		CHECK(requests[index].free_count == 0U &&
		    requests[index].completion_count == 0U);
	}
	CHECK(controller.worker_stop_count == 0U &&
	    controller.worker_join_count == 0U);
	CHECK(controller.root_stop_count == 1U &&
	    controller.root_join_count == 1U);
	CHECK(controller.hardware_stop_count == 1U &&
	    controller.halt_attempt_count == 1U &&
	    controller.master_disable_count == 1U &&
	    controller.irq_drain_count == 1U);
	CHECK(controller.dma_quiesced == 1U);

	/* A failed worker join also cannot bypass the hardware fail-safe.  Once an
	 * external retry performs the unique join, neither request completion nor
	 * the already-proved hardware stop is repeated. */
	model_init(&controller, &request);
	CHECK(model_uhci_quiesce_drain(&controller, &request, 1U, 500U, 0, 0,
	    EBUSY, 0) == EBUSY);
	CHECK(request.free_count == 1U && request.completion_count == 1U);
	CHECK(controller.worker_stop_count == 1U &&
	    controller.worker_join_count == 0U && !controller.worker_joined);
	CHECK(controller.worker_stop_sequence < controller.hardware_stop_sequence);
	CHECK(controller.hardware_stop_count == 1U &&
	    controller.dma_quiesced == 1U);
	CHECK(model_uhci_quiesce_drain(&controller, &request, 1U, 501U, 0, 0, 0,
	    0) == 0);
	CHECK(request.disconnect_count == 1U && request.free_count == 1U &&
	    request.completion_count == 1U);
	CHECK(controller.worker_stop_count == 2U &&
	    controller.worker_join_count == 1U && controller.worker_joined);
	CHECK(controller.hardware_stop_count == 1U);

	/* A worker preempted after active-set removal still owns completion.  The
	 * explicit in-flight count prevents a false clean drain and retirement join;
	 * the bounded failure nevertheless reaches the global stop. */
	model_init(&controller, &request);
	request.active = 0U;
	request.hcd_data = 0U;
	request.state = MODEL_COMPLETING;
	controller.completion_inflight = 1U;
	CHECK(model_uhci_quiesce_drain(&controller, &request, 0U, 525U, 0, 0, 0,
	    0) == EBUSY);
	CHECK(controller.root_joined && !controller.worker_joined);
	CHECK(controller.worker_stop_count == 0U &&
	    controller.hardware_stop_count == 1U && controller.dma_quiesced);
	CHECK(request.schedule_dma && request.bounce_dma &&
	    request.free_count == 0U && request.completion_count == 0U);
	request.schedule_dma = 0U;
	request.bounce_dma = 0U;
	request.free_count = 1U;
	request.completion_count = 1U;
	request.state = MODEL_RELEASED;
	controller.completion_inflight = 0U;
	CHECK(model_uhci_quiesce_drain(&controller, &request, 0U, 526U, 0, 0, 0,
	    0) == 0);
	CHECK(controller.worker_joined && controller.worker_join_count == 1U);
	CHECK(controller.hardware_stop_count == 1U);

	/* A failed root-worker join is also retained, while the independent
	 * retirement join and hardware stop still execute in their frozen order. */
	model_init(&controller, &request);
	CHECK(model_uhci_quiesce_drain(&controller, &request, 1U, 550U, 0,
	    EBUSY, 0, 0) == EBUSY);
	CHECK(request.free_count == 1U && request.completion_count == 1U);
	CHECK(controller.root_stop_count == 1U &&
	    controller.root_join_count == 0U && !controller.root_joined);
	CHECK(controller.root_stop_sequence < controller.worker_stop_sequence);
	CHECK(controller.worker_join_sequence < controller.hardware_stop_sequence);
	CHECK(controller.worker_joined && controller.dma_quiesced);
	CHECK(model_uhci_quiesce_drain(&controller, &request, 1U, 551U, 0, 0, 0,
	    0) == 0);
	CHECK(controller.root_stop_count == 2U &&
	    controller.root_join_count == 1U && controller.root_joined);
	CHECK(controller.worker_stop_count == 1U &&
	    controller.worker_join_count == 1U);
	CHECK(controller.hardware_stop_count == 1U);

	/* If both boundaries fail, the first request-local error remains the return
	 * value, every global stop leg is still attempted, and no DMA is freed. */
	model_init(&controller, &request);
	CHECK(model_uhci_quiesce_drain(&controller, &request, 1U, 600U, 1, 0, 0,
	    EIO) == ETIMEDOUT);
	CHECK(controller.hardware_stop_count == 1U &&
	    controller.halt_attempt_count == 1U &&
	    controller.master_disable_count == 1U &&
	    controller.irq_drain_count == 1U);
	CHECK(controller.dma_quiesced == 0U);
	CHECK(request.active && request.hcd_data && request.schedule_dma &&
	    request.bounce_dma);
	CHECK(request.free_count == 0U && request.completion_count == 0U);
}

static void
test_worker_callback_reentrant_enqueue_dequeue(void)
{
	struct model_controller controller;
	struct model_request request;

	/* UHCI: this replacement request represents enqueue from a completion
	 * callback, followed by a synchronous dequeue on the same worker. */
	model_init(&controller, &request);
	controller.worker_context = 1U;
	CHECK(uhci_begin(&controller, &request, 1, 200U) == 0);
	CHECK(model_dispatch_retirement(&controller) == MODEL_DISPATCH_INLINE);
	CHECK(uhci_progress(&controller, &request, 201U, 0) == 0);
	CHECK(request.state == MODEL_RETIRED_CANCEL);
	CHECK(uhci_cancel_finish(&controller, &request) == 0);
	CHECK(controller.inline_progress_count == 1U);
	CHECK(controller.retirement_pending == 0U);
	CHECK(controller.retirement_wakeups == 0U);

	/* EHCI uses the same rule and the same bounded progress state machine;
	 * IAA below is the simulated matching acknowledgement for the replacement. */
	model_init(&controller, &request);
	controller.worker_context = 1U;
	CHECK(ehci_begin(&controller, &request, 1) == 0);
	CHECK(model_dispatch_retirement(&controller) == MODEL_DISPATCH_INLINE);
	CHECK(ehci_unlink(&controller, &request, 0) == 0);
	controller.iaad = 0U;
	controller.iaa = 1U;
	CHECK(ehci_advance(&controller, &request, 0) == 0);
	CHECK(request.state == MODEL_RETIRED_CANCEL);
	CHECK(ehci_cancel_finish(&controller, &request) == 0);
	CHECK(controller.inline_progress_count == 1U);
	CHECK(controller.retirement_pending == 0U);
	CHECK(controller.retirement_wakeups == 0U);

	/* An external caller still takes the ordinary deferred-worker path. */
	model_init(&controller, &request);
	CHECK(model_dispatch_retirement(&controller) == MODEL_DISPATCH_DEFERRED);
	CHECK(controller.inline_progress_count == 0U);
	CHECK(controller.retirement_pending == 1U);
	CHECK(controller.retirement_wakeups == 1U);
}

static void
test_uhci_frame_health_precedes_change(void)
{
	const uint16_t healthy_status = 0U;
	const uint16_t running = MODEL_UHCI_CMD_RUN;

	CHECK(model_uhci_frame_advance(100U, 100U, healthy_status, running,
	    0) == EAGAIN);
	CHECK(model_uhci_frame_advance(100U, 101U, healthy_status, running,
	    0) == 0);
	/* A changed low eleven-bit value is not proof when any raw register or
	 * controller-health observation is invalid. */
	CHECK(model_uhci_frame_advance(100U, UINT16_MAX, healthy_status,
	    running, 0) == EIO);
	CHECK(model_uhci_frame_advance(UINT16_MAX, 101U, healthy_status,
	    running, 0) == EIO);
	CHECK(model_uhci_frame_advance(100U, 0x0801U, healthy_status,
	    running, 0) == EIO);
	CHECK(model_uhci_frame_advance(0x0800U, 101U, healthy_status,
	    running, 0) == EIO);
	CHECK(model_uhci_frame_advance(100U, 101U, UINT16_MAX, running,
	    0) == EIO);
	CHECK(model_uhci_frame_advance(100U, 101U, healthy_status,
	    UINT16_MAX, 0) == EIO);
	CHECK(model_uhci_frame_advance(100U, 101U, healthy_status, 0U,
	    0) == EIO);
	CHECK(model_uhci_frame_advance(100U, 101U,
	    MODEL_UHCI_STS_HOST_SYSTEM_ERROR, running, 0) == EIO);
	CHECK(model_uhci_frame_advance(100U, 101U,
	    MODEL_UHCI_STS_PROCESS_ERROR, running, 0) == EIO);
	CHECK(model_uhci_frame_advance(100U, 101U, MODEL_UHCI_STS_HALTED,
	    running, 0) == EIO);
	CHECK(model_uhci_frame_advance(100U, 101U, healthy_status, running,
	    1) == EIO);
}

static void
test_non_control_toggle_progress(void)
{
	uint32_t statuses[3] = {0U, 0U, 0U};
	uint32_t tokens[3] = {0U, MODEL_UHCI_TOKEN_TOGGLE, 0U};

	/* Normal: all successful TDs advance the endpoint to the value after the
	 * final completed transaction. */
	CHECK(model_uhci_commit_toggle(0U, statuses, tokens, 3U) == 1U);
	/* Partial: an active TD and everything after it do not advance toggle. */
	statuses[1] = MODEL_UHCI_TD_ACTIVE;
	CHECK(model_uhci_commit_toggle(0U, statuses, tokens, 3U) == 1U);
	/* Error: preserve progress made by successful TDs before the failing TD. */
	statuses[1] = MODEL_UHCI_TD_ERRORS;
	CHECK(model_uhci_commit_toggle(0U, statuses, tokens, 3U) == 1U);
	/* Cancellation after two successful TDs preserves both transactions. */
	statuses[1] = 0U;
	statuses[2] = MODEL_UHCI_TD_ACTIVE;
	CHECK(model_uhci_commit_toggle(1U, statuses, tokens, 3U) == 0U);
	/* No successful TD leaves the original endpoint toggle untouched. */
	statuses[0] = MODEL_UHCI_TD_ERRORS;
	CHECK(model_uhci_commit_toggle(1U, statuses, tokens, 3U) == 1U);

	/* EHCI publishes the next non-control toggle in the retired QH overlay.
	 * Exercise normal, partial, error, and cancellation outcomes after IAA. */
	CHECK(model_ehci_commit_toggle(0U, MODEL_EHCI_QH_TOGGLE, 0, 1) == 1U);
	CHECK(model_ehci_commit_toggle(1U, 0U, 0, 1) == 0U);
	CHECK(model_ehci_commit_toggle(0U, MODEL_EHCI_QH_TOGGLE, 0, 1) == 1U);
	CHECK(model_ehci_commit_toggle(1U, 0U, 0, 1) == 0U);
	CHECK(model_ehci_commit_toggle(1U, 0U, 0, 0) == 1U);
	CHECK(model_ehci_commit_toggle(1U, 0U, 1, 1) == 1U);
}

static void
test_ehci_stale_and_matching_iaa(void)
{
	struct model_controller controller;
	struct model_request request;

	model_init(&controller, &request);
	controller.iaa = 1U;
	CHECK(ehci_begin(&controller, &request, 0) == 0);
	CHECK(ehci_unlink(&controller, &request, 0) == 0);
	CHECK(request.stale_ack_count == 1U);
	CHECK(request.generation == 1U);
	CHECK(request.unlinked == 1U);
	CHECK(request.free_count == 0U);
	/* IAA cannot match while the controller still owns IAAD. */
	controller.iaa = 1U;
	CHECK(ehci_advance(&controller, &request, 0) == EAGAIN);
	CHECK(request.free_count == 0U);
	controller.iaad = 0U;
	CHECK(ehci_advance(&controller, &request, 0) == 0);
	CHECK(request.matching_ack_count == 1U);
	CHECK(request.free_count == 1U && request.completion_count == 1U);
	CHECK(request.callback_under_lock == 0U);
	controller.iaa = 1U;
	CHECK(ehci_advance(&controller, &request, 0) == EALREADY);
	CHECK(request.free_count == 1U && request.completion_count == 1U);
}

static void
test_ehci_cancel_race_and_quiesce(void)
{
	struct model_controller controller;
	struct model_request request;

	model_init(&controller, &request);
	CHECK(ehci_begin(&controller, &request, 1) == 0);
	CHECK(ehci_begin(&controller, &request, 0) == EBUSY);
	CHECK(ehci_unlink(&controller, &request, 1) == EAGAIN);
	CHECK(request.unlinked == 0U);
	CHECK(model_quiesce(&request) == EBUSY);
	CHECK(ehci_unlink(&controller, &request, 0) == 0);
	controller.iaad = 0U;
	controller.iaa = 1U;
	CHECK(ehci_advance(&controller, &request, 0) == 0);
	CHECK(request.state == MODEL_RETIRED_CANCEL);
	CHECK(request.free_count == 0U && request.completion_count == 0U);
	CHECK(ehci_cancel_finish(&controller, &request) == 0);
	CHECK(request.free_count == 1U && request.completion_count == 0U);
	CHECK(model_quiesce(&request) == 0);
}

static void
test_ehci_failures_retain(void)
{
	struct model_controller controller;
	struct model_request request;

	model_init(&controller, &request);
	CHECK(ehci_begin(&controller, &request, 0) == 0);
	controller.async_status = 0U;
	CHECK(ehci_unlink(&controller, &request, 0) == EBUSY);
	CHECK(request.state == MODEL_FAILED);
	CHECK(request.active && request.hcd_data && request.schedule_dma &&
	    request.bounce_dma);
	CHECK(request.free_count == 0U && request.completion_count == 0U);
	CHECK(controller.quarantined && controller.quiescing);
	CHECK(model_stop(&request) == EBUSY);

	model_init(&controller, &request);
	CHECK(ehci_begin(&controller, &request, 1) == 0);
	CHECK(ehci_unlink(&controller, &request, 0) == 0);
	CHECK(ehci_advance(&controller, &request, 1) == EBUSY);
	CHECK(request.state == MODEL_FAILED);
	CHECK(request.active && request.hcd_data && request.schedule_dma &&
	    request.bounce_dma);
	CHECK(request.free_count == 0U && request.completion_count == 0U);
	CHECK(model_stop(&request) == EBUSY);
	controller.iaad = 0U;
	controller.iaa = 1U;
	CHECK(ehci_advance(&controller, &request, 0) == EALREADY);
	CHECK(request.free_count == 0U && request.completion_count == 0U);
}

static void
test_ehci_fatal_preserves_terminal_owner(void)
{
	struct model_controller controller;
	struct model_request request;

	/* The retirement worker has crossed IAA/PSS, popped the request, and owns
	 * completion even though active-list removal still lies ahead. */
	model_init(&controller, &request);
	request.unlinked = 1U;
	request.state = MODEL_COMPLETING;
	ehci_fatalize(&controller, &request);
	CHECK(controller.quarantined && controller.quiescing);
	CHECK(request.state == MODEL_COMPLETING);
	CHECK(request.active && request.hcd_data && request.schedule_dma &&
	    request.bounce_dma);
	CHECK(request.free_count == 0U && request.completion_count == 0U);
	model_release(&controller, &request, 1);
	CHECK(request.free_count == 1U && request.completion_count == 1U);

	/* Checked cancellation has the same unique terminal owner: the dequeue
	 * caller, rather than the completion worker, performs the one release. */
	model_init(&controller, &request);
	request.unlinked = 1U;
	request.state = MODEL_RETIRED_CANCEL;
	ehci_fatalize(&controller, &request);
	CHECK(request.state == MODEL_RETIRED_CANCEL);
	CHECK(request.free_count == 0U && request.completion_count == 0U);
	CHECK(ehci_cancel_finish(&controller, &request) == 0);
	CHECK(request.free_count == 1U && request.completion_count == 0U);

	/* Before the checked hardware boundary, fatalization must still retain the
	 * whole request graph under FAILED ownership. */
	model_init(&controller, &request);
	request.state = MODEL_WAIT_COMPLETE;
	ehci_fatalize(&controller, &request);
	CHECK(request.state == MODEL_FAILED);
	CHECK(request.active && request.hcd_data && request.schedule_dma &&
	    request.bounce_dma);
	CHECK(request.free_count == 0U && request.completion_count == 0U);
	CHECK(model_stop(&request) == EBUSY);
	/* A single production shutdown attempt returns the retirement error only
	 * after synchronously proving the hardware DMA-stop boundary. */
	CHECK(model_ehci_quiesce(&controller, EIO, 0) == EIO);
	CHECK(controller.hardware_stop_count == 1U);
	CHECK(controller.running == 0U && controller.dma_quiesced == 1U);
	CHECK(request.active && request.hcd_data && request.schedule_dma &&
	    request.bounce_dma);
}

static void
test_terminal_scan(void)
{
	unsigned tokens[] = {0U, MODEL_TD_ERROR, MODEL_TD_ACTIVE};
	size_t actuals[] = {8U, 0U, 99U};
	size_t actual = 0;
	int failed = 0;

	CHECK(terminal_scan(tokens, actuals, 3U, 8U, &actual, &failed) == 1);
	CHECK(failed == 1 && actual == 8U);
	tokens[1] = MODEL_TD_ACTIVE;
	CHECK(terminal_scan(tokens, actuals, 3U, 128U, &actual, &failed) == 0);
	tokens[1] = 0U;
	tokens[2] = 0U;
	actuals[2] = 121U;
	CHECK(terminal_scan(tokens, actuals, 3U, 128U, &actual, &failed) == -1);
}

int
main(void)
{
	test_uhci_normal_and_rollover();
	test_uhci_terminal_races();
	test_uhci_failure_retains();
	test_uhci_quiesce_drain_and_fail_safe_stop();
	test_worker_callback_reentrant_enqueue_dequeue();
	test_uhci_frame_health_precedes_change();
	test_non_control_toggle_progress();
	test_ehci_stale_and_matching_iaa();
	test_ehci_cancel_race_and_quiesce();
	test_ehci_failures_retain();
	test_ehci_fatal_preserves_terminal_owner();
	test_terminal_scan();
	test_smp_terminal_claim();
	printf("legacy HCD retirement model: %u checks PASS\n", checks);
	return 0;
}
