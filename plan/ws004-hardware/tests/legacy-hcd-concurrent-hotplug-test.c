/* WS004-p031 deterministic legacy-HCD concurrency/root-worker model. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REQUEST_MAX 16U
#define FRAME_COUNT 1024U
#define PERIODIC_BUCKETS 8U
#define UHCI_FRNUM_MASK 0x07ffU
#define UHCI_ADVANCE_TIMEOUT 100U
#define UHCI_QH_STALL_TICKS 20U
#define UHCI_MODEL_LINK_TERM 0x00000001U
#define UHCI_MODEL_LINK_QH 0x00000002U
#define UHCI_MODEL_LINK_DEPTH 0x00000004U
#define UHCI_MODEL_LINK_RESERVED 0x00000008U
#define UHCI_MODEL_LINK_ADDRESS 0xfffffff0U
#define UHCI_MODEL_TD_SHORT_PACKET 0x20000000U
#define UHCI_MODEL_TD_ACTIVE 0x00800000U
#define UHCI_MODEL_TD_ERRORS 0x007e0000U
#define UHCI_MODEL_PID_IN 0x69U
#define EHCI_PERIODIC_LEVELS 11U
#define EHCI_PERIODIC_NODES ((1U << EHCI_PERIODIC_LEVELS) - 1U)
#define EHCI_MAX_INTERVAL 14U
#define EHCI_PERIODIC_BUDGET_BITS 48000U
#define EHCI_PERIODIC_COMMAND_DISABLED 0x01U
#define EHCI_PERIODIC_STATUS_CLEAR 0x02U
#define EHCI_PERIODIC_UPDATE_COMPLETE 0x04U
#define EHCI_PERIODIC_COMMAND_ENABLED 0x08U
#define EHCI_PERIODIC_BARRIER_COMPLETE (EHCI_PERIODIC_COMMAND_DISABLED | \
	EHCI_PERIODIC_STATUS_CLEAR | EHCI_PERIODIC_UPDATE_COMPLETE | \
	EHCI_PERIODIC_COMMAND_ENABLED)
#define RECLAIM_SAFE_MAX_SIZE 8192U
#define USB_CONTROL_REQUEST_SIZE 8U

enum controller_kind {
	MODEL_UHCI,
	MODEL_EHCI
};

enum transfer_kind {
	MODEL_CONTROL,
	MODEL_INTERRUPT,
	MODEL_BULK
};

enum request_state {
	REQUEST_FREE,
	REQUEST_BUILDING,
	REQUEST_ACTIVE,
	REQUEST_RETIRING,
	REQUEST_RETIRED,
	REQUEST_FAILED
};

enum terminal_source {
	TERMINAL_NONE,
	TERMINAL_IRQ,
	TERMINAL_CANCEL,
	TERMINAL_DISCONNECT
};

struct request {
	unsigned id;
	unsigned device_generation;
	unsigned endpoint;
	unsigned interval;
	unsigned periodic_bucket;
	unsigned periodic_period;
	unsigned periodic_phase;
	unsigned periodic_node;
	unsigned retirement_generation;
	unsigned unlink_frnum;
	uint32_t service_mask;
	enum transfer_kind kind;
	enum request_state state;
	int linked;
	int terminal_claimed;
	int dma_owned;
	int iaa_observed;
	enum terminal_source terminal_source;
};

struct controller {
	enum controller_kind kind;
	struct request requests[REQUEST_MAX];
	unsigned next_id;
	unsigned active_count;
	unsigned builder_count;
	unsigned dma_count;
	unsigned callback_count;
	unsigned ignored_late_count;
	unsigned duplicate_terminal_count;
	unsigned callback_reentry_count;
	unsigned retirement_generation;
	unsigned iaa_owner;
	unsigned iaa_clear_count;
	unsigned iaa_doorbell_count;
	unsigned iaa_ack_count;
	unsigned iaa_ignored_count;
	unsigned iaa_duplicate_count;
	unsigned uhci_frnum;
	unsigned periodic_members[PERIODIC_BUCKETS];
	unsigned periodic_phase_next[EHCI_PERIODIC_LEVELS];
	unsigned async_members;
	unsigned frame_members[FRAME_COUNT];
	unsigned root_generation;
	unsigned root_events;
	unsigned root_scans;
	unsigned root_detaches;
	unsigned root_inserts;
	unsigned root_teardown_busy_once;
	int admission_open;
	int root_worker_running;
	int root_pending;
	int root_retry_pending;
	int topology_locked;
	int attached;
	int callback_resubmit_cancel;
	int iaa_status;
	int iaa_doorbell;
	int iaa_clear_stuck;
};

struct uhci_advance_td_model {
	uint32_t physical, link, status, token;
};

struct uhci_advance_model {
	struct uhci_advance_td_model tds[3];
	uint32_t qh_element;
	uint32_t candidate_element, candidate_status;
	unsigned td_count, candidate, repair_count;
	uint16_t candidate_frame;
	uint64_t candidate_started;
	int active, scheduled, quarantined;
};

struct ehci_budget_model {
	uint16_t used[FRAME_COUNT][8];
};

struct worker_lifecycle {
	unsigned allocations;
	unsigned releases;
	int published;
	int running;
	int pending;
	int stopping;
	int joining;
	int reaped;
};

enum startup_failure_stage {
	START_FAIL_NONE,
	START_FAIL_SCHEDULE_FRAME,
	START_FAIL_SCHEDULE_SKELETON,
	START_FAIL_SCHEDULE_ASYNC,
	START_FAIL_SCHEDULE_RECLAIM_REQUEST,
	START_FAIL_SCHEDULE_RECLAIM_BOUNCE,
	START_FAIL_HCD_REGISTRATION,
	START_FAIL_RETIREMENT_ALLOCATION,
	START_FAIL_RETIREMENT_PUBLICATION,
	START_FAIL_ROOT_ALLOCATION,
	START_FAIL_ROOT_PUBLICATION,
	START_FAIL_HID_ALLOCATION,
	START_FAIL_HID_WORKER_ALLOCATION,
	START_FAIL_HID_WORKER_PUBLICATION
};

struct startup_lifecycle {
	struct worker_lifecycle retirement;
	struct worker_lifecycle root;
	struct worker_lifecycle hid;
	unsigned schedule_allocations;
	unsigned schedule_releases;
	unsigned hid_allocations;
	unsigned hid_releases;
	int hcd_registered;
	int hid_driver_data;
};

struct reclaim_reserve_model {
	enum controller_kind kind;
	size_t schedule_size;
	size_t bounce_size;
	unsigned busy;
	unsigned dma_owned;
	unsigned dynamic_allocations;
	unsigned forbidden_allocation_attempts;
	unsigned allocation_forbidden;
};

static void
reclaim_reserve_init(struct reclaim_reserve_model *reserve,
	enum controller_kind kind)
{
	memset(reserve, 0, sizeof(*reserve));
	reserve->kind = kind;
	reserve->schedule_size = 4096U;
	reserve->bounce_size = RECLAIM_SAFE_MAX_SIZE +
	    USB_CONTROL_REQUEST_SIZE;
}

static int
reclaim_request_acquire(struct reclaim_reserve_model *reserve,
	int reclaim_safe, size_t length)
{
	if (!reclaim_safe) {
		if (reserve->allocation_forbidden) {
			reserve->forbidden_allocation_attempts++;
			return ENOMEM;
		}
		reserve->dynamic_allocations++;
		return 0;
	}
	if (length > RECLAIM_SAFE_MAX_SIZE)
		return EMSGSIZE;
	if (reserve->schedule_size < 4096U ||
	    reserve->bounce_size < length + USB_CONTROL_REQUEST_SIZE)
		return ENOMEM;
	if (reserve->busy)
		return EBUSY;
	reserve->busy = 1U;
	reserve->dma_owned = 1U;
	return 0;
}

static int
reclaim_request_retire(struct reclaim_reserve_model *reserve,
	int retirement_proven)
{
	if (!reserve->busy || !reserve->dma_owned)
		return EINVAL;
	/* An ambiguous controller retirement retains both the request reservation
	 * and its DMA graph.  Only positive retirement evidence makes it reusable. */
	if (!retirement_proven)
		return EIO;
	reserve->dma_owned = 0U;
	reserve->busy = 0U;
	return 0;
}

static void
controller_init(struct controller *controller, enum controller_kind kind)
{
	memset(controller, 0, sizeof(*controller));
	controller->kind = kind;
	controller->next_id = 1U;
	controller->root_generation = 1U;
	controller->admission_open = 1;
	controller->root_worker_running = 1;
}

static uint32_t
uhci_model_token(unsigned length)
{
	uint32_t encoded = length == 0U ? 0x7ffU : length - 1U;

	return UHCI_MODEL_PID_IN | (encoded << 21);
}

static void
uhci_advance_model_init(struct uhci_advance_model *model)
{
	unsigned index;

	memset(model, 0, sizeof(*model));
	model->td_count = 3U;
	model->active = 1;
	model->scheduled = 1;
	for (index = 0U; index < model->td_count; index++) {
		model->tds[index].physical = 0x1010U + index * 16U;
		model->tds[index].status = UHCI_MODEL_TD_ACTIVE;
		model->tds[index].token = uhci_model_token(8U);
		model->tds[index].link = index + 1U < model->td_count ?
		    (0x1010U + (index + 1U) * 16U) | UHCI_MODEL_LINK_DEPTH :
		    UHCI_MODEL_LINK_TERM;
	}
	model->qh_element = model->tds[0].physical;
}

static void
uhci_advance_model_clear(struct uhci_advance_model *model)
{
	model->candidate = 0U;
	model->candidate_element = 0U;
	model->candidate_status = 0U;
	model->candidate_frame = 0U;
	model->candidate_started = 0U;
}

