/* WS004-p011 host model for per-endpoint xHCI request ownership. */
#include <drivers/pci-xhci-lifecycle.h>
#include <drivers/usb.h>

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MODEL_SLOTS 3U
#define MODEL_DCIS 32U
#define MODEL_RING_TRBS 256U

enum model_terminal {
	MODEL_TERMINAL_NONE,
	MODEL_TERMINAL_COMPLETE,
	MODEL_TERMINAL_CANCELLED,
	MODEL_TERMINAL_DISCONNECTED
};

struct model_request {
	const char *name;
	uint64_t ring;
	size_t length;
	unsigned slot;
	unsigned dci;
	unsigned first_trb;
	unsigned trb_count;
	unsigned reclaim_safe;
	unsigned reserve_owned;
	unsigned cancelling;
	unsigned late_event_seen;
	enum model_terminal terminal;
};

struct model_controller {
	struct model_request *active[MODEL_SLOTS][MODEL_DCIS];
	const char *completion_order[16];
	unsigned completion_count;
	unsigned active_count;
	unsigned reserve_busy;
	unsigned halted;
	unsigned busmaster_off;
	unsigned irq_drained;
	unsigned event_claim_busy;
};

static int
model_submit(struct model_controller *controller,
	struct model_request *request)
{
	enum drv_xhci_reserve_action action;

	if (request->slot == 0 || request->slot >= MODEL_SLOTS ||
	    request->dci == 0 || request->dci >= MODEL_DCIS)
		return EINVAL;
	if (controller->active[request->slot][request->dci] != NULL)
		return EBUSY;
	action = drv_xhci_reserve_action(request->reclaim_safe,
	    request->length, DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE, 1,
	    controller->reserve_busy != 0);
	if (action == DRV_XHCI_RESERVE_BUSY)
		return EBUSY;
	if (action == DRV_XHCI_RESERVE_REJECT)
		return request->length > DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE ?
		    EMSGSIZE : ENOMEM;
	request->reserve_owned = action == DRV_XHCI_RESERVE_USE;
	if (request->reserve_owned)
		controller->reserve_busy = 1U;
	request->terminal = MODEL_TERMINAL_NONE;
	request->cancelling = 0;
	request->late_event_seen = 0;
	controller->active[request->slot][request->dci] = request;
	controller->active_count++;
	return 0;
}

static void
model_unlink(struct model_controller *controller,
	struct model_request *request, enum model_terminal terminal)
{
	assert(controller->active[request->slot][request->dci] == request);
	assert(controller->active_count != 0);
	controller->active[request->slot][request->dci] = NULL;
	controller->active_count--;
	if (request->reserve_owned) {
		assert(controller->reserve_busy);
		controller->reserve_busy = 0;
		request->reserve_owned = 0;
	}
	request->terminal = terminal;
	assert(controller->completion_count < 16U);
	controller->completion_order[controller->completion_count++] =
	    request->name;
}

static int
model_event(struct model_controller *controller, unsigned slot, unsigned dci,
	uint64_t pointer)
{
	struct model_request *request;
	unsigned trb_offset;

	if (slot == 0 || slot >= MODEL_SLOTS || dci == 0 ||
	    dci >= MODEL_DCIS)
		return 0;
	request = controller->active[slot][dci];
	if (request == NULL ||
	    !drv_xhci_transfer_event_matches(request->ring, MODEL_RING_TRBS,
		request->slot, request->dci, request->first_trb,
		request->trb_count, pointer, slot, dci, &trb_offset))
		return 0;
	assert(trb_offset < request->trb_count);
	if (request->cancelling) {
		request->late_event_seen = 1U;
		return 1;
	}
	model_unlink(controller, request, MODEL_TERMINAL_COMPLETE);
	return 1;
}

static void
model_cancel_begin(struct model_controller *controller,
	struct model_request *request)
{
	assert(controller->active[request->slot][request->dci] == request);
	assert(!request->cancelling);
	request->cancelling = 1U;
}

