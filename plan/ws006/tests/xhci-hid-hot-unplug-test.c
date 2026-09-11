/* Focused model for the xHCI HID hot-unplug cancellation boundary. */
#include <drivers/pci-xhci-lifecycle.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>

struct cancellation_result {
	unsigned restart_calls;
	unsigned releases;
};

static int
model_post_dequeue(int device_teardown, int restart_error,
	struct cancellation_result *result)
{
	enum drv_xhci_post_dequeue_action action;

	action = drv_xhci_cancel_post_dequeue_action(device_teardown);
	if (action == DRV_XHCI_POST_DEQUEUE_RESTART_ENDPOINT) {
		result->restart_calls++;
		if (restart_error != 0)
			return restart_error;
	}
	if (!drv_xhci_request_resources_releasable(1, 0))
		return EIO;
	result->releases++;
	return 0;
}

static void
test_ordinary_cancel_restarts(void)
{
	struct cancellation_result result = { 0 };

	assert(model_post_dequeue(0, 0, &result) == 0);
	assert(result.restart_calls == 1U);
	assert(result.releases == 1U);
}

static void
test_ordinary_cancel_retains_on_restart_failure(void)
{
	struct cancellation_result result = { 0 };

	assert(model_post_dequeue(0, ETIMEDOUT, &result) == ETIMEDOUT);
	assert(result.restart_calls == 1U);
	assert(result.releases == 0U);
}

static void
test_disconnect_releases_without_restart(void)
{
	struct cancellation_result result = { 0 };

	/* An injected restart failure is irrelevant because the disconnected
	 * endpoint is deliberately not rung after Set TR Dequeue succeeds. */
	assert(model_post_dequeue(1, ETIMEDOUT, &result) == 0);
	assert(result.restart_calls == 0U);
	assert(result.releases == 1U);
}

int
main(void)
{
	test_ordinary_cancel_restarts();
	test_ordinary_cancel_retains_on_restart_failure();
	test_disconnect_releases_without_restart();
	puts("xHCI HID hot-unplug lifecycle test: PASS");
	return 0;
}