static int
uhci_advance_model_snapshot(struct uhci_advance_model *model,
	unsigned *index_result, uint32_t *element_result,
	uint32_t *status_result, uint32_t *link_result)
{
	uint32_t element = model->qh_element;
	uint32_t link, next_status, physical, status, token;
	unsigned actual, expected, index;

	if ((element & UHCI_MODEL_LINK_TERM) != 0U)
		return 0;
	if ((element & (UHCI_MODEL_LINK_QH | UHCI_MODEL_LINK_RESERVED)) != 0U)
		return EIO;
	physical = element & UHCI_MODEL_LINK_ADDRESS;
	for (index = 0U; index < model->td_count; index++)
		if (model->tds[index].physical == physical)
			break;
	if (index == model->td_count)
		return EIO;
	status = model->tds[index].status;
	if ((status & (UHCI_MODEL_TD_ACTIVE | UHCI_MODEL_TD_ERRORS)) != 0U)
		return 0;
	link = model->tds[index].link;
	if ((link & UHCI_MODEL_LINK_TERM) != 0U)
		return 0;
	if ((link & (UHCI_MODEL_LINK_QH | UHCI_MODEL_LINK_RESERVED)) != 0U ||
	    index + 1U >= model->td_count ||
	    (link & UHCI_MODEL_LINK_ADDRESS) != model->tds[index + 1U].physical)
		return EIO;
	next_status = model->tds[index + 1U].status;
	if ((next_status & UHCI_MODEL_TD_ACTIVE) == 0U)
		return 0;
	if ((status & UHCI_MODEL_TD_SHORT_PACKET) != 0U) {
		token = model->tds[index].token;
		if ((token & 0xffU) != UHCI_MODEL_PID_IN)
			return EIO;
		actual = ((status & 0x7ffU) + 1U) & 0x7ffU;
		expected = (((token >> 21) & 0x7ffU) + 1U) & 0x7ffU;
		if (actual < expected)
			return 0;
		if (actual > expected)
			return EIO;
	}
	*index_result = index;
	*element_result = element;
	*status_result = status;
	*link_result = link;
	return 1;
}

static int
uhci_advance_model_step(struct uhci_advance_model *model,
	unsigned raw_frame, uint64_t tick, int running, int healthy)
{
	uint32_t element, link, status;
	unsigned index;
	int candidate;

	if (!model->active || !model->scheduled) {
		uhci_advance_model_clear(model);
		return 0;
	}
	if (model->quarantined)
		return EIO;
	if (raw_frame > UHCI_FRNUM_MASK || !running || !healthy) {
		model->quarantined = 1;
		return EIO;
	}
	if (model->candidate != 0U) {
		candidate = uhci_advance_model_snapshot(model, &index, &element,
		    &status, &link);
		if (candidate > 1) {
			model->quarantined = 1;
			return candidate;
		}
		if (candidate == 1 && model->candidate == index + 1U &&
		    model->candidate_element == element &&
		    model->candidate_status == status) {
			if (raw_frame != model->candidate_frame &&
			    tick - model->candidate_started >=
			    UHCI_QH_STALL_TICKS) {
				model->qh_element = link;
				model->repair_count++;
				uhci_advance_model_clear(model);
				return 1;
			}
			if (tick - model->candidate_started >=
			    UHCI_ADVANCE_TIMEOUT) {
				model->quarantined = 1;
				return ETIMEDOUT;
			}
			return 0;
		}
		uhci_advance_model_clear(model);
		if (candidate == 0)
			return 0;
	}
	candidate = uhci_advance_model_snapshot(model, &index, &element,
	    &status, &link);
	if (candidate > 1) {
		model->quarantined = 1;
		return candidate;
	}
	if (candidate == 0)
		return 0;
	model->candidate = index + 1U;
	model->candidate_element = element;
	model->candidate_status = status;
	model->candidate_frame = (uint16_t)raw_frame;
	model->candidate_started = tick;
	return 0;
}

static unsigned
interval_bucket(unsigned interval)
{
	unsigned bucket = 0U;

	if (interval == 0U)
		return UINT32_MAX;
	while (interval > 1U && bucket + 1U < PERIODIC_BUCKETS) {
		interval = (interval + 1U) / 2U;
		bucket++;
	}
	return bucket;
}

static unsigned
uhci_periodic_period(unsigned interval)
{
	unsigned period = 1U;
	unsigned normalized = interval > 128U ? 128U : interval;

	assert(normalized != 0U);
	while (period * 2U <= normalized)
		period *= 2U;
	return period;
}

static unsigned
reverse_bits(unsigned value, unsigned bits)
{
	unsigned reversed = 0U;

	while (bits-- != 0U) {
		reversed = (reversed << 1) | (value & 1U);
		value >>= 1;
	}
	return reversed;
}

static int
ehci_periodic_parameters(unsigned interval, unsigned *period,
	uint32_t *service_mask)
{
	unsigned microframes;

	if (interval == 0U)
		return EINVAL;
	if (interval > EHCI_MAX_INTERVAL)
		return ENOTSUP;
	microframes = 1U << (interval - 1U);
	if (microframes == 1U) {
		*period = 1U;
		*service_mask = 0xffU;
	} else if (microframes == 2U) {
		*period = 1U;
		*service_mask = 0x55U;
	} else if (microframes == 4U) {
		*period = 1U;
		*service_mask = 0x11U;
	} else {
		*period = microframes / 8U;
		*service_mask = 0x01U;
	}
	return 0;
}

static int
ehci_frame_reaches_node(unsigned frame, unsigned target)
{
	unsigned node = FRAME_COUNT - 1U +
	    reverse_bits(frame, EHCI_PERIODIC_LEVELS - 1U);

	assert(frame < FRAME_COUNT && target < EHCI_PERIODIC_NODES);
	for (;;) {
		if (node == target)
			return 1;
		if (node == 0U)
			return 0;
		node = (node - 1U) / 2U;
	}
}

static int
ehci_budget_reserve(struct ehci_budget_model *budget, unsigned period,
	unsigned phase, uint8_t mask, unsigned cost)
{
	unsigned frame, microframe;

	if (period == 0U || period > FRAME_COUNT ||
	    (period & (period - 1U)) != 0U || phase >= period || mask == 0U ||
	    cost == 0U || cost > EHCI_PERIODIC_BUDGET_BITS)
		return EINVAL;
	for (frame = phase; frame < FRAME_COUNT; frame += period)
		for (microframe = 0U; microframe < 8U; microframe++)
			if ((mask & (1U << microframe)) != 0U &&
			    budget->used[frame][microframe] >
			    EHCI_PERIODIC_BUDGET_BITS - cost)
				return ENOSPC;
	for (frame = phase; frame < FRAME_COUNT; frame += period)
		for (microframe = 0U; microframe < 8U; microframe++)
			if ((mask & (1U << microframe)) != 0U)
				budget->used[frame][microframe] += (uint16_t)cost;
	return 0;
}

static void
ehci_budget_release(struct ehci_budget_model *budget, unsigned period,
	unsigned phase, uint8_t mask, unsigned cost)
{
	unsigned frame, microframe;

	for (frame = phase; frame < FRAME_COUNT; frame += period)
		for (microframe = 0U; microframe < 8U; microframe++)
			if ((mask & (1U << microframe)) != 0U) {
				assert(budget->used[frame][microframe] >= cost);
				budget->used[frame][microframe] -= (uint16_t)cost;
			}
}

static int
endpoint_owned(const struct controller *controller, unsigned generation,
	unsigned endpoint)
{
	unsigned index;

	for (index = 0; index < REQUEST_MAX; index++) {
		const struct request *request = &controller->requests[index];

		if ((request->state == REQUEST_ACTIVE ||
		    request->state == REQUEST_RETIRING) &&
		    request->device_generation == generation &&
		    request->endpoint == endpoint)
			return 1;
	}
	return 0;
}

static struct request *
request_find(struct controller *controller, unsigned id)
{
	unsigned index;

	for (index = 0; index < REQUEST_MAX; index++)
		if (controller->requests[index].state != REQUEST_FREE &&
		    controller->requests[index].id == id)
			return &controller->requests[index];
	return NULL;
}

static void schedule_unlink(struct controller *, struct request *);

static void
ehci_promote_async_retirement(struct controller *controller)
{
	struct request *oldest = NULL;
	unsigned index;

	assert(controller->kind == MODEL_EHCI);
	if (controller->iaa_owner != 0U || controller->iaa_doorbell)
		return;
	for (index = 0; index < REQUEST_MAX; index++) {
		struct request *request = &controller->requests[index];

		if (request->state != REQUEST_RETIRING ||
		    request->kind == MODEL_INTERRUPT)
			continue;
		if (oldest == NULL || request->retirement_generation <
		    oldest->retirement_generation)
			oldest = request;
	}
	if (oldest == NULL)
		return;
	/* IAA is one untagged W1C status bit.  A bit which predates this
	 * doorbell is cleared and read back before software publishes an owner. */
	if (controller->iaa_status) {
		controller->iaa_clear_count++;
		controller->iaa_status = 0;
		if (controller->iaa_clear_stuck) {
			controller->iaa_status = 1;
			return;
		}
		assert(!controller->iaa_status);
	}
	/* The request remains controller-reachable while queued behind another
	 * owner.  Only its own fresh doorbell turn removes its async link. */
	assert(oldest->linked);
	schedule_unlink(controller, oldest);
	controller->iaa_owner = oldest->id;
	controller->iaa_doorbell = 1;
	controller->iaa_doorbell_count++;
}

static int
ehci_iaa_hardware_signal(struct controller *controller)
{
	assert(controller->kind == MODEL_EHCI);
	if (!controller->iaa_doorbell)
		return 0;
	controller->iaa_doorbell = 0;
	controller->iaa_status = 1;
	return 1;
}

static int
ehci_iaa_irq(struct controller *controller)
{
	struct request *owner;

	assert(controller->kind == MODEL_EHCI);
	if (!controller->iaa_status) {
		controller->iaa_ignored_count++;
		return 0;
	}
	/* IRQ acknowledgement clears and reads back the same untagged bit. */
	controller->iaa_status = 0;
	controller->iaa_ack_count++;
	assert(!controller->iaa_status);
	if (controller->iaa_owner == 0U) {
		controller->iaa_ignored_count++;
		return 0;
	}
	owner = request_find(controller, controller->iaa_owner);
	assert(owner != NULL && owner->state == REQUEST_RETIRING &&
	    owner->kind != MODEL_INTERRUPT);
	if (owner->iaa_observed)
		controller->iaa_duplicate_count++;
	owner->iaa_observed = 1;
	return 1;
}

static int
submit_begin(struct controller *controller, struct request **result,
	unsigned fail_stage)
{
	unsigned index;

	if (!controller->admission_open || !controller->attached)
		return ENODEV;
	controller->builder_count++;
	if (fail_stage == 1U) {
		controller->builder_count--;
		return ENOMEM;
	}
	for (index = 0; index < REQUEST_MAX; index++)
		if (controller->requests[index].state == REQUEST_FREE)
			break;
	if (index == REQUEST_MAX) {
		controller->builder_count--;
		return ENOSPC;
	}
	memset(&controller->requests[index], 0,
	    sizeof(controller->requests[index]));
	controller->requests[index].state = REQUEST_BUILDING;
	controller->requests[index].id = controller->next_id++;
	if (fail_stage == 2U) {
		controller->requests[index].state = REQUEST_FREE;
		controller->builder_count--;
		return ENOMEM;
	}
	controller->requests[index].dma_owned = 1;
	controller->dma_count++;
	*result = &controller->requests[index];
	return 0;
}