static int
model_cancel_finish(struct model_controller *controller,
	struct model_request *request, int set_dequeue_succeeded,
	int controller_quiesced, enum model_terminal terminal)
{
	/* Stop/Set TR Dequeue cannot pass a Transfer Event consumer which has
	 * dequeued the event but still holds event_lock for its ownership claim. */
	if (set_dequeue_succeeded && !controller_quiesced &&
	    controller->event_claim_busy)
		return EBUSY;
	if (!drv_xhci_request_resources_releasable(set_dequeue_succeeded,
	    controller_quiesced)) {
		request->cancelling = 2U;
		return EIO;
	}
	model_unlink(controller, request, terminal);
	return 0;
}

static void
model_event_dequeue_begin(struct model_controller *controller)
{
	assert(!controller->event_claim_busy);
	controller->event_claim_busy = 1U;
}

static int
model_event_claim_finish(struct model_controller *controller, unsigned slot,
	unsigned dci, uint64_t pointer)
{
	int claimed;

	assert(controller->event_claim_busy);
	claimed = model_event(controller, slot, dci, pointer);
	controller->event_claim_busy = 0;
	return claimed;
}

static unsigned
model_device_drain(struct model_controller *controller, unsigned slot)
{
	unsigned dci, drained = 0;

	for (dci = 1; dci < MODEL_DCIS; dci++) {
		struct model_request *request = controller->active[slot][dci];

		if (request == NULL)
			continue;
		model_cancel_begin(controller, request);
		assert(model_cancel_finish(controller, request, 1, 0,
		    MODEL_TERMINAL_DISCONNECTED) == 0);
		drained++;
	}
	return drained;
}

static int
model_controller_drain(struct model_controller *controller)
{
	unsigned slot, dci;

	if (!controller->halted || !controller->busmaster_off ||
	    !controller->irq_drained)
		return EBUSY;
	for (slot = 1; slot < MODEL_SLOTS; slot++)
		for (dci = 1; dci < MODEL_DCIS; dci++) {
			struct model_request *request =
			    controller->active[slot][dci];

			if (request == NULL)
				continue;
			request->cancelling = 1U;
			assert(model_cancel_finish(controller, request, 0, 1,
			    MODEL_TERMINAL_DISCONNECTED) == 0);
		}
	return 0;
}

static struct model_request
request(const char *name, unsigned slot, unsigned dci, unsigned first,
	unsigned count, size_t length, int reclaim_safe)
{
	struct model_request result;

	memset(&result, 0, sizeof(result));
	result.name = name;
	result.slot = slot;
	result.dci = dci;
	result.ring = 0x100000ULL + ((uint64_t)slot << 16) +
	    ((uint64_t)dci << 12);
	result.first_trb = first;
	result.trb_count = count;
	result.length = length;
	result.reclaim_safe = reclaim_safe != 0;
	return result;
}

static uint64_t
last_trb(const struct model_request *request)
{
	unsigned index = (request->first_trb + request->trb_count - 1U) %
	    (MODEL_RING_TRBS - 1U);

	return request->ring + (uint64_t)index * 16U;
}

static int
next_permutation(unsigned *items, unsigned count)
{
	unsigned i, j, temporary;

	for (i = count - 1U; i != 0 && items[i - 1U] >= items[i]; i--)
		;
	if (i == 0)
		return 0;
	for (j = count - 1U; items[j] <= items[i - 1U]; j--)
		;
	temporary = items[i - 1U];
	items[i - 1U] = items[j];
	items[j] = temporary;
	for (j = count - 1U; i < j; i++, j--) {
		temporary = items[i];
		items[i] = items[j];
		items[j] = temporary;
	}
	return 1;
}

