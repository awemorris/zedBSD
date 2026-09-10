/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdint.h>
#include <sys/types.h>
/* Keep host and target signal typedefs separate in this kernel-only fixture. */
#define sigset_t zedbsd_test_sigset_t
typedef uint32_t tid_t;
#include <assert.h>
#include <stdio.h>
#include "../../../src/drivers/pci/pci-xhci.c"

static unsigned frees;
void drv_dma_free_coherent(struct drv_dma_device *device, struct drv_dma_buffer *buffer)
{ (void)device; assert(buffer->address != NULL); frees++; memset(buffer, 0, sizeof(*buffer)); }

int main(void)
{
	struct xhci_controller controller;
	struct xhci_device device;
	struct xhci_endpoint *endpoint;
	struct xhci_request request;
	struct xhci_trb event;
	struct drv_xhci_endpoint_context_words words;
	uint32_t context[16];
	unsigned i, offset;
	uint64_t pointer;

	memset(&controller, 0, sizeof(controller)); memset(&device, 0, sizeof(device));
	memset(&request, 0, sizeof(request)); memset(&event, 0, sizeof(event));
	memset(&words, 0, sizeof(words));
	controller.devices = &device; controller.context_size = 64; device.slot = 1;
	endpoint = &device.endpoints[5]; endpoint->dci = 5;
	endpoint->ring.dma.device_address = 0x10000; endpoint->ring.cycle = 1;
	assert(xhci_stream_ring(endpoint, 0) == &endpoint->ring);
	assert(xhci_stream_ring(endpoint, 1) == NULL);
	fill_endpoint(&controller, context, endpoint, &words);
	assert(context[2] == 0x10001 && context[0] == 0);
	endpoint->maximum_stream_id = 3;
	endpoint->stream_contexts.device_address = 0x20000;
	endpoint->stream_contexts.address = (void *)1;
	assert(xhci_stream_ring(endpoint, 0) == NULL);
	assert(xhci_stream_ring(endpoint, 4) == NULL);
	for (i = 1; i <= 3; i++) {
		endpoint->streams[i - 1].dma.device_address = 0x30000 + i * 0x1000;
		endpoint->streams[i - 1].dma.address = (void *)(uintptr_t)i;
		assert(xhci_stream_ring(endpoint, i) == &endpoint->streams[i - 1]);
	}
	fill_endpoint(&controller, context, endpoint, &words);
	assert(context[0] == ((1U << 10) | (1U << 15)));
	assert(context[2] == 0x20000 && context[3] == 0);
	request.device = &device; request.endpoint = endpoint;
	request.slot = 1; request.dci = 5; request.first_trb = 2; request.trb_count = 2;
	request.stream_id = 2; request.ring = &endpoint->streams[1];
	endpoint->active = &request;
	event.control = (1U << 24) | (5U << 16) | XHCI_TRB_TYPE(32);
	for (i = 0; i <= 3; i++) {
		pointer = (i == 0 ? endpoint->ring.dma.device_address : endpoint->streams[i - 1].dma.device_address) + 2 * sizeof(struct xhci_trb);
		event.parameter_low = (uint32_t)pointer;
		assert((xhci_event_request_locked(&controller, &event, &offset) == &request) == (i == 2));
	}
	pointer = request.ring->dma.device_address + 3 * sizeof(struct xhci_trb);
	event.parameter_low = (uint32_t)pointer;
	assert(xhci_event_request_locked(&controller, &event, &offset) == &request && offset == 1);
	request.stream_id = 1;
	assert(xhci_event_request_locked(&controller, &event, &offset) == NULL);
	request.stream_id = 2;
	event.control ^= 1U << 24;
	assert(xhci_event_request_locked(&controller, &event, &offset) == NULL);
	endpoint->active = NULL; device.slot_disabled = 1;
	xhci_streams_free(&controller, endpoint);
	assert(frees == 4 && endpoint->maximum_stream_id == 0);
	assert(endpoint->stream_contexts.address == NULL);
	for (i = 0; i < 3; i++) assert(endpoint->streams[i].dma.address == NULL);
	puts("xHCI stream context, provenance and owned-array cleanup: PASS");
	return 0;
}