static void
schedule_link(struct controller *controller, struct request *request)
{
	unsigned frame, level = 0U;

	request->linked = 1;
	if (request->kind != MODEL_INTERRUPT) {
		controller->async_members++;
		return;
	}
	if (controller->kind == MODEL_UHCI) {
		unsigned period = uhci_periodic_period(request->interval);

		request->periodic_bucket = interval_bucket(period);
		assert(request->periodic_bucket < PERIODIC_BUCKETS);
		controller->periodic_members[request->periodic_bucket]++;
		for (frame = 0; frame < FRAME_COUNT; frame += period)
			controller->frame_members[frame]++;
		return;
	}
	request->periodic_bucket = interval_bucket(request->interval);
	assert(request->periodic_bucket < PERIODIC_BUCKETS);
	controller->periodic_members[request->periodic_bucket]++;
	assert(request->periodic_period != 0U &&
	    request->periodic_period <= FRAME_COUNT && request->service_mask != 0U);
	while ((1U << level) < request->periodic_period)
		level++;
	assert(level < EHCI_PERIODIC_LEVELS);
	request->periodic_phase = controller->periodic_phase_next[level]++ &
	    (request->periodic_period - 1U);
	request->periodic_node = (1U << level) - 1U +
	    reverse_bits(request->periodic_phase, level);
	assert(request->periodic_node < EHCI_PERIODIC_NODES);
	for (frame = 0; frame < FRAME_COUNT; frame++)
		if (ehci_frame_reaches_node(frame, request->periodic_node))
			controller->frame_members[frame]++;
}

static void
schedule_unlink(struct controller *controller, struct request *request)
{
	unsigned frame;

	assert(request->linked);
	request->linked = 0;
	if (request->kind != MODEL_INTERRUPT) {
		assert(controller->async_members != 0U);
		controller->async_members--;
		return;
	}
	assert(request->periodic_bucket < PERIODIC_BUCKETS);
	assert(controller->periodic_members[request->periodic_bucket] != 0U);
	controller->periodic_members[request->periodic_bucket]--;
	if (controller->kind == MODEL_UHCI) {
		unsigned period = uhci_periodic_period(request->interval);

		for (frame = 0; frame < FRAME_COUNT; frame += period) {
			assert(controller->frame_members[frame] != 0U);
			controller->frame_members[frame]--;
		}
		return;
	}
	for (frame = 0; frame < FRAME_COUNT; frame++)
		if (ehci_frame_reaches_node(frame, request->periodic_node)) {
			assert(controller->frame_members[frame] != 0U);
			controller->frame_members[frame]--;
		}
}

static int
submit_commit(struct controller *controller, struct request *request,
	unsigned endpoint, enum transfer_kind kind, unsigned interval,
	unsigned fail_stage)
{
	uint32_t service_mask = 0U;
	unsigned periodic_period = 0U;
	int error = 0;

	assert(controller->builder_count != 0U);
	if (!controller->admission_open || !controller->attached)
		error = ENODEV;
	else if (endpoint_owned(controller, controller->root_generation,
	    endpoint))
		error = EBUSY;
	else if (kind == MODEL_INTERRUPT) {
		if (controller->kind == MODEL_UHCI) {
			if (interval == 0U)
				error = EINVAL;
		} else {
			error = ehci_periodic_parameters(interval, &periodic_period,
			    &service_mask);
		}
	}
	else if (fail_stage == 2U)
		error = EIO;
	if (error != 0) {
		request->state = REQUEST_FREE;
		assert(controller->dma_count != 0U);
		controller->dma_count--;
		controller->builder_count--;
		return error;
	}
	request->device_generation = controller->root_generation;
	request->endpoint = endpoint;
	request->kind = kind;
	request->interval = interval;
	request->periodic_period = periodic_period;
	request->service_mask = service_mask;
	request->state = REQUEST_ACTIVE;
	schedule_link(controller, request);
	controller->active_count++;
	controller->builder_count--;
	return 0;
}

static int
submit(struct controller *controller, unsigned endpoint,
	enum transfer_kind kind, unsigned interval, unsigned *id)
{
	struct request *request;
	int error;

	error = submit_begin(controller, &request, 0U);
	if (error != 0)
		return error;
	error = submit_commit(controller, request, endpoint, kind, interval, 0U);
	if (error == 0)
		*id = request->id;
	return error;
}

static int
terminal_claim_source(struct controller *controller, unsigned id,
	enum terminal_source source)
{
	struct request *request = request_find(controller, id);

	if (request == NULL)
		return 0;
	if (request->state != REQUEST_ACTIVE || request->terminal_claimed) {
		controller->duplicate_terminal_count++;
		return 0;
	}
	assert(source != TERMINAL_NONE);
	request->terminal_claimed = 1;
	request->terminal_source = source;
	request->state = REQUEST_RETIRING;
	request->retirement_generation = ++controller->retirement_generation;
	if (controller->kind != MODEL_EHCI || request->kind == MODEL_INTERRUPT)
		schedule_unlink(controller, request);
	if (controller->kind == MODEL_UHCI) {
		assert((controller->uhci_frnum & ~UHCI_FRNUM_MASK) == 0U);
		request->unlink_frnum = controller->uhci_frnum;
	}
	if (controller->kind == MODEL_EHCI && request->kind != MODEL_INTERRUPT)
		ehci_promote_async_retirement(controller);
	return 1;
}

static int
terminal_claim(struct controller *controller, unsigned id)
{
	return terminal_claim_source(controller, id, TERMINAL_IRQ);
}

static void callback_reentry(struct controller *controller);

static int
retire(struct controller *controller, unsigned id, unsigned observation,
	int hardware_healthy)
{
	struct request *request = request_find(controller, id);

	if (request == NULL || request->state != REQUEST_RETIRING)
		return EBUSY;
	if (!hardware_healthy) {
		request->state = REQUEST_FAILED;
		return EIO;
	}
	if (controller->kind == MODEL_UHCI) {
		if ((observation & ~UHCI_FRNUM_MASK) != 0U)
			return EIO;
		if (observation == request->unlink_frnum)
			return EAGAIN;
	} else if (request->kind == MODEL_INTERRUPT) {
		/* Every checked disable/status/update/re-enable phase is required. */
		if (observation != EHCI_PERIODIC_BARRIER_COMPLETE)
			return EAGAIN;
	} else {
		/* Hardware supplies no generation tag.  Retirement consumes only the
		 * current software owner after its doorbell was acknowledged and the W1C
		 * status bit was observed clear. */
		if (controller->iaa_owner != request->id ||
		    !request->iaa_observed || controller->iaa_doorbell ||
		    controller->iaa_status || request->linked)
			return EAGAIN;
		controller->iaa_owner = 0U;
	}
	request->state = REQUEST_RETIRED;
	assert(controller->active_count != 0U);
	controller->active_count--;
	assert(request->dma_owned && controller->dma_count != 0U);
	request->dma_owned = 0;
	controller->dma_count--;
	controller->callback_count++;
	request->state = REQUEST_FREE;
	if (controller->kind == MODEL_EHCI && controller->iaa_owner == 0U)
		ehci_promote_async_retirement(controller);
	if (controller->callback_resubmit_cancel) {
		controller->callback_resubmit_cancel = 0;
		callback_reentry(controller);
	}
	return 0;
}

static void
callback_reentry(struct controller *controller)
{
	struct request *replacement;
	unsigned id, observation;

	/* A terminal callback is outside the controller schedule lock.  It can
	 * submit a replacement and synchronously cancel that exact new owner without
	 * disturbing an unrelated request or re-entering its own retirement. */
	assert(submit(controller, 0x84U, MODEL_INTERRUPT, 2U, &id) == 0);
	assert(terminal_claim_source(controller, id, TERMINAL_CANCEL) == 1);
	replacement = request_find(controller, id);
	assert(replacement != NULL);
	if (controller->kind == MODEL_UHCI)
		observation = (replacement->unlink_frnum + 1U) & UHCI_FRNUM_MASK;
	else
		observation = EHCI_PERIODIC_BARRIER_COMPLETE;
	controller->callback_reentry_count++;
	assert(retire(controller, id, observation, 1) == 0);
}

static int
complete_request(struct controller *controller, unsigned id)
{
	struct request *request;
	unsigned observation;
	int claimed;

	claimed = terminal_claim(controller, id);
	if (claimed != 1)
		return claimed == EAGAIN ? EAGAIN : ENOENT;
	request = request_find(controller, id);
	assert(request != NULL);
	if (controller->kind == MODEL_UHCI)
		observation = (request->unlink_frnum + 1U) & UHCI_FRNUM_MASK;
	else if (request->kind == MODEL_INTERRUPT)
		observation = EHCI_PERIODIC_BARRIER_COMPLETE;
	else {
		assert(ehci_iaa_hardware_signal(controller) == 1);
		assert(ehci_iaa_irq(controller) == 1);
		observation = 0U;
	}
	return retire(controller, id, observation, 1);
}

static void
root_latch(struct controller *controller)
{
	if (!controller->root_worker_running)
		return;
	controller->root_pending = 1;
	controller->root_events++;
}

static int
root_destroy_generation(struct controller *controller)
{
	unsigned index;

	assert(controller->attached);
	controller->admission_open = 0;
	for (index = 0; index < REQUEST_MAX; index++) {
		struct request *request = &controller->requests[index];

		if (request->state != REQUEST_ACTIVE ||
		    request->device_generation != controller->root_generation)
			continue;
		assert(terminal_claim_source(controller, request->id,
		    TERMINAL_DISCONNECT) == 1);
		assert(complete_request(controller, request->id) == ENOENT);
		/* terminal_claim already happened; use the matching observation. */
		if (controller->kind == MODEL_UHCI)
			assert(retire(controller, request->id,
			    (request->unlink_frnum + 1U) & UHCI_FRNUM_MASK,
			    1) == 0);
			else if (request->kind == MODEL_INTERRUPT)
				assert(retire(controller, request->id,
				    EHCI_PERIODIC_BARRIER_COMPLETE, 1) == 0);
			else {
				assert(controller->iaa_owner == request->id);
				assert(ehci_iaa_hardware_signal(controller) == 1);
				assert(ehci_iaa_irq(controller) == 1);
				assert(retire(controller, request->id, 0U, 1) == 0);
			}
	}
	if (controller->root_teardown_busy_once != 0U) {
		controller->root_teardown_busy_once--;
		controller->root_retry_pending = 1;
		return EBUSY;
	}
	controller->attached = 0;
	controller->root_retry_pending = 0;
	controller->root_detaches++;
	return 0;
}