static void
test_completion_permutations(void)
{
	static const char *const names[] = {
		"control", "notification", "rx", "tx", "storage"
	};
	unsigned order[] = { 0, 1, 2, 3, 4 };
	unsigned permutations = 0;

	do {
		struct model_controller controller = { 0 };
		struct model_request requests[] = {
			request(names[0], 1, 1, 10, 3, 64, 0),
			request(names[1], 1, 3, 30, 1, 8, 0),
			request(names[2], 1, 5, 254, 2, 2048, 0),
			request(names[3], 1, 4, 50, 1, 2048, 0),
			request(names[4], 2, 3, 70, 1, 4096, 1)
		};
		unsigned i;

		for (i = 0; i < 5U; i++)
			assert(model_submit(&controller, &requests[i]) == 0);
		assert(controller.active_count == 5U);
		assert(controller.reserve_busy == 1U);
		for (i = 0; i < 5U; i++) {
			unsigned which = order[i];

			assert(model_event(&controller, requests[which].slot,
			    requests[which].dci, last_trb(&requests[which])));
			assert(controller.completion_order[i] == names[which]);
		}
		assert(controller.active_count == 0);
		assert(controller.reserve_busy == 0);
		permutations++;
	} while (next_permutation(order, 5U));
	assert(permutations == 120U);
}

static void
test_identity_and_cancel(void)
{
	struct model_controller controller = { 0 };
	struct model_request old = request("old-rx", 1, 5, 254, 2, 512, 0);
	struct model_request other = request("tx", 1, 4, 254, 2, 512, 0);
	struct model_request replacement =
	    request("new-rx", 1, 5, 254, 2, 512, 0);
	struct model_request duplicate =
	    request("duplicate", 1, 5, 100, 1, 512, 0);
	uint64_t stale = last_trb(&old);

	assert(model_submit(&controller, &old) == 0);
	assert(model_submit(&controller, &other) == 0);
	assert(model_submit(&controller, &duplicate) == EBUSY);
	assert(!model_event(&controller, 2, old.dci, stale));
	assert(!model_event(&controller, old.slot, old.dci + 1U, stale));
	assert(!model_event(&controller, old.slot, old.dci, stale + 1U));
	assert(!model_event(&controller, old.slot, old.dci,
	    old.ring + 255U * 16U));

	/* Reproduce the critical ordering: IRQ dequeues an event, cancellation
	 * starts, then IRQ resumes.  The claim gate prevents Set TR Dequeue and
	 * exact ring-address reuse from overtaking the dequeued event. */
	model_event_dequeue_begin(&controller);
	model_cancel_begin(&controller, &old);
	assert(model_cancel_finish(&controller, &old, 1, 0,
	    MODEL_TERMINAL_CANCELLED) == EBUSY);
	assert(model_event_claim_finish(&controller, old.slot, old.dci, stale));
	assert(old.late_event_seen);
	assert(model_cancel_finish(&controller, &old, 0, 0,
	    MODEL_TERMINAL_CANCELLED) == EIO);
	/* A software-only unlink is forbidden: the endpoint remains owned and
	 * its ring cannot be reused until Set TR Dequeue succeeds. */
	assert(model_submit(&controller, &replacement) == EBUSY);
	assert(other.terminal == MODEL_TERMINAL_NONE);
	assert(model_event(&controller, other.slot, other.dci,
	    last_trb(&other)));
	assert(other.terminal == MODEL_TERMINAL_COMPLETE);

	assert(model_cancel_finish(&controller, &old, 1, 0,
	    MODEL_TERMINAL_CANCELLED) == 0);
	assert(model_submit(&controller, &replacement) == 0);
	/* Model immediate endpoint/ring reuse with the exact same address.  The
	 * old event was already claimed before the gate opened, so it cannot be
	 * replayed against this temporally newer owner. */
	assert(last_trb(&replacement) == stale);
	assert(replacement.terminal == MODEL_TERMINAL_NONE);
	assert(model_event(&controller, replacement.slot, replacement.dci,
	    last_trb(&replacement)));
	assert(replacement.terminal == MODEL_TERMINAL_COMPLETE);
}

