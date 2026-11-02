/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The xHCI Normal TD of a bulk OUT transfer that ends with a zero-length
 * packet, on the host (include/drivers/pci/pci-xhci-normal.h): when the
 * zero-length packet is wanted, the words of the payload TRBs and of the
 * zero-length TD, and how the Transfer Events of the two TDs end the request.
 *
 * The TD layout is also run through a model of the controller and of a
 * CDC-ECM function as QEMU emulates them: the controller moves each TD (the
 * TRBs up to one without Chain) as one USB packet and raises an event for a
 * TRB with IOC; the function ends a frame at a packet that is not a whole
 * number of its 64-byte packets, or is empty (QEMU's hw/usb/dev-network.c,
 * usb_net_handle_dataout).  A frame of 704 bytes must reach the network, and the old
 * layout (the zero-length TRB chained into the payload's TD) must not, which
 * is how the frame was lost.
 */

#include <drivers/pci/pci-xhci-normal.h>

#include <stdio.h>
#include <string.h>

#define MODEL_TRBS 8U
#define MODEL_PACKET 64U

/*
 * One TRB of the model ring: its buffer length and its control word.
 */
struct model_trb {
	uint32_t status;
	uint32_t control;
};

/*
 * A model of a ring holding one request, and of what the controller and
 * the function did with it: the frames the function sent on and the
 * events the controller raised.
 */
struct model {
	struct model_trb trbs[MODEL_TRBS];
	unsigned count;
	size_t function_buffered;
	size_t frames[MODEL_TRBS];
	unsigned frame_count;
	unsigned event_offsets[MODEL_TRBS];
	unsigned event_codes[MODEL_TRBS];
	unsigned event_count;
};

static unsigned failures;
static unsigned checks;

static void check(int good, const char *what);
static void build_current(struct model *model, size_t length, int zero_packet);
static void build_old(struct model *model, size_t length, int zero_packet);
static void run_controller(struct model *model);
static void check_zero_packet_decision(void);
static void check_words(void);
static void check_events(void);
static void check_model(void);

int
main(
	void)
{
	/* Runs every group of checks. */
	check_zero_packet_decision();
	check_words();
	check_events();
	check_model();

	/* Reports the outcome. */
	printf("xhci-zero-packet: %u checks, %u failed\n", checks, failures);
	if (failures != 0) {
		printf("xhci-zero-packet: FAIL\n");
		return 1;
	}

	/* Succeeded: every check passed. */
	printf("xhci-zero-packet: PASS\n");
	return 0;
}

/* Counts one check and reports it when it fails. */
static void
check(
	int good,
	const char *what)
{
	checks++;

	/* Reports a failed check by name. */
	if (!good) {
		failures++;
		printf("FAIL: %s\n", what);
	}
}

/* Builds a request's TRBs as pci-xhci.c's enqueue_normal does, below 64 KiB. */
static void
build_current(
	struct model *model,
	size_t length,
	int zero_packet)
{
	struct drv_xhci_normal_words words;

	/* The payload in one TRB, which ends its TD. */
	memset(model, 0, sizeof(*model));
	drv_xhci_normal_payload_words(length, length, length, MODEL_PACKET, 0, 1, &words);
	model->trbs[model->count].status = words.status;
	model->trbs[model->count].control = words.control;
	model->count++;

	/* The zero-length packet's own TD. */
	if (zero_packet) {
		drv_xhci_normal_zero_words(&words);
		model->trbs[model->count].status = words.status;
		model->trbs[model->count].control = words.control;
		model->count++;
	}
}

/* Builds a request's TRBs the way they were built before: the zero-length TRB chained into the payload's TD. */
static void
build_old(
	struct model *model,
	size_t length,
	int zero_packet)
{
	/* The payload, chained on to the zero-length TRB. */
	memset(model, 0, sizeof(*model));
	model->trbs[model->count].status = (uint32_t)length | (1U << DRV_XHCI_NORMAL_TD_SIZE_SHIFT);
	model->trbs[model->count].control = DRV_XHCI_NORMAL_TRB_TYPE | DRV_XHCI_NORMAL_TRB_CHAIN;
	if (!zero_packet) {
		model->trbs[model->count].status = (uint32_t)length;
		model->trbs[model->count].control = DRV_XHCI_NORMAL_TRB_TYPE | DRV_XHCI_NORMAL_TRB_IOC;
	}
	model->count++;

	/* The zero-length TRB that ended the same TD. */
	if (zero_packet) {
		model->trbs[model->count].status = 0;
		model->trbs[model->count].control = DRV_XHCI_NORMAL_TRB_TYPE | DRV_XHCI_NORMAL_TRB_IOC;
		model->count++;
	}
}

/*
 * Runs the ring through the model controller and function.
 *
 * Each TD is one OUT packet of all its TRBs' bytes; the function keeps the
 * bytes and sends a frame on at a packet that does not fill its last 64
 * bytes, or is empty.  An event
 * (Success) is raised for each TRB with IOC.
 */
static void
run_controller(
	struct model *model)
{
	size_t packet;
	unsigned index;

	/* Walks the TRBs, closing a TD at each one without Chain. */
	packet = 0;
	for (index = 0; index < model->count; index++) {
		packet += model->trbs[index].status & 0x1ffffU;
		if ((model->trbs[index].control & DRV_XHCI_NORMAL_TRB_IOC) != 0) {
			model->event_offsets[model->event_count] = index;
			model->event_codes[model->event_count] = DRV_XHCI_NORMAL_COMPLETION_SUCCESS;
			model->event_count++;
		}
		if ((model->trbs[index].control & DRV_XHCI_NORMAL_TRB_CHAIN) != 0)
			continue;

		/* The TD's packet reaches the function, which ends the frame at a short one. */
		model->function_buffered += packet;
		if (packet % MODEL_PACKET != 0 || packet == 0) {
			model->frames[model->frame_count] = model->function_buffered;
			model->frame_count++;
			model->function_buffered = 0;
		}
		packet = 0;
	}
}

/* Checks when a transfer asks for a zero-length TD. */
static void
check_zero_packet_decision(
	void)
{
	check(drv_xhci_normal_zero_packet(1, 0, 704, 1, 64) == 1, "bulk OUT 704 of 64 asked: zero-length TD");
	check(drv_xhci_normal_zero_packet(1, 0, 1024, 1, 512) == 1, "bulk OUT 1024 of 512 asked: zero-length TD");
	check(drv_xhci_normal_zero_packet(1, 0, 705, 1, 64) == 0, "bulk OUT 705: no zero-length TD");
	check(drv_xhci_normal_zero_packet(1, 0, 704, 0, 64) == 0, "bulk OUT 704 not asked: none");
	check(drv_xhci_normal_zero_packet(1, 1, 704, 1, 64) == 0, "bulk IN: none");
	check(drv_xhci_normal_zero_packet(0, 0, 704, 1, 64) == 0, "interrupt OUT: none");
	check(drv_xhci_normal_zero_packet(1, 0, 0, 1, 64) == 0, "empty transfer: none");
	check(drv_xhci_normal_zero_packet(1, 0, 704, 1, 0) == 0, "no packet size: none");
}

/* Checks the words of the payload TRBs and of the zero-length TD. */
static void
check_words(
	void)
{
	struct drv_xhci_normal_words words;

	/* The single payload TRB of an OUT TD ends it: IOC, no Chain, no ISP, TD Size 0. */
	drv_xhci_normal_payload_words(704, 704, 704, 64, 0, 1, &words);
	check(words.status == 704U, "last payload TRB: length 704, TD Size 0");
	check(words.control == (DRV_XHCI_NORMAL_TRB_TYPE | DRV_XHCI_NORMAL_TRB_IOC), "last payload TRB: Normal, IOC");

	/* The first of two payload TRBs chains and counts the packets after it. */
	drv_xhci_normal_payload_words(65536, 65536 + 640, 65536, 64, 0, 0, &words);
	check((words.status & 0x1ffffU) == 65536U, "first payload TRB: length 65536");
	check((words.status >> DRV_XHCI_NORMAL_TD_SIZE_SHIFT) == 10U, "first payload TRB: TD Size 10");
	check(words.control == (DRV_XHCI_NORMAL_TRB_TYPE | DRV_XHCI_NORMAL_TRB_CHAIN), "first payload TRB: Chain");

	/* TD Size is capped at 31. */
	drv_xhci_normal_payload_words(4096, 8192, 4096, 64, 0, 0, &words);
	check((words.status >> DRV_XHCI_NORMAL_TD_SIZE_SHIFT) == 31U, "TD Size capped at 31");

	/* An IN TRB asks for the short-packet event. */
	drv_xhci_normal_payload_words(512, 512, 512, 512, 1, 1, &words);
	check(words.control == (DRV_XHCI_NORMAL_TRB_TYPE | DRV_XHCI_NORMAL_TRB_ISP | DRV_XHCI_NORMAL_TRB_IOC),
	    "IN payload TRB: ISP and IOC");

	/* The zero-length TD is one TRB of length 0 with its own event. */
	drv_xhci_normal_zero_words(&words);
	check(words.status == 0U, "zero-length TD: length 0, TD Size 0");
	check(words.control == (DRV_XHCI_NORMAL_TRB_TYPE | DRV_XHCI_NORMAL_TRB_IOC), "zero-length TD: Normal, IOC");
}

/* Checks how the events of the two TDs end a request. */
static void
check_events(
	void)
{
	/* A request without a zero-length TD ends at any event. */
	check(drv_xhci_normal_event_kind(0, 0, 1, 1) == DRV_XHCI_NORMAL_EVENT_FINISH, "no zero-length TD: Success ends");
	check(drv_xhci_normal_event_kind(0, 0, 2, 1) == DRV_XHCI_NORMAL_EVENT_FINISH, "no zero-length TD, 2 TRBs: ends");

	/* The payload TD's event waits for the zero-length TD's when the endpoint goes on. */
	check(drv_xhci_normal_event_kind(1, 0, 2, 1) == DRV_XHCI_NORMAL_EVENT_WAIT_ZERO, "payload Success waits");
	check(drv_xhci_normal_event_kind(1, 1, 3, 1) == DRV_XHCI_NORMAL_EVENT_WAIT_ZERO, "second payload TRB waits");
	check(drv_xhci_normal_event_kind(1, 0, 2, 13) == DRV_XHCI_NORMAL_EVENT_WAIT_ZERO, "payload Short Packet waits");

	/* An error that halts the endpoint ends the request at the payload. */
	check(drv_xhci_normal_event_kind(1, 0, 2, 6) == DRV_XHCI_NORMAL_EVENT_FINISH, "payload Stall ends");
	check(drv_xhci_normal_event_kind(1, 0, 2, 4) == DRV_XHCI_NORMAL_EVENT_FINISH, "payload USB Transaction Error ends");
	check(drv_xhci_normal_event_kind(1, 0, 2, 3) == DRV_XHCI_NORMAL_EVENT_FINISH, "payload Babble ends");

	/* The zero-length TD's event ends the request, whatever its code. */
	check(drv_xhci_normal_event_kind(1, 1, 2, 1) == DRV_XHCI_NORMAL_EVENT_FINISH, "zero-length Success ends");
	check(drv_xhci_normal_event_kind(1, 1, 2, 13) == DRV_XHCI_NORMAL_EVENT_FINISH, "zero-length Short Packet ends");
	check(drv_xhci_normal_event_kind(1, 1, 2, 6) == DRV_XHCI_NORMAL_EVENT_FINISH, "zero-length Stall ends");

	/* Which TD decides the outcome. */
	check(drv_xhci_normal_payload_decides(0, 0) == 0, "no payload event: zero-length TD decides");
	check(drv_xhci_normal_payload_decides(1, 1) == 0, "payload Success: zero-length TD decides");
	check(drv_xhci_normal_payload_decides(1, 13) == 1, "payload Short Packet decides");
}

/* Checks that the TD layout delivers the frame through the model, and that the old layout lost it. */
static void
check_model(
	void)
{
	struct model model;
	enum drv_xhci_normal_event kind;
	unsigned index;
	int finished;

	/* A 704-byte frame with its zero-length TD reaches the network once. */
	build_current(&model, 704, 1);
	run_controller(&model);
	check(model.count == 2U, "704: two TRBs");
	check(model.frame_count == 1U && model.frames[0] == 704U, "704: one frame of 704 bytes sent on");
	check(model.function_buffered == 0U, "704: nothing left in the function");
	check(model.event_count == 2U, "704: an event for each TD");

	/* The events end the request at the zero-length TD's, with Success. */
	finished = 0;
	for (index = 0; index < model.event_count; index++) {
		kind = drv_xhci_normal_event_kind(1, model.event_offsets[index], model.count,
		    model.event_codes[index]);
		if (kind == DRV_XHCI_NORMAL_EVENT_FINISH) {
			check(index + 1U == model.event_count, "704: only the last event ends the request");
			check(drv_xhci_normal_payload_decides(1, model.event_codes[0]) == 0, "704: ends with Success");
			finished = 1;
		}
	}
	check(finished, "704: the request ends");

	/* A frame that is not a whole number of packets needs no zero-length TD. */
	build_current(&model, 705, 0);
	run_controller(&model);
	check(model.count == 1U && model.frame_count == 1U && model.frames[0] == 705U, "705: one TD, one frame");

	/* The old layout kept the 704 bytes in the function and sent no frame. */
	build_old(&model, 704, 1);
	run_controller(&model);
	check(model.frame_count == 0U && model.function_buffered == 704U, "old 704: the frame is held back");
}