static int
root_worker_step(struct controller *controller, int now_attached,
	int connection_changed, int port_disabled)
{
	int error;

	if (!controller->root_worker_running)
		return ENODEV;
	if (!controller->root_pending && !controller->root_retry_pending)
		return EAGAIN;
	if (controller->topology_locked)
		return EBUSY;
	controller->root_pending = 0;
	controller->root_scans++;
	/* A connected+C_CONNECTION observation is still a generation boundary:
	 * disconnect and reconnect may both have occurred between two polls. */
	if (controller->attached && (!now_attached || connection_changed ||
	    port_disabled ||
	    controller->root_retry_pending)) {
		error = root_destroy_generation(controller);
		if (error != 0)
			return error;
	}
	if (!controller->attached && now_attached) {
		controller->root_generation++;
		controller->attached = 1;
		controller->admission_open = 1;
		controller->root_inserts++;
	}
	return 0;
}

static void
late_completion(struct controller *controller, unsigned id,
	unsigned old_generation)
{
	struct request *request = request_find(controller, id);

	if (request == NULL || request->device_generation != old_generation ||
	    old_generation != controller->root_generation) {
		controller->ignored_late_count++;
		return;
	}
	assert(!"old-generation completion reached current request");
}

static void
root_worker_stop(struct controller *controller)
{
	controller->root_worker_running = 0;
	controller->root_pending = 0;
}

static int
worker_start(struct worker_lifecycle *worker, unsigned fail_stage)
{
	if (worker->published || worker->joining)
		return EBUSY;
	/* Stage 1 fails allocation; stage 2 fails publication and unwinds it. */
	if (fail_stage == 1U)
		return ENOMEM;
	worker->allocations++;
	if (fail_stage == 2U) {
		worker->releases++;
		return EIO;
	}
	worker->published = 1;
	worker->running = 1;
	worker->stopping = 0;
	worker->reaped = 0;
	return 0;
}

static int
worker_stop(struct worker_lifecycle *worker, int caller_is_worker,
	int fail_join)
{
	if (worker->joining)
		return EBUSY;
	if (!worker->published)
		return 0;
	if (caller_is_worker) {
		/* Self-stop requests exit but leaves publication for an external join. */
		worker->stopping = 1;
		worker->pending = 0;
		worker->running = 0;
		return EDEADLK;
	}
	worker->stopping = 1;
	worker->joining = 1;
	worker->pending = 0;
	worker->running = 0;
	/* A failed join retains the published owner so another caller can retry. */
	if (fail_join) {
		worker->joining = 0;
		return EIO;
	}
	/* Joining suppresses notifiers while the published pointer crosses the
	 * final reap.  Only a successful reap clears that retained owner. */
	worker->reaped = 1;
	worker->published = 0;
	worker->joining = 0;
	worker->releases++;
	return 0;
}

static void
startup_cleanup(struct startup_lifecycle *startup)
{
	if (startup->hid.published)
		assert(worker_stop(&startup->hid, 0, 0) == 0);
	if (startup->hid_driver_data)
		startup->hid_driver_data = 0;
	startup->hid_releases += startup->hid_allocations;
	startup->hid_allocations = 0U;
	if (startup->root.published)
		assert(worker_stop(&startup->root, 0, 0) == 0);
	if (startup->retirement.published)
		assert(worker_stop(&startup->retirement, 0, 0) == 0);
	startup->hcd_registered = 0;
	startup->schedule_releases += startup->schedule_allocations;
	startup->schedule_allocations = 0U;
}

static int
startup_attempt(struct startup_lifecycle *startup,
	enum controller_kind kind,
	enum startup_failure_stage fail_stage)
{
	unsigned schedule_last = START_FAIL_SCHEDULE_RECLAIM_BOUNCE;
	unsigned schedule_stage;
	int error;

	memset(startup, 0, sizeof(*startup));
	for (schedule_stage = START_FAIL_SCHEDULE_FRAME;
	    schedule_stage <= schedule_last; schedule_stage++) {
		if (kind == MODEL_UHCI &&
		    schedule_stage == START_FAIL_SCHEDULE_ASYNC)
			continue;
		if (fail_stage == (enum startup_failure_stage)schedule_stage) {
			error = ENOMEM;
			goto fail;
		}
		startup->schedule_allocations++;
	}
	/* drv_usb_hcd_register() owns start, so a registration failure must unwind
	 * every schedule allocation even though the bus was never published. */
	if (fail_stage == START_FAIL_HCD_REGISTRATION) {
		error = EIO;
		goto fail;
	}
	startup->hcd_registered = 1;
	error = worker_start(&startup->retirement,
	    fail_stage == START_FAIL_RETIREMENT_ALLOCATION ? 1U :
	    fail_stage == START_FAIL_RETIREMENT_PUBLICATION ? 2U : 0U);
	if (error != 0)
		goto fail;
	error = worker_start(&startup->root,
	    fail_stage == START_FAIL_ROOT_ALLOCATION ? 1U :
	    fail_stage == START_FAIL_ROOT_PUBLICATION ? 2U : 0U);
	if (error != 0)
		goto fail;
	/* The checkpoint publishes driver data only after all owned allocations.
	 * Any later HID-worker failure clears that publication before releasing them. */
	if (fail_stage == START_FAIL_HID_ALLOCATION) {
		error = ENOMEM;
		goto fail;
	}
	startup->hid_allocations = 3U;
	startup->hid_driver_data = 1;
	error = worker_start(&startup->hid,
	    fail_stage == START_FAIL_HID_WORKER_ALLOCATION ? 1U :
	    fail_stage == START_FAIL_HID_WORKER_PUBLICATION ? 2U : 0U);
	if (error != 0)
		goto fail;
	return 0;

fail:
	startup_cleanup(startup);
	return error;
}

static void
test_concurrent_progress(enum controller_kind kind)
{
	static const unsigned permutations[][3] = {
		{ 0U, 1U, 2U },
		{ 0U, 2U, 1U },
		{ 1U, 0U, 2U },
		{ 1U, 2U, 0U },
		{ 2U, 0U, 1U },
		{ 2U, 1U, 0U }
	};
	unsigned permutation;

	for (permutation = 0U;
	    permutation < sizeof(permutations) / sizeof(permutations[0]);
	    permutation++) {
		struct controller controller;
		struct request *builder_a, *builder_b;
		unsigned interrupt_id, terminal_ids[3], rejected_id;
		unsigned callbacks, index;

		controller_init(&controller, kind);
		controller.attached = 1;
		assert(submit(&controller, 0x81U, MODEL_INTERRUPT, 8U,
		    &interrupt_id) == 0);
		assert(submit(&controller, 0x81U, MODEL_INTERRUPT, 8U,
		    &rejected_id) == EBUSY);
		assert(controller.active_count == 1U && controller.dma_count == 1U);
		assert(submit_begin(&controller, &builder_a, 0U) == 0);
		assert(submit_begin(&controller, &builder_b, 0U) == 0);
		assert(controller.builder_count == 2U);
		assert(submit_commit(&controller, builder_a, 0x82U, MODEL_BULK,
		    0U, 0U) == 0);
		terminal_ids[1] = builder_a->id;
		assert(submit_commit(&controller, builder_b, 0x02U, MODEL_BULK,
		    0U, 0U) == 0);
		terminal_ids[2] = builder_b->id;
		assert(submit(&controller, 0x00U, MODEL_CONTROL, 0U,
		    &terminal_ids[0]) == 0);
		assert(controller.active_count == 4U);

		/* Every control/bulk terminal order progresses while interrupt-IN
		 * remains persistently pending. */
		callbacks = controller.callback_count;
		for (index = 0U; index < 3U; index++) {
			assert(complete_request(&controller,
			    terminal_ids[permutations[permutation][index]]) == 0);
			assert(request_find(&controller, interrupt_id) != NULL);
			assert(controller.callback_count == callbacks + index + 1U);
		}
		assert(controller.active_count == 1U);
		assert(complete_request(&controller, interrupt_id) == 0);
		assert(controller.active_count == 0U);
		assert(controller.dma_count == 0U);
	}
}

static void
test_cancel_irq_exactly_once(enum controller_kind kind,
	enum terminal_source winner)
{
	struct controller controller;
	struct request *request;
	unsigned primary, unrelated, observation, callbacks;
	enum terminal_source loser;

	controller_init(&controller, kind);
	controller.attached = 1;
	assert(submit(&controller, 0x82U, MODEL_BULK, 0U, &primary) == 0);
	assert(submit(&controller, 0x81U, MODEL_INTERRUPT, 2U,
	    &unrelated) == 0);
	loser = winner == TERMINAL_IRQ ? TERMINAL_CANCEL : TERMINAL_IRQ;
	assert(terminal_claim_source(&controller, primary, winner) == 1);
	assert(terminal_claim_source(&controller, primary, loser) == 0);
	request = request_find(&controller, primary);
	assert(request != NULL && request->terminal_source == winner);
	assert(controller.duplicate_terminal_count == 1U);
	if (kind == MODEL_UHCI)
		observation = (request->unlink_frnum + 1U) & UHCI_FRNUM_MASK;
	else {
		assert(ehci_iaa_hardware_signal(&controller) == 1);
		assert(ehci_iaa_irq(&controller) == 1);
		observation = 0U;
	}
	callbacks = controller.callback_count;
	controller.callback_resubmit_cancel = 1;
	assert(retire(&controller, primary, observation, 1) == 0);
	/* The winner publishes once.  Its callback can submit and synchronously
	 * cancel a replacement without touching the unrelated interrupt owner. */
	assert(controller.callback_count == callbacks + 2U);
	assert(controller.callback_reentry_count == 1U);
	assert(request_find(&controller, unrelated) != NULL);
	assert(terminal_claim_source(&controller, primary, loser) == 0);
	assert(controller.callback_count == callbacks + 2U);
	assert(complete_request(&controller, unrelated) == 0);
	assert(controller.callback_count == callbacks + 3U);
	assert(controller.active_count == 0U && controller.dma_count == 0U);
}