static void
test_device_and_controller_quiesce(void)
{
	struct model_controller controller = { 0 };
	struct model_request control = request("control", 1, 1, 1, 2, 0, 0);
	struct model_request rx = request("rx", 1, 5, 8, 1, 2048, 0);
	struct model_request storage =
	    request("storage", 2, 3, 9, 1, 4096, 1);

	assert(model_submit(&controller, &control) == 0);
	assert(model_submit(&controller, &rx) == 0);
	assert(model_submit(&controller, &storage) == 0);
	assert(model_device_drain(&controller, 1) == 2U);
	assert(control.terminal == MODEL_TERMINAL_DISCONNECTED);
	assert(rx.terminal == MODEL_TERMINAL_DISCONNECTED);
	assert(storage.terminal == MODEL_TERMINAL_NONE);
	assert(controller.active_count == 1U);
	assert(controller.reserve_busy == 1U);
	assert(model_controller_drain(&controller) == EBUSY);
	assert(storage.terminal == MODEL_TERMINAL_NONE);
	controller.halted = 1U;
	controller.busmaster_off = 1U;
	controller.irq_drained = 1U;
	assert(model_controller_drain(&controller) == 0);
	assert(storage.terminal == MODEL_TERMINAL_DISCONNECTED);
	assert(controller.active_count == 0);
	assert(controller.reserve_busy == 0);
}

static void
test_reclaim_reserve(void)
{
	struct model_controller controller = { 0 };
	struct model_request storage =
	    request("storage", 2, 3, 1, 1, 8192, 1);
	struct model_request storage2 =
	    request("storage2", 1, 7, 1, 1, 512, 1);
	struct model_request oversize =
	    request("oversize", 1, 9, 1, 1, 8193, 1);
	struct model_request ncm_rx =
	    request("ncm-rx", 1, 5, 1, 1, 512, 0);

	assert(model_submit(&controller, &storage) == 0);
	assert(storage.reserve_owned);
	assert(model_submit(&controller, &storage2) == EBUSY);
	/* Ordinary NCM traffic remains dynamic even while the storage reserve is
	 * occupied; it can never consume or block that reserve itself. */
	assert(model_submit(&controller, &ncm_rx) == 0);
	assert(!ncm_rx.reserve_owned);
	assert(model_submit(&controller, &oversize) == EMSGSIZE);
	assert(model_event(&controller, ncm_rx.slot, ncm_rx.dci,
	    last_trb(&ncm_rx)));
	assert(model_event(&controller, storage.slot, storage.dci,
	    last_trb(&storage)));
	assert(model_submit(&controller, &storage2) == 0);
	assert(storage2.reserve_owned);
	assert(model_event(&controller, storage2.slot, storage2.dci,
	    last_trb(&storage2)));
	assert(controller.reserve_busy == 0);
}

struct command_callback_model {
	unsigned command_busy;
	unsigned completion_claimed;
	unsigned completion_published;
	unsigned nested_command_completed;
};

static void
model_callback_with_nested_command(struct command_callback_model *model)
{
	/* A callback may take a command-using USB path.  It must be dispatched
	 * only after the polling command releases command_busy. */
	assert(!model->command_busy);
	model->command_busy = 1U;
	model->nested_command_completed++;
	model->command_busy = 0;
}

static void
test_command_callback_deferral(void)
{
	struct command_callback_model model = { 0 };

	model.command_busy = 1U;
	/* command_ex consumes and ownership-claims an unrelated Transfer Event,
	 * but must not publish it or invoke its callback inline. */
	model.completion_claimed = 1U;
	assert(model.completion_published == 0);
	assert(model.nested_command_completed == 0);
	model.command_busy = 0;
	model.completion_published = 1U;
	model_callback_with_nested_command(&model);
	assert(model.completion_claimed && model.completion_published);
	assert(model.nested_command_completed == 1U);
}

int
main(void)
{
	test_completion_permutations();
	test_identity_and_cancel();
	test_device_and_controller_quiesce();
	test_reclaim_reserve();
	test_command_callback_deferral();
	puts("xHCI concurrent URB test: PASS");
	return 0;
}