static void
test_request_local_retirement(enum controller_kind kind)
{
	struct controller controller;
	unsigned first, second;
	struct request *request;
	unsigned async_before, periodic_before;

	controller_init(&controller, kind);
	controller.attached = 1;
	assert(submit(&controller, 0x81U, MODEL_INTERRUPT, 4U, &first) == 0);
	assert(submit(&controller, 0x02U, MODEL_BULK, 0U, &second) == 0);
	async_before = controller.async_members;
	periodic_before = controller.periodic_members[interval_bucket(4U)];
	assert(terminal_claim(&controller, first) == 1);
	assert(controller.async_members == async_before);
	assert(controller.periodic_members[interval_bucket(4U)] + 1U ==
	    periodic_before);
	request = request_find(&controller, first);
	assert(request != NULL);
	if (kind == MODEL_UHCI) {
		assert(retire(&controller, first, request->unlink_frnum,
		    1) == EAGAIN);
		assert(retire(&controller, first,
		    (request->unlink_frnum + 1U) & UHCI_FRNUM_MASK, 1) == 0);
	} else {
		assert(retire(&controller, first, 0U, 1) == EAGAIN);
		assert(retire(&controller, first,
		    EHCI_PERIODIC_BARRIER_COMPLETE, 1) == 0);
	}
	assert(request_find(&controller, second) != NULL);
	assert(complete_request(&controller, second) == 0);

	assert(submit(&controller, 0x03U, MODEL_BULK, 0U, &first) == 0);
	assert(terminal_claim(&controller, first) == 1);
	request = request_find(&controller, first);
	assert(request != NULL);
	assert(retire(&controller, first, 0U, 0) == EIO);
	assert(request->state == REQUEST_FAILED && request->dma_owned);
	assert(controller.dma_count == 1U);
}

static void
test_uhci_qh_element_advance_workaround(void)
{
	struct uhci_advance_model model, unrelated;
	uint32_t original;

	/* The first observation only arms the proof.  Neither the same FRNUM nor a
	 * normal fresh-frame writeback window authorizes a write.  Only the same
	 * stopped boundary lasting the established 200-ms threshold copies the
	 * exact immutable TD link, and repeated polling cannot advance it twice. */
	uhci_advance_model_init(&model);
	uhci_advance_model_init(&unrelated);
	model.tds[0].status = 7U;
	original = unrelated.qh_element;
	assert(uhci_advance_model_step(&model, 10U, 20U, 1, 1) == 0);
	assert(model.candidate == 1U && model.qh_element == 0x1010U);
	assert(uhci_advance_model_step(&model, 10U, 21U, 1, 1) == 0);
	assert(model.repair_count == 0U && model.qh_element == 0x1010U);
	assert(uhci_advance_model_step(&model, 11U, 22U, 1, 1) == 0);
	assert(model.repair_count == 0U && model.qh_element == 0x1010U);
	assert(uhci_advance_model_step(&model, 29U,
	    20U + UHCI_QH_STALL_TICKS - 1U, 1, 1) == 0);
	assert(model.repair_count == 0U && model.qh_element == 0x1010U);
	assert(uhci_advance_model_step(&model, 30U,
	    20U + UHCI_QH_STALL_TICKS, 1, 1) == 1);
	assert(model.qh_element == model.tds[0].link);
	assert(model.repair_count == 1U && model.candidate == 0U);
	assert(uhci_advance_model_step(&model, 31U,
	    20U + UHCI_QH_STALL_TICKS + 1U, 1, 1) == 0);
	assert(model.repair_count == 1U && unrelated.qh_element == original);

	/* A normal HC writeback which wins during the observation interval clears
	 * the candidate and is never overwritten by software. */
	uhci_advance_model_init(&model);
	model.tds[0].status = 7U;
	assert(uhci_advance_model_step(&model, 30U, 30U, 1, 1) == 0);
	model.qh_element = model.tds[0].link;
	assert(uhci_advance_model_step(&model, 31U, 31U, 1, 1) == 0);
	assert(model.repair_count == 0U && model.candidate == 0U);

	/* Raw FRNUM inequality includes the 0x7ff -> 0 wrap.  Pointer identity
	 * ignores legal DEPTH while the exact observed element remains stable. */
	uhci_advance_model_init(&model);
	model.qh_element |= UHCI_MODEL_LINK_DEPTH;
	model.tds[0].status = 7U;
	assert(uhci_advance_model_step(&model, UHCI_FRNUM_MASK, 40U,
	    1, 1) == 0);
	assert(uhci_advance_model_step(&model, 0U,
	    40U + UHCI_QH_STALL_TICKS, 1, 1) == 1);
	assert(model.qh_element == model.tds[0].link);

	/* SPD-short is an intentional non-advance.  With SPD clear, the UHCI
	 * contract still advances a successful short IN, while SPD-full is also
	 * eligible for the PIIX repair. */
	uhci_advance_model_init(&model);
	model.tds[0].status = UHCI_MODEL_TD_SHORT_PACKET | 3U;
	assert(uhci_advance_model_step(&model, 50U, 50U, 1, 1) == 0);
	assert(model.candidate == 0U && model.repair_count == 0U);
	uhci_advance_model_init(&model);
	model.tds[0].status = 3U;
	assert(uhci_advance_model_step(&model, 50U, 50U, 1, 1) == 0);
	assert(uhci_advance_model_step(&model, 51U,
	    50U + UHCI_QH_STALL_TICKS, 1, 1) == 1);
	uhci_advance_model_init(&model);
	model.tds[0].status = UHCI_MODEL_TD_SHORT_PACKET | 7U;
	assert(uhci_advance_model_step(&model, 52U, 52U, 1, 1) == 0);
	assert(uhci_advance_model_step(&model, 53U,
	    52U + UHCI_QH_STALL_TICKS, 1, 1) == 1);

	/* Errors and final TDs remain owned by the IRQ terminal path. */
	uhci_advance_model_init(&model);
	model.tds[0].status = 0x00020000U;
	assert(model.tds[0].status != 0U);
	assert(uhci_advance_model_step(&model, 60U, 60U, 1, 1) == 0);
	assert(model.candidate == 0U && model.repair_count == 0U);
	uhci_advance_model_init(&model);
	model.tds[0].status = 7U;
	model.tds[1].status = 7U;
	original = model.qh_element;
	assert(uhci_advance_model_step(&model, 60U, 60U, 1, 1) == 0);
	assert(!model.quarantined && model.candidate == 0U &&
	    model.qh_element == original);
	uhci_advance_model_init(&model);
	model.tds[0].status = 7U;
	model.tds[1].status = 0x00020000U;
	original = model.qh_element;
	assert(uhci_advance_model_step(&model, 60U, 60U, 1, 1) == 0);
	assert(!model.quarantined && model.candidate == 0U &&
	    model.qh_element == original);
	uhci_advance_model_init(&model);
	model.tds[0].status = 7U;
	model.tds[1].status = 7U;
	model.tds[2].status = 7U;
	original = model.qh_element;
	assert(uhci_advance_model_step(&model, 60U, 60U, 1, 1) == 0);
	assert(!model.quarantined && model.candidate == 0U &&
	    model.repair_count == 0U && model.qh_element == original);

	/* QEMU publishes TD status and QH.element with separate DMA writes.  If a
	 * successor finishes after the candidate was armed but before its QH
	 * writeback, clear the stale candidate without quarantining or replaying
	 * either completed transaction. */
	uhci_advance_model_init(&model);
	model.tds[0].status = 7U;
	original = model.qh_element;
	assert(uhci_advance_model_step(&model, 61U, 61U, 1, 1) == 0);
	assert(model.candidate == 1U);
	model.tds[1].status = 7U;
	assert(uhci_advance_model_step(&model, 62U, 62U, 1, 1) == 0);
	assert(!model.quarantined && model.candidate == 0U &&
	    model.repair_count == 0U && model.qh_element == original);
	model.qh_element = model.tds[1].link;
	assert(uhci_advance_model_step(&model, 63U, 63U, 1, 1) == 0);
	assert(!model.quarantined && model.repair_count == 0U);
	uhci_advance_model_init(&model);
	model.qh_element = model.tds[2].physical;
	model.tds[2].status = 7U;
	assert(uhci_advance_model_step(&model, 60U, 60U, 1, 1) == 0);
	assert(model.candidate == 0U && model.repair_count == 0U);

	/* Software unlink/cancel invalidates the observation before a later frame
	 * can write through a stale request pointer. */
	uhci_advance_model_init(&model);
	model.tds[0].status = 7U;
	original = model.qh_element;
	assert(uhci_advance_model_step(&model, 70U, 70U, 1, 1) == 0);
	model.scheduled = 0;
	assert(uhci_advance_model_step(&model, 71U, 71U, 1, 1) == 0);
	assert(model.candidate == 0U && model.qh_element == original);

	/* Invalid controller evidence and a frozen frame fail closed without
	 * fabricating an element pointer or releasing descriptor ownership. */
	uhci_advance_model_init(&model);
	model.tds[0].status = 7U;
	original = model.qh_element;
	assert(uhci_advance_model_step(&model, UINT16_MAX, 80U, 1, 1) == EIO);
	assert(model.quarantined && model.qh_element == original);
	uhci_advance_model_init(&model);
	model.tds[0].status = 7U;
	assert(uhci_advance_model_step(&model, 80U, 80U, 0, 1) == EIO);
	assert(model.quarantined);
	uhci_advance_model_init(&model);
	model.tds[0].status = 7U;
	assert(uhci_advance_model_step(&model, 80U, 80U, 1, 0) == EIO);
	assert(model.quarantined);
	uhci_advance_model_init(&model);
	model.tds[0].status = 7U;
	original = model.qh_element;
	assert(uhci_advance_model_step(&model, 90U, 90U, 1, 1) == 0);
	assert(uhci_advance_model_step(&model, 90U,
	    90U + UHCI_ADVANCE_TIMEOUT, 1, 1) == ETIMEDOUT);
	assert(model.quarantined && model.qh_element == original);

	/* TERM is a normal terminal observation.  QH-typed and foreign element
	 * pointers cannot belong to this request and quarantine without a write. */
	uhci_advance_model_init(&model);
	model.qh_element = UHCI_MODEL_LINK_TERM;
	assert(uhci_advance_model_step(&model, 100U, 100U, 1, 1) == 0);
	assert(!model.quarantined && model.repair_count == 0U);
	uhci_advance_model_init(&model);
	model.qh_element |= UHCI_MODEL_LINK_QH;
	original = model.qh_element;
	assert(uhci_advance_model_step(&model, 100U, 100U, 1, 1) == EIO);
	assert(model.quarantined && model.qh_element == original);
	uhci_advance_model_init(&model);
	model.qh_element = 0x2000U;
	original = model.qh_element;
	assert(uhci_advance_model_step(&model, 100U, 100U, 1, 1) == EIO);
	assert(model.quarantined && model.qh_element == original);
}

static void
test_uhci_periodic_cadence_and_retirement(void)
{
	static const unsigned intervals[] = {
		1U, 2U, 3U, 4U, 8U, 10U, 127U, 128U, 129U, 255U
	};
	static const unsigned periods[] = {
		1U, 2U, 2U, 4U, 8U, 8U, 64U, 128U, 128U, 128U
	};
	struct controller controller;
	struct request *request;
	unsigned ids[sizeof(intervals) / sizeof(intervals[0])];
	unsigned first, second, failed, index, frame, frame_total = 0U;

	controller_init(&controller, MODEL_UHCI);
	controller.attached = 1;
	for (index = 0U; index < sizeof(intervals) / sizeof(intervals[0]);
	    index++) {
		assert(submit(&controller, 0x81U + index, MODEL_INTERRUPT,
		    intervals[index], &ids[index]) == 0);
		request = request_find(&controller, ids[index]);
		assert(request != NULL);
		assert(request->periodic_bucket == interval_bucket(periods[index]));
	}
	assert(controller.frame_members[0] == 10U);
	assert(controller.frame_members[1] == 1U);
	assert(controller.frame_members[2] == 3U);
	assert(controller.frame_members[4] == 4U);
	assert(controller.frame_members[8] == 6U);
	for (frame = 0U; frame < FRAME_COUNT; frame++)
		frame_total += controller.frame_members[frame];
	assert(frame_total == 1024U + 512U + 512U + 256U + 128U + 128U +
	    16U + 8U + 8U + 8U);
	for (index = 0U; index < sizeof(intervals) / sizeof(intervals[0]);
	    index++)
		assert(complete_request(&controller, ids[index]) == 0);
	assert(controller.active_count == 0U && controller.dma_count == 0U);

	/* A frame change through 0x7ff -> 0 is fresh; the captured frame itself
	 * is stale and cannot release DMA. */
	controller.uhci_frnum = UHCI_FRNUM_MASK;
	assert(submit(&controller, 0x02U, MODEL_BULK, 0U, &first) == 0);
	assert(terminal_claim(&controller, first) == 1);
	request = request_find(&controller, first);
	assert(request != NULL && request->unlink_frnum == UHCI_FRNUM_MASK);
	assert(retire(&controller, first, UHCI_FRNUM_MASK, 1) == EAGAIN);
	assert(request->dma_owned && controller.dma_count == 1U);
	assert(retire(&controller, first, 0U, 1) == 0);

	/* Adjacent unlink observations remain request-local. */
	controller.uhci_frnum = 100U;
	assert(submit(&controller, 0x82U, MODEL_BULK, 0U, &first) == 0);
	assert(submit(&controller, 0x03U, MODEL_BULK, 0U, &second) == 0);
	assert(terminal_claim(&controller, first) == 1);
	controller.uhci_frnum = 101U;
	assert(terminal_claim(&controller, second) == 1);
	assert(retire(&controller, first, 101U, 1) == 0);
	request = request_find(&controller, second);
	assert(request != NULL && request->unlink_frnum == 101U);
	assert(retire(&controller, second, 101U, 1) == EAGAIN);
	assert(retire(&controller, second, 102U, 1) == 0);

	/* Unhealthy hardware fails closed and retains the complete request graph. */
	controller.uhci_frnum = 200U;
	assert(submit(&controller, 0x04U, MODEL_BULK, 0U, &failed) == 0);
	assert(terminal_claim(&controller, failed) == 1);
	request = request_find(&controller, failed);
	assert(request != NULL);
	assert(retire(&controller, failed, 201U, 0) == EIO);
	assert(request->state == REQUEST_FAILED && request->dma_owned);
	assert(controller.active_count == 1U && controller.dma_count == 1U);
}

static void
test_ehci_serialized_async_retirement(void)
{
	struct controller controller;
	struct request *first_request, *second_request;
	unsigned failed, first, second;

	controller_init(&controller, MODEL_EHCI);
	controller.attached = 1;
	controller.retirement_generation = 40U;
	/* A stale untagged status bit must be W1C-cleared and read back before
	 * software publishes the first owner and rings its one doorbell. */
	controller.iaa_status = 1;
	assert(submit(&controller, 0x81U, MODEL_BULK, 0U, &first) == 0);
	assert(submit(&controller, 0x02U, MODEL_BULK, 0U, &second) == 0);
	assert(terminal_claim(&controller, first) == 1);
	assert(terminal_claim(&controller, second) == 1);
	first_request = request_find(&controller, first);
	second_request = request_find(&controller, second);
	assert(first_request != NULL && second_request != NULL);
	assert(controller.iaa_clear_count == 1U && !controller.iaa_status);
	assert(controller.iaa_owner == first && controller.iaa_doorbell);
	assert(controller.iaa_doorbell_count == 1U);
	assert(!first_request->linked && second_request->linked);
	assert(second_request->retirement_generation ==
	    first_request->retirement_generation + 1U);
	/* An absent interrupt and an IRQ dispatch with no latched bit cannot create
	 * acknowledgement evidence or advance the serialized owner. */
	assert(retire(&controller, first, 0U, 1) == EAGAIN);
	assert(ehci_iaa_irq(&controller) == 0);
	assert(!first_request->iaa_observed && controller.iaa_owner == first);
	assert(retire(&controller, second, 0U, 1) == EAGAIN);
	assert(ehci_iaa_hardware_signal(&controller) == 1);
	assert(ehci_iaa_irq(&controller) == 1);
	/* A duplicate dispatch sees the cleared W1C bit.  Even an explicitly
	 * re-latched duplicate can only set the current owner's boolean once. */
	assert(ehci_iaa_irq(&controller) == 0);
	controller.iaa_status = 1;
	assert(ehci_iaa_irq(&controller) == 1);
	assert(controller.iaa_duplicate_count == 1U);
	assert(retire(&controller, first, 0U, 1) == 0);
	assert(controller.iaa_owner == second && controller.iaa_doorbell);
	assert(controller.iaa_doorbell_count == 2U);
	assert(!second_request->linked);
	/* Re-entry for the old, already-cleared IRQ supplies no bit and therefore
	 * cannot acknowledge the new owner. */
	assert(ehci_iaa_irq(&controller) == 0);
	assert(!second_request->iaa_observed);
	assert(retire(&controller, second, 0U, 1) == EAGAIN);
	assert(ehci_iaa_hardware_signal(&controller) == 1);
	assert(ehci_iaa_irq(&controller) == 1);
	assert(retire(&controller, second, 0U, 1) == 0);
	assert(retire(&controller, second, 0U, 1) == EBUSY);
	assert(controller.callback_count == 2U);
	assert(controller.active_count == 0U && controller.dma_count == 0U);

	/* A stale-bit clear/readback failure retains the complete request graph and
	 * does not publish an owner or ring IAAD; a later successful retry is exact. */
	controller_init(&controller, MODEL_EHCI);
	controller.attached = 1;
	controller.iaa_status = 1;
	controller.iaa_clear_stuck = 1;
	assert(submit(&controller, 0x03U, MODEL_BULK, 0U, &failed) == 0);
	assert(terminal_claim(&controller, failed) == 1);
	assert(controller.iaa_owner == 0U && !controller.iaa_doorbell);
	assert(request_find(&controller, failed)->linked);
	assert(retire(&controller, failed, 0U, 1) == EAGAIN);
	assert(controller.dma_count == 1U);
	controller.iaa_clear_stuck = 0;
	ehci_promote_async_retirement(&controller);
	assert(controller.iaa_owner == failed && controller.iaa_doorbell);
	assert(!request_find(&controller, failed)->linked);
	assert(ehci_iaa_hardware_signal(&controller) == 1);
	assert(ehci_iaa_irq(&controller) == 1);
	assert(retire(&controller, failed, 0U, 1) == 0);
	assert(controller.dma_count == 0U);
}

static void
test_ehci_periodic_cadence_and_masks(void)
{
	static const unsigned periods[EHCI_MAX_INTERVAL] = {
		1U, 1U, 1U, 1U, 2U, 4U, 8U, 16U, 32U, 64U, 128U, 256U,
		512U, 1024U
	};
	static const uint32_t masks[EHCI_MAX_INTERVAL] = {
		0xffU, 0x55U, 0x11U, 0x01U, 0x01U, 0x01U, 0x01U, 0x01U,
		0x01U, 0x01U, 0x01U, 0x01U, 0x01U, 0x01U
	};
	struct controller controller;
	unsigned ids[EHCI_MAX_INTERVAL];
	unsigned frame, index;

	controller_init(&controller, MODEL_EHCI);
	controller.attached = 1;
	for (index = 0U; index < EHCI_MAX_INTERVAL; index++) {
		struct request *request;
		unsigned first_frame = FRAME_COUNT, previous_frame = FRAME_COUNT;
		unsigned frame_count = 0U, level = 0U, mask_count = 0U;
		uint32_t mask;

		assert(submit(&controller, 0x100U + index, MODEL_INTERRUPT,
		    index + 1U, &ids[index]) == 0);
		request = request_find(&controller, ids[index]);
		assert(request != NULL && request->periodic_period == periods[index]);
		assert(request->service_mask == masks[index]);
		while ((1U << level) < request->periodic_period)
			level++;
		assert(request->periodic_node >= (1U << level) - 1U);
		assert(request->periodic_node < (1U << (level + 1U)) - 1U);
		assert(request->periodic_node == (1U << level) - 1U +
		    reverse_bits(request->periodic_phase, level));
		for (frame = 0U; frame < FRAME_COUNT; frame++)
			if (ehci_frame_reaches_node(frame, request->periodic_node)) {
				if (first_frame == FRAME_COUNT)
					first_frame = frame;
				else
					assert(frame - previous_frame ==
					    request->periodic_period);
				previous_frame = frame;
				frame_count++;
			}
		assert(frame_count == FRAME_COUNT / request->periodic_period);
		assert(first_frame + FRAME_COUNT - previous_frame ==
		    request->periodic_period);
		assert((request->service_mask & ~0xffU) == 0U);
		for (mask = request->service_mask; mask != 0U; mask >>= 1)
			mask_count += mask & 1U;
		assert(frame_count * mask_count ==
		    (FRAME_COUNT * 8U) / (1U << index));
	}
	assert(submit(&controller, 0x200U, MODEL_INTERRUPT, 0U, &frame) == EINVAL);
	assert(submit(&controller, 0x201U, MODEL_INTERRUPT,
	    EHCI_MAX_INTERVAL + 1U, &frame) == ENOTSUP);
	for (index = 0U; index < EHCI_MAX_INTERVAL; index++)
		assert(complete_request(&controller, ids[index]) == 0);
	for (frame = 0U; frame < FRAME_COUNT; frame++)
		assert(controller.frame_members[frame] == 0U);
	assert(controller.active_count == 0U && controller.dma_count == 0U &&
	    controller.builder_count == 0U);
}

static void
test_ehci_periodic_bandwidth(void)
{
	struct ehci_budget_model budget;

	memset(&budget, 0, sizeof(budget));
	assert(ehci_budget_reserve(&budget, 1U, 0U, 0xffU, 30208U) == 0);
	assert(ehci_budget_reserve(&budget, 1U, 0U, 0xffU, 30208U) == ENOSPC);
	ehci_budget_release(&budget, 1U, 0U, 0xffU, 30208U);
	assert(ehci_budget_reserve(&budget, 1U, 0U, 0xffU, 24000U) == 0);
	assert(ehci_budget_reserve(&budget, 1U, 0U, 0xffU, 24000U) == 0);
	assert(ehci_budget_reserve(&budget, 1U, 0U, 0xffU, 1U) == ENOSPC);
	ehci_budget_release(&budget, 1U, 0U, 0xffU, 24000U);
	ehci_budget_release(&budget, 1U, 0U, 0xffU, 24000U);

	/* Disjoint microframe phases and disjoint frame phases reserve
	 * independently, while invalid requests fail before changing the table. */
	assert(ehci_budget_reserve(&budget, 1U, 0U, 0x55U, 48000U) == 0);
	assert(ehci_budget_reserve(&budget, 1U, 0U, 0xaaU, 48000U) == 0);
	ehci_budget_release(&budget, 1U, 0U, 0x55U, 48000U);
	ehci_budget_release(&budget, 1U, 0U, 0xaaU, 48000U);
	assert(ehci_budget_reserve(&budget, 128U, 0U, 0x01U, 48000U) == 0);
	assert(ehci_budget_reserve(&budget, 128U, 0U, 0x01U, 1U) == ENOSPC);
	assert(ehci_budget_reserve(&budget, 128U, 1U, 0x01U, 48000U) == 0);
	assert(ehci_budget_reserve(&budget, 0U, 0U, 0x01U, 1U) == EINVAL);
	ehci_budget_release(&budget, 128U, 0U, 0x01U, 48000U);
	ehci_budget_release(&budget, 128U, 1U, 0x01U, 48000U);
}

static void
test_ehci_periodic_checked_barrier(void)
{
	struct controller controller;
	struct request *periodic_request;
	unsigned periodic, storage, callbacks;

	controller_init(&controller, MODEL_EHCI);
	controller.attached = 1;
	assert(submit(&controller, 0x81U, MODEL_INTERRUPT, 4U,
	    &periodic) == 0);
	assert(submit(&controller, 0x02U, MODEL_BULK, 0U, &storage) == 0);
	assert(terminal_claim(&controller, periodic) == 1);
	periodic_request = request_find(&controller, periodic);
	assert(periodic_request != NULL && periodic_request->dma_owned);
	assert(retire(&controller, periodic,
	    EHCI_PERIODIC_BARRIER_COMPLETE &
	    ~EHCI_PERIODIC_COMMAND_ENABLED, 1) == EAGAIN);
	assert(retire(&controller, periodic,
	    EHCI_PERIODIC_BARRIER_COMPLETE &
	    ~EHCI_PERIODIC_STATUS_CLEAR, 1) == EAGAIN);
	/* Async storage progresses while periodic unlink awaits its checked
	 * controller barrier. */
	callbacks = controller.callback_count;
	assert(complete_request(&controller, storage) == 0);
	assert(controller.callback_count == callbacks + 1U);
	assert(periodic_request->state == REQUEST_RETIRING);
	assert(periodic_request->dma_owned && controller.dma_count == 1U);
	assert(retire(&controller, periodic,
	    EHCI_PERIODIC_BARRIER_COMPLETE, 1) == 0);
	assert(controller.active_count == 0U && controller.dma_count == 0U);
}

static void
test_builder_and_shutdown(enum controller_kind kind)
{
	struct controller controller;
	struct request *request;
	unsigned id;

	controller_init(&controller, kind);
	controller.attached = 1;
	assert(submit_begin(&controller, &request, 1U) == ENOMEM);
	assert(controller.builder_count == 0U && controller.dma_count == 0U);
	assert(submit_begin(&controller, &request, 2U) == ENOMEM);
	assert(controller.builder_count == 0U && controller.dma_count == 0U);
	assert(submit_begin(&controller, &request, 0U) == 0);
	controller.admission_open = 0;
	assert(submit_commit(&controller, request, 0x81U, MODEL_INTERRUPT, 1U,
	    0U) == ENODEV);
	assert(controller.builder_count == 0U && controller.dma_count == 0U);
	controller.admission_open = 1;
	assert(submit_begin(&controller, &request, 0U) == 0);
	assert(submit_commit(&controller, request, 0x82U, MODEL_BULK, 0U,
	    2U) == EIO);
	assert(controller.builder_count == 0U && controller.dma_count == 0U);
	assert(submit_begin(&controller, &request, 0U) == 0);
	assert(submit_commit(&controller, request, 0x81U, MODEL_INTERRUPT, 0U,
	    0U) == EINVAL);
	assert(controller.dma_count == 0U);
	assert(submit(&controller, 0x81U, MODEL_INTERRUPT, 1U, &id) == 0);
	controller.topology_locked = 1;
	root_latch(&controller);
	assert(root_worker_step(&controller, 0, 1, 0) == EBUSY);
	controller.topology_locked = 0;
	assert(root_worker_step(&controller, 0, 1, 0) == 0);
	assert(controller.active_count == 0U && controller.dma_count == 0U);
	root_worker_stop(&controller);
	assert(root_worker_step(&controller, 1, 1, 0) == ENODEV);
}

static void
test_worker_failure_and_self_stop(void)
{
	struct worker_lifecycle worker;

	memset(&worker, 0, sizeof(worker));
	assert(worker_start(&worker, 1U) == ENOMEM);
	assert(worker.allocations == 0U && worker.releases == 0U);
	assert(!worker.published && !worker.running);
	assert(worker_start(&worker, 2U) == EIO);
	assert(worker.allocations == 1U && worker.releases == 1U);
	assert(!worker.published && !worker.running);

	assert(worker_start(&worker, 0U) == 0);
	worker.pending = 1;
	assert(worker_stop(&worker, 1, 0) == EDEADLK);
	assert(worker.published && worker.stopping && !worker.running);
	assert(!worker.pending && !worker.joining && !worker.reaped);
	/* The external owner observes the self-stop and performs the sole join. */
	assert(worker_stop(&worker, 0, 0) == 0);
	assert(!worker.published && !worker.joining && worker.reaped);
	assert(worker.allocations == 2U && worker.releases == 2U);

	memset(&worker, 0, sizeof(worker));
	assert(worker_start(&worker, 0U) == 0);
	worker.pending = 1;
	assert(worker_stop(&worker, 0, 1) == EIO);
	/* Join failure is fail-closed: publication and allocation stay owned,
	 * and a later external caller can retry the reap exactly once. */
	assert(worker.published && !worker.joining && !worker.reaped);
	assert(worker.allocations == 1U && worker.releases == 0U);
	assert(worker_stop(&worker, 0, 0) == 0);
	assert(!worker.published && worker.reaped);
	assert(worker.allocations == 1U && worker.releases == 1U);
	assert(worker_stop(&worker, 0, 0) == 0);
	assert(worker.releases == 1U);
}

static void
test_partial_startup_failure_matrix(enum controller_kind kind)
{
	struct startup_lifecycle startup;
	enum startup_failure_stage stage;
	unsigned schedule_count = kind == MODEL_UHCI ? 4U : 5U;

	for (stage = START_FAIL_SCHEDULE_FRAME;
	    stage <= START_FAIL_HID_WORKER_PUBLICATION; stage++) {
		if (kind == MODEL_UHCI && stage == START_FAIL_SCHEDULE_ASYNC)
			continue;
		assert(startup_attempt(&startup, kind, stage) != 0);
		assert(startup.schedule_allocations == 0U);
		assert(startup.schedule_releases <= schedule_count);
		assert(!startup.hcd_registered && !startup.hid_driver_data);
		assert(!startup.retirement.published && !startup.root.published &&
		    !startup.hid.published);
		assert(startup.retirement.allocations ==
		    startup.retirement.releases);
		assert(startup.root.allocations == startup.root.releases);
		assert(startup.hid.allocations == startup.hid.releases);
		assert(startup.hid_allocations == 0U);
		assert(startup.hid_releases == 0U ||
		    startup.hid_releases == 3U);
	}
	assert(startup_attempt(&startup, kind, START_FAIL_NONE) == 0);
	assert(startup.hcd_registered && startup.hid_driver_data);
	assert(startup.schedule_allocations == schedule_count &&
	    startup.retirement.published && startup.root.published &&
	    startup.hid.published);
	startup_cleanup(&startup);
	assert(startup.schedule_allocations == 0U &&
	    startup.schedule_releases == schedule_count);
	assert(!startup.hcd_registered && !startup.hid_driver_data);
	assert(startup.retirement.releases == 1U &&
	    startup.root.releases == 1U && startup.hid.releases == 1U);
	assert(startup.hid_releases == 3U);
}

static void
test_hotplug_generations(enum controller_kind kind)
{
	struct controller controller;
	unsigned iteration;

	controller_init(&controller, kind);
	controller.attached = 1;
	for (iteration = 0; iteration < 100U; iteration++) {
		unsigned old_generation = controller.root_generation;
		unsigned interrupt_id, storage_id;

		assert(submit(&controller, 0x81U, MODEL_INTERRUPT, 8U,
		    &interrupt_id) == 0);
		assert(submit(&controller, 0x02U, MODEL_BULK, 0U,
		    &storage_id) == 0);
		assert(complete_request(&controller, storage_id) == 0);
		root_latch(&controller);
		root_latch(&controller); /* Coalesced before worker dispatch. */
		assert(root_worker_step(&controller, 0, 1, 0) == 0);
		assert(controller.active_count == 0U);
		late_completion(&controller, interrupt_id, old_generation);
		root_latch(&controller);
		assert(root_worker_step(&controller, 1, 1, 0) == 0);
		assert(controller.root_generation == old_generation + 1U);
	}
	assert(controller.root_detaches == 100U);
	assert(controller.root_inserts == 100U);
	assert(controller.root_scans == 200U);
	assert(controller.root_events == 300U);
	assert(controller.ignored_late_count == 100U);
	assert(controller.active_count == 0U && controller.dma_count == 0U);
	assert(controller.builder_count == 0U);
}

static void
test_collapsed_disconnect_reconnect(enum controller_kind kind)
{
	struct controller controller;
	unsigned old_generation, old_interrupt, new_interrupt;

	controller_init(&controller, kind);
	controller.attached = 1;
	old_generation = controller.root_generation;
	assert(submit(&controller, 0x81U, MODEL_INTERRUPT, 8U,
	    &old_interrupt) == 0);
	root_latch(&controller);
	/* Both wire transitions collapse into one connected+C_CONNECTION sample. */
	assert(root_worker_step(&controller, 1, 1, 0) == 0);
	assert(controller.root_detaches == 1U);
	assert(controller.root_inserts == 1U);
	assert(controller.root_generation == old_generation + 1U);
	late_completion(&controller, old_interrupt, old_generation);
	assert(submit(&controller, 0x81U, MODEL_INTERRUPT, 8U,
	    &new_interrupt) == 0);
	assert(complete_request(&controller, new_interrupt) == 0);
	assert(controller.ignored_late_count == 1U);
	assert(controller.root_events == 1U && controller.root_scans == 1U);
	assert(controller.active_count == 0U && controller.dma_count == 0U);
	assert(controller.builder_count == 0U);
}

static void
test_disconnect_retry_without_change(enum controller_kind kind)
{
	struct controller controller;
	unsigned old_generation, old_interrupt, new_interrupt;

	controller_init(&controller, kind);
	controller.attached = 1;
	controller.root_teardown_busy_once = 1U;
	old_generation = controller.root_generation;
	assert(submit(&controller, 0x81U, MODEL_INTERRUPT, 8U,
	    &old_interrupt) == 0);
	root_latch(&controller);
	assert(root_worker_step(&controller, 1, 1, 0) == EBUSY);
	assert(controller.attached && !controller.admission_open);
	assert(controller.root_retry_pending);
	assert(controller.root_generation == old_generation);
	assert(controller.root_detaches == 0U && controller.root_inserts == 0U);
	assert(controller.active_count == 0U && controller.dma_count == 0U);
	/* No new change bit is latched.  A periodic scan must retry the retained
	 * disconnecting generation and only then enumerate the connected device. */
	assert(root_worker_step(&controller, 1, 0, 0) == 0);
	assert(!controller.root_retry_pending);
	assert(controller.root_detaches == 1U);
	assert(controller.root_inserts == 1U);
	assert(controller.root_generation == old_generation + 1U);
	late_completion(&controller, old_interrupt, old_generation);
	assert(submit(&controller, 0x81U, MODEL_INTERRUPT, 8U,
	    &new_interrupt) == 0);
	assert(complete_request(&controller, new_interrupt) == 0);
	assert(controller.ignored_late_count == 1U);
	assert(controller.root_events == 1U && controller.root_scans == 2U);
	assert(controller.active_count == 0U && controller.dma_count == 0U);
	assert(controller.builder_count == 0U);
}

static void
test_port_disable_recovery(enum controller_kind kind)
{
	struct controller controller;
	unsigned old_generation, old_interrupt, new_interrupt;

	controller_init(&controller, kind);
	controller.attached = 1;
	old_generation = controller.root_generation;
	assert(submit(&controller, 0x81U, MODEL_INTERRUPT, 8U,
	    &old_interrupt) == 0);
	root_latch(&controller);
	/* A connected port which loses enable is reset and re-enumerated as a new
	 * generation even if no connection edge accompanied C_PORT_ENABLE. */
	assert(root_worker_step(&controller, 1, 0, 1) == 0);
	assert(controller.root_detaches == 1U);
	assert(controller.root_inserts == 1U);
	assert(controller.root_generation == old_generation + 1U);
	late_completion(&controller, old_interrupt, old_generation);
	assert(submit(&controller, 0x81U, MODEL_INTERRUPT, 8U,
	    &new_interrupt) == 0);
	assert(complete_request(&controller, new_interrupt) == 0);
	assert(controller.ignored_late_count == 1U);
	assert(controller.active_count == 0U && controller.dma_count == 0U);
}

static int
ehci_reset_handoff_model(unsigned companion_count, unsigned connected,
	unsigned enabled, unsigned owner_write_sticks, unsigned *owner)
{
	*owner = 0U;
	if (!connected)
		return ENODEV;
	if (enabled)
		return 0;
	if (companion_count == 0U)
		return ENOTSUP;
	if (!owner_write_sticks)
		return EIO;
	*owner = 1U;
	return 0;
}

static void
test_ehci_companion_handoff(void)
{
	unsigned owner;

	/* A companion-less EHCI still accepts a high-speed device.  It rejects
	 * only the low/full-speed handoff which the hardware cannot satisfy. */
	assert(ehci_reset_handoff_model(0U, 1U, 1U, 0U, &owner) == 0);
	assert(owner == 0U);
	assert(ehci_reset_handoff_model(0U, 1U, 0U, 0U, &owner) == ENOTSUP);
	assert(owner == 0U);
	assert(ehci_reset_handoff_model(1U, 1U, 0U, 1U, &owner) == 0);
	assert(owner == 1U);
	assert(ehci_reset_handoff_model(1U, 1U, 0U, 0U, &owner) == EIO);
	assert(owner == 0U);
	assert(ehci_reset_handoff_model(1U, 0U, 0U, 1U, &owner) == ENODEV);
}

static void
test_reclaim_safe_preallocated_reserve(enum controller_kind kind)
{
	struct reclaim_reserve_model reserve;

	reclaim_reserve_init(&reserve, kind);
	assert(reserve.schedule_size == 4096U);
	assert(reserve.bounce_size == RECLAIM_SAFE_MAX_SIZE +
	    USB_CONTROL_REQUEST_SIZE);
	reserve.allocation_forbidden = 1U;

	/* The reclaim-safe path succeeds at its public bound without attempting a
	 * request, schedule, or bounce allocation.  Its one controller-start reserve
	 * serializes a second recovery submission. */
	assert(reclaim_request_acquire(&reserve, 1,
	    RECLAIM_SAFE_MAX_SIZE) == 0);
	assert(reserve.dynamic_allocations == 0U);
	assert(reserve.forbidden_allocation_attempts == 0U);
	assert(reserve.busy && reserve.dma_owned);
	assert(reclaim_request_acquire(&reserve, 1, 1U) == EBUSY);
	/* The serialized reserve is not a controller-wide admission gate.  An
	 * unrelated ordinary endpoint keeps using its independent dynamic path. */
	reserve.allocation_forbidden = 0U;
	assert(reclaim_request_acquire(&reserve, 0, 1U) == 0);
	assert(reserve.dynamic_allocations == 1U);
	assert(reserve.busy && reserve.dma_owned);
	reserve.allocation_forbidden = 1U;

	/* A failed retirement proof retains the reserve and prevents unsafe DMA
	 * reuse.  Positive evidence alone releases it for the next recovery. */
	assert(reclaim_request_retire(&reserve, 0) == EIO);
	assert(reserve.busy && reserve.dma_owned);
	assert(reclaim_request_acquire(&reserve, 1, 1U) == EBUSY);
	assert(reclaim_request_retire(&reserve, 1) == 0);
	assert(!reserve.busy && !reserve.dma_owned);
	assert(reclaim_request_acquire(&reserve, 1, 0U) == 0);
	assert(reclaim_request_retire(&reserve, 1) == 0);

	/* The flag selects the reserve: oversize reclaim work fails before taking it,
	 * while an ordinary request still reaches the dynamic allocator. */
	assert(reclaim_request_acquire(&reserve, 1,
	    RECLAIM_SAFE_MAX_SIZE + 1U) == EMSGSIZE);
	assert(!reserve.busy && !reserve.dma_owned);
	assert(reclaim_request_acquire(&reserve, 0, 1U) == ENOMEM);
	assert(reserve.forbidden_allocation_attempts == 1U);
	assert(reserve.dynamic_allocations == 1U);
}

int
main(void)
{
	test_concurrent_progress(MODEL_UHCI);
	test_concurrent_progress(MODEL_EHCI);
	test_cancel_irq_exactly_once(MODEL_UHCI, TERMINAL_IRQ);
	test_cancel_irq_exactly_once(MODEL_UHCI, TERMINAL_CANCEL);
	test_cancel_irq_exactly_once(MODEL_EHCI, TERMINAL_IRQ);
	test_cancel_irq_exactly_once(MODEL_EHCI, TERMINAL_CANCEL);
	test_request_local_retirement(MODEL_UHCI);
	test_request_local_retirement(MODEL_EHCI);
	test_uhci_qh_element_advance_workaround();
	test_uhci_periodic_cadence_and_retirement();
	test_ehci_serialized_async_retirement();
	test_ehci_periodic_cadence_and_masks();
	test_ehci_periodic_bandwidth();
	test_ehci_periodic_checked_barrier();
	test_ehci_companion_handoff();
	test_reclaim_safe_preallocated_reserve(MODEL_UHCI);
	test_reclaim_safe_preallocated_reserve(MODEL_EHCI);
	test_builder_and_shutdown(MODEL_UHCI);
	test_builder_and_shutdown(MODEL_EHCI);
	test_worker_failure_and_self_stop();
	test_partial_startup_failure_matrix(MODEL_UHCI);
	test_partial_startup_failure_matrix(MODEL_EHCI);
	test_hotplug_generations(MODEL_UHCI);
	test_hotplug_generations(MODEL_EHCI);
	test_collapsed_disconnect_reconnect(MODEL_UHCI);
	test_collapsed_disconnect_reconnect(MODEL_EHCI);
	test_disconnect_retry_without_change(MODEL_UHCI);
	test_disconnect_retry_without_change(MODEL_EHCI);
	test_port_disable_recovery(MODEL_UHCI);
	test_port_disable_recovery(MODEL_EHCI);
	puts("HW-T25 legacy HCD concurrency/hotplug model: PASS");
	return 0;
}
