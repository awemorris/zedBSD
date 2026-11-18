/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455: the bus thread, the receive path and control requests.
 *
 * One routine, service(), reads the SDIO core's interrupt status, answers
 * the firmware's mailbox and reads every pending frame; the bus thread
 * runs it after each interrupt and every 100 ms, and a direct-mode
 * request runs it while it waits for its answer.  Frames go out only
 * within the credit the firmware grants in every frame it sends.
 *
 * Every buffer a request or an answer passes through (the slot, the frame
 * buffers) is cleared once its bytes have moved on, so a key that a
 * request carries does not stay in memory.
 *
 */

#include "drivers/wifi/bcm43455/bcm43455-bus.h"
#include "drivers/wifi/bcm43455/bcm43455-bytes.h"
#include "drivers/wifi/bcm43455/bcm43455-hw.h"
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>

/* How much of a frame is read before its length is known: its SDPCM header. */
#define FIRST_READ			BCM43455_SDPCM_HEADER_LENGTH

/*
 * The most frames one pass of the receive path reads before it lets sends in.
 */
#define FRAMES_PER_SERVICE		32U

/* How long a direct-mode request sleeps between looks at the chip. */
#define DIRECT_STEP_MS			10U

/*
 * The firmware's error code for a command or variable it does not have.
 *
 * Value referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/inc/whd_types.h (lines 1066 and 1090, the offset of 2000
 * left out).
 */
#define FIRMWARE_UNSUPPORTED		(-23)

/*
 * The first sequence number and the credit before the firmware's first frame: a
 * window of one frame.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.c revision 1.44 (line
 * 268).
 */
#define INITIAL_SEQUENCE		0xffU
#define INITIAL_CREDIT			0U

static void bus_thread(void *argument);
static int service(struct bcm43455_bus *bus);
static int handle_mailbox(struct bcm43455_bus *bus);
static int receive_frame(struct bcm43455_bus *bus);
static int read_fifo(struct bcm43455_bus *bus, uint8_t *buffer, size_t length);
static int discard_rest(struct bcm43455_bus *bus, size_t length);
static int abort_frame(struct bcm43455_bus *bus);
static void dispatch(struct bcm43455_bus *bus, const struct bcm43455_sdpcm_fields *header);
static void deliver_answer(struct bcm43455_bus *bus, const uint8_t *payload, size_t length);
static int answer_error(const struct bcm43455_bcdc_answer *answer);
static int send_frame(struct bcm43455_bus *bus, unsigned channel, size_t length);
static int send_pending(struct bcm43455_bus *bus);
static int send_slot(struct bcm43455_bus *bus);
static int send_data(struct bcm43455_bus *bus);
static size_t build_control(struct bcm43455_bus *bus, uint32_t command, int set, uint16_t id, const uint8_t *payload, size_t payload_length);
static int direct_control(struct bcm43455_bus *bus, uint32_t command, int set, const uint8_t *input, size_t input_length, size_t request_length, uint8_t *answer, size_t answer_capacity, size_t *answer_length);
static int own_request(struct bcm43455_bus *bus, uint32_t command, int set, const uint8_t *input, size_t input_length, size_t request_length, uint8_t *answer, size_t answer_capacity, size_t *answer_length, int *outcome);
static int slot_control(struct bcm43455_bus *bus, uint32_t command, int set, const uint8_t *input, size_t input_length, size_t request_length, uint8_t *answer, size_t answer_capacity, size_t *answer_length);
static int direct_wait(struct bcm43455_bus *bus, uint64_t deadline, int *outcome);
static uint16_t take_id_locked(struct bcm43455_bus *bus);
static uint64_t deadline_after(unsigned milliseconds);

/*
 * Prepares a bus whose chip is attached later.
 */
int
bcm43455_bus_init(
	struct bcm43455_bus *bus,
	const struct bcm43455_bus_hooks *hooks,
	void *hook_context)
{
	int error;

	/* Refuses a bus without hooks. */
	if (bus == NULL || hooks == NULL)
		return EINVAL;

	/* Starts from a bus that knows nothing. */
	kern_memset(bus, 0, sizeof(*bus));
	bus->hooks = hooks;
	bus->hook_context = hook_context;
	bus->next_id = 1;

	/*
	 * Prepares the locks and the queues requesters and stoppers sleep on.
	 */
	error = mutex_init(&bus->control_lock, LOCK_RANK_DEVICE, "bcm43455 control");
	if (error != 0)
		return error;
	spin_init(&bus->lock, LOCK_RANK_DEVICE, "bcm43455 bus");
	waitq_init(&bus->answer_wait, "bcm43455 answer");
	waitq_init(&bus->finished_wait, "bcm43455 bus finished");

	/* Succeeded: the bus waits for its chip. */
	return 0;
}

/*
 * Forgets the SDPCM state of a previous firmware run.
 */
void
bcm43455_bus_forget(
	struct bcm43455_bus *bus)
{
	/* A new firmware is sent one frame before it grants more. */
	bus->tx_sequence = (uint8_t)INITIAL_SEQUENCE;
	bus->tx_credit_limit = (uint8_t)INITIAL_CREDIT;
	bus->flow_control = 0;
	bus->firmware_protocol = 0;
	kern_memset(&bus->direct, 0, sizeof(bus->direct));
	kern_memset(bus->rx_frame, 0, sizeof(bus->rx_frame));
	kern_memset(bus->tx_frame, 0, sizeof(bus->tx_frame));
}

/*
 * Allows or forbids direct mode.
 */
void
bcm43455_bus_direct(
	struct bcm43455_bus *bus,
	int allowed)
{
	/*
	 * Waits out a request in flight, so the change takes effect between
	 * requests.
	 */
	mutex_lock(&bus->control_lock);

	bus->direct_allowed = 0;
	if (allowed)
		bus->direct_allowed = 1;

	mutex_unlock(&bus->control_lock);
}

/*
 * Sends one control command and waits for its answer.
 */
int
bcm43455_bus_control(
	struct bcm43455_bus *bus,
	uint32_t command,
	int set,
	const uint8_t *input,
	size_t input_length,
	uint8_t *answer,
	size_t answer_capacity,
	size_t *answer_length)
{
	unsigned long enabled;
	size_t request_length;
	unsigned running;
	int error;

	/* Refuses a request that does not fit a message. */
	if (input == NULL && input_length != 0)
		return EINVAL;
	if (answer == NULL && answer_capacity != 0)
		return EINVAL;
	if (input_length > BCM43455_BCDC_CONTROL_MAX || answer_capacity > BCM43455_BCDC_CONTROL_MAX)
		return EINVAL;

	/*
	 * A get sends as many bytes as it wants back, its input at their start.
	 */
	request_length = input_length;
	if (!set && answer_capacity > request_length)
		request_length = answer_capacity;

	/* Admits one request at a time, held while it waits. */
	mutex_lock(&bus->control_lock);

	/*
	 * Hands the request to the thread when it runs, else runs it directly
	 * if allowed.
	 */
	enabled = spin_lock_irqsave(&bus->lock);

	running = bus->thread_running;

	spin_unlock_irqrestore(&bus->lock, enabled);

	/* Sends the request the way the bus is served now. */
	if (running) {
		error = slot_control(bus, command, set, input, input_length, request_length, answer, answer_capacity, answer_length);
	} else if (bus->direct_allowed) {
		error = direct_control(bus, command, set, input, input_length, request_length, answer, answer_capacity, answer_length);
	} else {
		/*
		 * Neither the thread nor the lifecycle owner holds the bus: it
		 * is down.
		 */
		error = ENETDOWN;
	}

	mutex_unlock(&bus->control_lock);

	/* Reports why the command had no answer. */
	if (error != 0)
		return error;

	/* Succeeded: the firmware answered. */
	return 0;
}

/*
 * Reads an iovar.
 */
int
bcm43455_bus_iovar_get(
	struct bcm43455_bus *bus,
	const char *name,
	uint8_t *answer,
	size_t capacity,
	size_t *answer_length)
{
	uint8_t request[BCM43455_BCDC_CONTROL_MAX];
	size_t length;
	int error;

	/* The request is the variable's name. */
	error = bcm43455_bcdc_iovar(request, sizeof(request), name, NULL, 0, &length);
	if (error != 0)
		return error;

	/* Asks for the value. */
	error = bcm43455_bus_control(bus, BCM43455_COMMAND_READ_VARIABLE, 0, request, length, answer, capacity, answer_length);
	if (error != 0)
		return error;

	/* Succeeded: answer holds the value. */
	return 0;
}

/*
 * Sets an iovar.
 */
int
bcm43455_bus_iovar_set(
	struct bcm43455_bus *bus,
	const char *name,
	const void *value,
	size_t length)
{
	uint8_t request[BCM43455_BCDC_CONTROL_MAX];
	size_t request_length;
	int error;

	/* The request is the variable's name and its new value. */
	error = bcm43455_bcdc_iovar(request, sizeof(request), name, value, length, &request_length);
	if (error != 0)
		return error;

	/* Sets it, then clears the copy, since a value may be a key. */
	error = bcm43455_bus_control(bus, BCM43455_COMMAND_WRITE_VARIABLE, 1, request, request_length, NULL, 0, NULL);
	kern_memset(request, 0, request_length);
	if (error != 0)
		return error;

	/* Succeeded: the firmware took the value. */
	return 0;
}

/*
 * Sets an iovar that holds one 32-bit number.
 */
int
bcm43455_bus_iovar_set_u32(
	struct bcm43455_bus *bus,
	const char *name,
	uint32_t value)
{
	uint8_t bytes[4];
	int error;

	/* The firmware's numbers are little-endian. */
	bcm43455_put_le32(bytes, value);
	error = bcm43455_bus_iovar_set(bus, name, bytes, sizeof(bytes));
	if (error != 0)
		return error;

	/* Succeeded: the firmware took the number. */
	return 0;
}

/*
 * Starts the bus thread.
 */
int
bcm43455_bus_thread_start(
	struct bcm43455_bus *bus)
{
	struct thread *thread;
	unsigned long enabled;
	int error;

	/* Takes the bus from direct mode between requests. */
	mutex_lock(&bus->control_lock);

	/* Makes the thread before anything shows it as running. */
	error = kthread_create(bus_thread, bus, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		mutex_unlock(&bus->control_lock);
		kern_logf("bcm43455: bus thread not created (%d)\n", error);
		return error;
	}

	/*
	 * From here requests go to the thread, and only the thread uses the
	 * bus.
	 */
	enabled = spin_lock_irqsave(&bus->lock);

	bus->thread_running = 1;
	bus->stop_requested = 0;
	bus->work_requested = 0;
	bus->radio_work_pending = 0;
	bus->thread_error = 0;
	kern_memset(&bus->slot, 0, sizeof(bus->slot));

	spin_unlock_irqrestore(&bus->lock, enabled);

	/*
	 * Starts it; nobody joins it, bcm43455_bus_thread_stop() waits for its
	 * flag instead.
	 */
	thread->detached = 1;
	thread_start(thread);

	mutex_unlock(&bus->control_lock);

	/* Succeeded: the thread serves the bus. */
	return 0;
}

/*
 * Stops the bus thread and waits until it has let the bus go.
 */
int
bcm43455_bus_thread_stop(
	struct bcm43455_bus *bus)
{
	unsigned long enabled;
	uint64_t observed;
	unsigned running;
	int error;

	/*
	 * Asks the thread to stop; a thread that already stopped, or never ran,
	 * only reports why.
	 */
	enabled = spin_lock_irqsave(&bus->lock);

	running = bus->thread_running;
	if (running)
		bus->stop_requested = 1;

	spin_unlock_irqrestore(&bus->lock, enabled);

	/* Wakes a running thread from its wait for the card. */
	if (running)
		bus->chip.ops->interrupt_kick(bus->chip.context);

	/* Waits until it has given the bus back. */
	enabled = spin_lock_irqsave(&bus->lock);

	while (bus->thread_running) {
		observed = waitq_sequence(&bus->finished_wait);
		(void)waitq_sleep(&bus->finished_wait, &bus->lock, observed, 0, 0);
	}

	/* Takes the error the thread stopped on. */
	error = bus->thread_error;

	spin_unlock_irqrestore(&bus->lock, enabled);

	/* Reports the error the thread stopped on. */
	if (error != 0)
		return error;

	/* Succeeded: the bus is the owner's again. */
	return 0;
}

/*
 * Tells the bus thread that the device has a frame to send.
 */
void
bcm43455_bus_wake(
	struct bcm43455_bus *bus)
{
	unsigned long enabled;
	unsigned running;

	/* work_requested makes the thread pull frames before it waits again. */
	enabled = spin_lock_irqsave(&bus->lock);

	bus->work_requested = 1;
	running = bus->thread_running;

	spin_unlock_irqrestore(&bus->lock, enabled);

	/* Wakes the thread from its wait for the card. */
	if (running)
		bus->chip.ops->interrupt_kick(bus->chip.context);
}

/*
 * Asks the bus thread to run the work hook.
 */
void
bcm43455_bus_request_work(
	struct bcm43455_bus *bus)
{
	unsigned long enabled;
	unsigned running;

	/*
	 * radio_work_pending makes the thread run the hook before it waits
	 * again; the thread clears it before the run, so a request made
	 * during the run makes another.
	 */
	enabled = spin_lock_irqsave(&bus->lock);

	bus->radio_work_pending = 1;
	running = bus->thread_running;

	spin_unlock_irqrestore(&bus->lock, enabled);

	/* Wakes the thread from its wait for the card. */
	if (running)
		bus->chip.ops->interrupt_kick(bus->chip.context);
}

/*
 * Sends one control command from the work hook and waits for its answer.
 */
int
bcm43455_bus_thread_control(
	struct bcm43455_bus *bus,
	uint32_t command,
	int set,
	const uint8_t *input,
	size_t input_length,
	uint8_t *answer,
	size_t answer_capacity,
	size_t *answer_length,
	int *outcome)
{
	size_t request_length;
	int error;

	/* Refuses a request that does not fit a message. */
	if (outcome == NULL)
		return EINVAL;
	*outcome = EINVAL;
	if (input == NULL && input_length != 0)
		return 0;
	if (answer == NULL && answer_capacity != 0)
		return 0;
	if (input_length > BCM43455_BCDC_CONTROL_MAX || answer_capacity > BCM43455_BCDC_CONTROL_MAX)
		return 0;

	/*
	 * A get sends as many bytes as it wants back, its input at their start.
	 */
	request_length = input_length;
	if (!set && answer_capacity > request_length)
		request_length = answer_capacity;

	/*
	 * The thread owns the bus: it sends the command and reads until the
	 * answer comes.
	 */
	error = own_request(bus, command, set, input, input_length, request_length, answer, answer_capacity, answer_length, outcome);
	if (error != 0)
		return error;

	/* Succeeded: the bus still works; outcome says how the command went. */
	return 0;
}

/*
 * The bus thread: serves the chip until asked to stop or until the bus
 * fails.
 */
static void
bus_thread(
	void *argument)
{
	struct bcm43455_bus *bus;
	unsigned long enabled;
	uint64_t poll_deadline;
	unsigned stop;
	unsigned work;
	unsigned radio_work;
	unsigned window;
	int waited;
	int error;

	/* The argument is the bus the thread serves; it has not failed yet. */
	bus = argument;
	error = 0;

	/* Serves the chip one wakeup at a time. */
	for (;;) {
		/*
		 * Samples the stop request, whether anything waits to be sent
		 * within the credit, and takes the request for radio work.
		 */
		window = bcm43455_sdpcm_window(bus->tx_credit_limit, bus->tx_sequence);
		enabled = spin_lock_irqsave(&bus->lock);

		stop = bus->stop_requested;

		/*
		 * Only a credit lets a queued request or a waiting frame count
		 * as work.
		 */
		work = 0;
		if (window != 0) {
			if (bus->slot.state == BCM43455_SLOT_QUEUED)
				work = 1;
			if (bus->work_requested)
				work = 1;
		}

		/*
		 * Takes the radio work, which counts as work with or without a
		 * credit.
		 */
		radio_work = bus->radio_work_pending;
		bus->radio_work_pending = 0;
		if (radio_work)
			work = 1;

		spin_unlock_irqrestore(&bus->lock, enabled);

		/* Ends on request. */
		if (stop)
			break;

		/*
		 * Without work, sleeps until the card interrupts, a kick comes,
		 * or the poll is due.
		 */
		waited = EAGAIN;
		if (!work) {
			poll_deadline = deadline_after(BCM43455_BUS_POLL_MS);
			waited = bus->chip.ops->interrupt_wait(bus->chip.context, poll_deadline);
			if (waited == 0)
				bus->counters.interrupts++;
			else if (waited == ETIMEDOUT)
				bus->counters.polls++;
		}

		/*
		 * Reads what the chip has, then lets its interrupt in again if
		 * it fired.
		 */
		error = service(bus);
		if (waited == 0)
			bus->chip.ops->interrupt_rearm(bus->chip.context);
		if (error != 0)
			break;

		/*
		 * Does the device's radio work, which sends its own commands;
		 * a request queued in the slot meanwhile goes out after it.
		 */
		if (radio_work && bus->hooks->work != NULL) {
			error = bus->hooks->work(bus->hook_context);
			if (error != 0)
				break;
		}

		/*
		 * Sends the request and the frames that wait, within the
		 * credit.
		 */
		error = send_pending(bus);
		if (error != 0)
			break;
	}

	/*
	 * Tells the device that its bus failed, so it can report the link lost.
	 */
	if (error != 0) {
		kern_logf("bcm43455: bus thread stopped on error %d\n", error);
		if (bus->hooks->failed != NULL)
			bus->hooks->failed(bus->hook_context, error);
	}

	/*
	 * Gives the bus back.  A request still in the slot is answered with
	 * ENETDOWN, so its requester does not wait out its timeout.
	 */
	enabled = spin_lock_irqsave(&bus->lock);

	if (bus->slot.state == BCM43455_SLOT_QUEUED || bus->slot.state == BCM43455_SLOT_SENT) {
		kern_memset(bus->slot.request, 0, sizeof(bus->slot.request));
		bus->slot.error = ENETDOWN;
		bus->slot.answer_length = 0;
		bus->slot.state = BCM43455_SLOT_ANSWERED;
		waitq_wake_all(&bus->answer_wait);
	} else if (bus->slot.state == BCM43455_SLOT_ABANDONED) {
		bus->slot.state = BCM43455_SLOT_EMPTY;
	}

	/* Leaves the error for the stopper and tells it the thread is gone. */
	bus->thread_error = error;
	bus->thread_running = 0;
	waitq_wake_all(&bus->finished_wait);

	spin_unlock_irqrestore(&bus->lock, enabled);
}

/*
 * Reads the SDIO core's interrupt status, answers the mailbox and reads the
 * pending frames.  Returns an error only for a bus or firmware failure that
 * ends the bus.
 *
 * The frames are read on every pass, with or without the frame indication: a
 * missed indication must not leave a frame waiting.  Reading when there is none
 * costs one header's read.
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.c revision 1.44
 * (bwfm_sdio_task(), lines 699-745).
 */
static int
service(
	struct bcm43455_bus *bus)
{
	uint32_t status;
	unsigned frames;
	int error;

	/*
	 * Reads what the SDIO core has to say: the to-host mailbox's bits and
	 * chip activity.
	 */
	error = bcm43455_chip_read32(&bus->chip, bus->chip.sdio.base + BCM43455_SDIO_CORE_INTERRUPTS, &status);
	if (error != 0)
		return error;
	status &= BCM43455_INT_MAILBOX_MASK | BCM43455_INT_CHIP_ACTIVE;

	/*
	 * Clears the bits it raised before acting on them, so a new one is not
	 * lost.
	 */
	if (status != 0) {
		error = bcm43455_chip_write32(&bus->chip, bus->chip.sdio.base + BCM43455_SDIO_CORE_INTERRUPTS, status);
		if (error != 0)
			return error;
	}

	/* Answers the firmware's mailbox. */
	if ((status & BCM43455_INT_HOST_MAILBOX) != 0) {
		error = handle_mailbox(bus);
		if (error != 0)
			return error;
	}

	/*
	 * Reads frames until the chip has none, a bad one stops the pass, or
	 * the pass is long enough.
	 */
	for (frames = 0; frames < FRAMES_PER_SERVICE; frames++) {
		error = receive_frame(bus);
		if (error == ENOENT)
			break;
		if (error == EAGAIN)
			break;
		if (error != 0)
			return error;
	}

	/* Succeeded: the chip's frames are read. */
	return 0;
}

/*
 * Reads the firmware's mailbox data, acknowledges it and acts on it: the
 * protocol version, a halt, and its flow control.
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.c revision 1.44
 * (bwfm_sdio_task(), lines 699-745).
 */
static int
handle_mailbox(
	struct bcm43455_bus *bus)
{
	uint32_t data;
	unsigned version;
	int error;

	/*
	 * Reads the data and acknowledges it, which frees the mailbox for the
	 * next.
	 */
	error = bcm43455_chip_read32(&bus->chip, bus->chip.sdio.base + BCM43455_SDIO_TO_HOST_DATA, &data);
	if (error != 0)
		return error;
	error = bcm43455_chip_write32(&bus->chip, bus->chip.sdio.base + BCM43455_SDIO_TO_CHIP_MAILBOX, BCM43455_TO_CHIP_ACK);
	if (error != 0)
		return error;

	/*
	 * Records the protocol version the firmware announces, and refuses
	 * another one.
	 */
	version = (data >> BCM43455_TO_HOST_VERSION_SHIFT) & BCM43455_TO_HOST_VERSION_MASK;
	if (version != 0 && bus->firmware_protocol == 0) {
		bus->firmware_protocol = version;
		kern_logf("bcm43455: firmware speaks SDPCM protocol %u (mailbox %08x)\n", version, data);
		if (version != BCM43455_SDPCM_PROTOCOL)
			return EPROTO;
	}

	/*
	 * A halted firmware ends the bus: it serves nothing more until
	 * reloaded.
	 */
	if ((data & BCM43455_TO_HOST_FIRMWARE_HALT) != 0) {
		kern_logf("bcm43455: firmware halted (mailbox %08x)\n", data);
		return EIO;
	}

	/* Takes the firmware's per-priority flow control. */
	if ((data & BCM43455_TO_HOST_FLOW_CONTROL) != 0)
		bus->flow_control = (uint8_t)(data >> BCM43455_TO_HOST_FLOW_SHIFT);

	/* Succeeded: the mailbox is answered. */
	return 0;
}

/*
 * Reads one frame and hands it on: the twelve-byte header first, then the rest
 * in four-byte units.  Returns ENOENT when the chip has none, EAGAIN when a bad
 * frame was dropped and this pass should end, and a bus error otherwise.
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.c revision 1.44
 * (bwfm_sdio_rx_frames(), lines 1197-1260).
 */
static int
receive_frame(
	struct bcm43455_bus *bus)
{
	struct bcm43455_sdpcm_fields header;
	size_t rest;
	size_t used;
	int error;

	/* Reads the frame's header. */
	error = bcm43455_chip_frame_window(&bus->chip);
	if (error != 0)
		return error;
	error = bus->chip.ops->read(bus->chip.context, BCM43455_FUNCTION_FRAMES, BCM43455_FRAME_ADDRESS, 0, bus->rx_frame, FIRST_READ);
	if (error != 0)
		return error;

	/* Decodes it; an empty tag means no frame, a garbled one is aborted. */
	error = bcm43455_sdpcm_decode(bus->rx_frame, FIRST_READ, &header);
	if (error == ENOENT) {
		kern_memset(bus->rx_frame, 0, FIRST_READ);
		return ENOENT;
	}

	/* Aborts a garbled frame, so the next read starts at a frame. */
	if (error != 0) {
		kern_memset(bus->rx_frame, 0, FIRST_READ);
		bus->counters.bad_frames++;
		error = abort_frame(bus);
		if (error != 0)
			return error;
		return EAGAIN;
	}

	/* Every frame carries the firmware's latest credit and flow control. */
	bus->tx_credit_limit = header.max_sequence;
	bus->flow_control = header.flow_control;

	/* A frame of only a header carries nothing but the credit. */
	if (header.length == FIRST_READ) {
		kern_memset(bus->rx_frame, 0, FIRST_READ);
		return 0;
	}

	/*
	 * Reads the rest in four-byte units, or drops a frame too long to keep.
	 */
	rest = (size_t)header.length - FIRST_READ;
	rest = (rest + 3U) & ~(size_t)3U;
	if (FIRST_READ + rest > sizeof(bus->rx_frame)) {
		kern_memset(bus->rx_frame, 0, FIRST_READ);
		bus->counters.dropped++;
		error = discard_rest(bus, rest);
		if (error != 0)
			return error;
		return 0;
	}

	/* Reads the rest behind the header. */
	error = read_fifo(bus, bus->rx_frame + FIRST_READ, rest);
	if (error != 0) {
		kern_memset(bus->rx_frame, 0, FIRST_READ + rest);
		return error;
	}

	/* The frame takes the header and the rest. */
	used = FIRST_READ + rest;

	/* Hands the frame to its channel's consumer, then clears it. */
	bus->counters.frames_in++;
	dispatch(bus, &header);
	kern_memset(bus->rx_frame, 0, used);

	/* Succeeded: one frame was read. */
	return 0;
}

/*
 * Reads length bytes (a multiple of four) from the frame function's FIFO:
 * whole blocks first, then the bytes left over.
 */
static int
read_fifo(
	struct bcm43455_bus *bus,
	uint8_t *buffer,
	size_t length)
{
	size_t blocks;
	unsigned block;
	int error;

	/*
	 * A run longer than a block goes as whole blocks, then a byte-mode
	 * tail.
	 */
	block = bus->chip.ops->block_size(bus->chip.context, BCM43455_FUNCTION_FRAMES);
	blocks = 0;
	if (block != 0 && length > block)
		blocks = length - length % block;

	/* The whole blocks. */
	if (blocks != 0) {
		error = bus->chip.ops->read(bus->chip.context, BCM43455_FUNCTION_FRAMES, BCM43455_FRAME_ADDRESS, 0, buffer, blocks);
		if (error != 0)
			return error;
	}

	/* The bytes left over. */
	if (length > blocks) {
		error = bus->chip.ops->read(
			bus->chip.context,
			BCM43455_FUNCTION_FRAMES,
			BCM43455_FRAME_ADDRESS,
			0,
			buffer + blocks,
			length - blocks);
		if (error != 0)
			return error;
	}

	/* Succeeded: buffer holds the bytes. */
	return 0;
}

/*
 * Reads and throws away the rest of a frame too long for the receive
 * buffer, a buffer's worth at a time.
 */
static int
discard_rest(
	struct bcm43455_bus *bus,
	size_t length)
{
	size_t piece;
	size_t room;
	int error;

	/* Reads into the buffer past the header, as much as fits each time. */
	room = sizeof(bus->rx_frame) - FIRST_READ;
	room &= ~(size_t)3U;
	while (length != 0) {
		/* Takes as much as fits. */
		piece = length;
		if (piece > room)
			piece = room;

		/* Reads it and forgets it. */
		error = read_fifo(bus, bus->rx_frame + FIRST_READ, piece);
		kern_memset(bus->rx_frame + FIRST_READ, 0, piece);
		if (error != 0)
			return error;
		length -= piece;
	}

	/* Succeeded: the frame is gone. */
	return 0;
}

/*
 * Aborts the frame being read, tells the chip to drop it, and tells the
 * firmware that the host refused it, so the next read starts at the next frame.
 *
 * Procedure referenced: Infineon wifi-host-driver bd99da7b
 * whd_bus_sdio_protocol.c (whd_bus_sdio_abort_read(), lines 1317-1336).
 */
static int
abort_frame(
	struct bcm43455_bus *bus)
{
	int error;

	/* Aborts the frame function's transfer. */
	error = bus->chip.ops->write_byte(bus->chip.context, BCM43455_FUNCTION_COMMON, BCM43455_CCCR_IO_ABORT, BCM43455_FUNCTION_FRAMES);
	if (error != 0)
		return error;

	/* Terminates the frame in the read FIFO. */
	error = bus->chip.ops->write_byte(
		bus->chip.context,
		BCM43455_FUNCTION_BACKPLANE,
		BCM43455_F1_FRAME_CONTROL,
		BCM43455_FRAME_CONTROL_READ_TERMINATE);
	if (error != 0)
		return error;

	/* Tells the firmware the frame was refused. */
	error = bcm43455_chip_write32(&bus->chip, bus->chip.sdio.base + BCM43455_SDIO_TO_CHIP_MAILBOX, BCM43455_TO_CHIP_NAK);
	if (error != 0)
		return error;

	/* Succeeded: the next read starts afresh. */
	return 0;
}

/*
 * Hands a received frame's payload to whoever its channel belongs to.
 */
static void
dispatch(
	struct bcm43455_bus *bus,
	const struct bcm43455_sdpcm_fields *header)
{
	struct bcm43455_bcdc_data data;
	struct bcm43455_event event;
	const uint8_t *payload;
	const uint8_t *frame;
	size_t length;
	int error;

	/* The payload starts where the header says. */
	payload = bus->rx_frame + header->payload_start;
	length = (size_t)header->length - header->payload_start;

	/* Each channel has its consumer. */
	switch (header->channel) {
	case BCM43455_SDPCM_CHANNEL_CONTROL:
		/* A control answer goes to the request it answers. */
		bus->counters.control_in++;
		deliver_answer(bus, payload, length);
		break;

	case BCM43455_SDPCM_CHANNEL_EVENT:
		/* An event is an Ethernet frame behind a BCDC data header. */
		error = bcm43455_bcdc_decode_data(payload, length, &data);
		if (error != 0) {
			bus->counters.bad_frames++;
			break;
		}

		/* Decodes the event the Ethernet frame carries. */
		frame = payload + data.frame_offset;
		error = bcm43455_event_decode(frame, data.frame_length, &event);
		if (error != 0) {
			bus->counters.bad_frames++;
			break;
		}

		/* Hands the event to the device. */
		bus->counters.events_in++;
		bus->hooks->event(bus->hook_context, &event, frame, data.frame_length);
		break;

	case BCM43455_SDPCM_CHANNEL_DATA:
		/*
		 * A data frame is an Ethernet frame behind a BCDC data header.
		 */
		error = bcm43455_bcdc_decode_data(payload, length, &data);
		if (error != 0) {
			bus->counters.bad_frames++;
			break;
		}

		/* Hands the Ethernet frame to the device. */
		bus->counters.data_in++;
		bus->hooks->data(bus->hook_context, payload + data.frame_offset, data.frame_length);
		break;

	case BCM43455_SDPCM_CHANNEL_GLOM:
		/*
		 * A glommed frame should not come once receive glomming is
		 * turned off; the first one is logged, since its frames are lost.
		 */
		bus->counters.dropped++;
		bus->counters.glom_frames++;
		if (bus->counters.glom_frames == 1)
			kern_logf("bcm43455: glommed frame dropped; the firmware still gloms received frames\n");
		break;

	default:
		/* A frame of an unknown channel is dropped. */
		bus->counters.dropped++;
		break;
	}
}

/*
 * Gives a control answer to the request it answers: the direct-mode
 * request, or the one in the slot.  An answer to nobody (a request that
 * gave up) is dropped.
 */
static void
deliver_answer(
	struct bcm43455_bus *bus,
	const uint8_t *payload,
	size_t length)
{
	struct bcm43455_bcdc_answer answer;
	unsigned long enabled;
	size_t copied;
	int error;

	/* Decodes the header; a garbled one answers nobody. */
	error = bcm43455_bcdc_decode_control(payload, length, &answer);
	if (error != 0) {
		bus->counters.bad_frames++;
		return;
	}

	/* The direct-mode request in flight takes its own answer. */
	if (bus->direct.active &&
	    !bus->direct.done &&
	    answer.id == bus->direct.id &&
	    answer.command == bus->direct.command) {
		copied = answer.payload_length;
		if (copied > bus->direct.answer_capacity)
			copied = bus->direct.answer_capacity;
		if (copied != 0)
			kern_memcpy(bus->direct.answer, payload + answer.payload_offset, copied);
		bus->direct.answer_length = copied;
		bus->direct.error = answer_error(&answer);
		bus->direct.done = 1;
		return;
	}

	/*
	 * Otherwise the answer belongs to the slot's request, if that is the
	 * one sent.
	 */
	enabled = spin_lock_irqsave(&bus->lock);

	if (bus->slot.state == BCM43455_SLOT_SENT &&
	    bus->slot.id == answer.id &&
	    bus->slot.command == answer.command) {
		copied = answer.payload_length;
		if (copied > bus->slot.answer_capacity)
			copied = bus->slot.answer_capacity;
		if (copied != 0)
			kern_memcpy(bus->slot.answer, payload + answer.payload_offset, copied);
		bus->slot.answer_length = copied;
		bus->slot.error = answer_error(&answer);
		bus->slot.state = BCM43455_SLOT_ANSWERED;
		waitq_wake_all(&bus->answer_wait);
	} else if (bus->slot.state == BCM43455_SLOT_ABANDONED && bus->slot.id == answer.id) {
		/* The late answer frees the slot its requester gave up on. */
		bus->slot.state = BCM43455_SLOT_EMPTY;
		bus->counters.stale_answers++;
	} else {
		bus->counters.stale_answers++;
	}

	spin_unlock_irqrestore(&bus->lock, enabled);
}

/* Turns a failed answer into an errno, logging the firmware's own code. */
static int
answer_error(
	const struct bcm43455_bcdc_answer *answer)
{
	/* An answer without the error flag succeeded. */
	if (!answer->failed)
		return 0;

	/* A command or variable the firmware does not have. */
	if (answer->status == FIRMWARE_UNSUPPORTED)
		return EOPNOTSUPP;

	/* Any other refusal is an I/O error, logged with the firmware's status. */
	kern_logf("bcm43455: firmware refused command %u (status %d)\n", answer->command, (int)answer->status);
	return EIO;
}

/*
 * Sends the frame in tx_frame: writes its SDPCM header, pads it to the bus's
 * unit, writes it to the frame function and clears it.
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.c revision 1.44
 * (bwfm_sdio_tx_ctrlframe()).
 */
static int
send_frame(
	struct bcm43455_bus *bus,
	unsigned channel,
	size_t length)
{
	size_t padded;
	unsigned block;
	int error;

	/* Pads the frame to what the bus moves. */
	block = bus->chip.ops->block_size(bus->chip.context, BCM43455_FUNCTION_FRAMES);
	padded = bcm43455_sdpcm_padded_length(length, block);
	if (padded > sizeof(bus->tx_frame)) {
		kern_memset(bus->tx_frame, 0, sizeof(bus->tx_frame));
		return EINVAL;
	}

	/* Clears the padding behind the frame. */
	kern_memset(bus->tx_frame + length, 0, padded - length);

	/* Stamps the header with the next sequence number. */
	bcm43455_sdpcm_encode(bus->tx_frame, (uint16_t)length, bus->tx_sequence, (uint8_t)channel);

	/* Writes the frame, then clears it whatever came of the write. */
	error = bcm43455_chip_frame_window(&bus->chip);
	if (error == 0)
		error = bus->chip.ops->write(bus->chip.context, BCM43455_FUNCTION_FRAMES, BCM43455_FRAME_ADDRESS, 0, bus->tx_frame, padded);
	kern_memset(bus->tx_frame, 0, padded);
	if (error != 0)
		return error;

	/* The sequence moves on with every frame the chip took. */
	bus->tx_sequence++;
	bus->counters.frames_out++;

	/* Succeeded: the frame is with the firmware. */
	return 0;
}

/*
 * Sends, within the credit, the queued request first and then the
 * device's data frames.
 */
static int
send_pending(
	struct bcm43455_bus *bus)
{
	int error;

	/* The control request goes first: a requester is waiting on it. */
	error = send_slot(bus);
	if (error != 0)
		return error;

	/* Then as many data frames as the credit allows. */
	error = send_data(bus);
	if (error != 0)
		return error;

	/* Succeeded: what could be sent was sent. */
	return 0;
}

/*
 * Moves the queued request into a frame and sends it.
 */
static int
send_slot(
	struct bcm43455_bus *bus)
{
	unsigned long enabled;
	unsigned window;
	size_t length;
	int error;

	/* Waits for credit; the request stays queued. */
	window = bcm43455_sdpcm_window(bus->tx_credit_limit, bus->tx_sequence);
	if (window == 0)
		return 0;

	/* Copies a queued request into the frame and clears the slot's copy. */
	length = 0;
	enabled = spin_lock_irqsave(&bus->lock);

	if (bus->slot.state == BCM43455_SLOT_QUEUED) {
		length = build_control(
			bus,
			bus->slot.command,
			(int)bus->slot.set,
			bus->slot.id,
			bus->slot.request,
			bus->slot.request_length);
		kern_memset(bus->slot.request, 0, sizeof(bus->slot.request));
		bus->slot.state = BCM43455_SLOT_SENT;
	}

	spin_unlock_irqrestore(&bus->lock, enabled);

	/* Nothing was queued. */
	if (length == 0)
		return 0;

	/* Sends it; a failed write answers the requester and ends the bus. */
	error = send_frame(bus, BCM43455_SDPCM_CHANNEL_CONTROL, length);
	if (error != 0) {
		enabled = spin_lock_irqsave(&bus->lock);

		if (bus->slot.state == BCM43455_SLOT_SENT) {
			bus->slot.error = error;
			bus->slot.answer_length = 0;
			bus->slot.state = BCM43455_SLOT_ANSWERED;
			waitq_wake_all(&bus->answer_wait);
		}

		spin_unlock_irqrestore(&bus->lock, enabled);
		return error;
	}

	/* Succeeded: the request is with the firmware. */
	return 0;
}

/*
 * Pulls the device's frames to send and sends them within the credit.
 */
static int
send_data(
	struct bcm43455_bus *bus)
{
	unsigned long enabled;
	unsigned window;
	size_t length;
	int given;
	int error;

	/*
	 * Clears the request before pulling, so a frame the device queues
	 * meanwhile asks again and is not lost.
	 */
	enabled = spin_lock_irqsave(&bus->lock);

	bus->work_requested = 0;

	spin_unlock_irqrestore(&bus->lock, enabled);

	/* Sends frames until the device has none or the credit runs out. */
	for (;;) {
		/*
		 * Without credit, the frames stay with the device until the
		 * firmware grants more.
		 */
		window = bcm43455_sdpcm_window(bus->tx_credit_limit, bus->tx_sequence);
		if (window == 0) {
			enabled = spin_lock_irqsave(&bus->lock);

			bus->work_requested = 1;

			spin_unlock_irqrestore(&bus->lock, enabled);
			break;
		}

		/*
		 * Asks the device for its next frame, placed behind both
		 * headers.
		 */
		length = 0;
		given = bus->hooks->transmit(
			bus->hook_context,
			bus->tx_frame + BCM43455_SDPCM_HEADER_LENGTH + BCM43455_BCDC_DATA_LENGTH,
			BCM43455_BUS_ETHERNET_MAX,
			&length);
		if (!given)
			break;

		/* Puts the data header in front and sends it. */
		bcm43455_bcdc_encode_data(bus->tx_frame + BCM43455_SDPCM_HEADER_LENGTH, 0, 0);
		error = send_frame(bus, BCM43455_SDPCM_CHANNEL_DATA, BCM43455_SDPCM_HEADER_LENGTH + BCM43455_BCDC_DATA_LENGTH + length);
		if (error != 0)
			return error;
		bus->counters.data_out++;
	}

	/* Succeeded: the frames that fit the credit were sent. */
	return 0;
}

/*
 * Writes a control message (its BCDC header and payload) behind the
 * SDPCM header in tx_frame; returns the frame's length.
 */
static size_t
build_control(
	struct bcm43455_bus *bus,
	uint32_t command,
	int set,
	uint16_t id,
	const uint8_t *payload,
	size_t payload_length)
{
	uint8_t *message;

	/* The BCDC header follows the SDPCM header, the payload follows it. */
	message = bus->tx_frame + BCM43455_SDPCM_HEADER_LENGTH;
	bcm43455_bcdc_encode_control(message, command, (uint32_t)payload_length, set, id);
	if (payload_length != 0)
		kern_memcpy(message + BCM43455_BCDC_CONTROL_LENGTH, payload, payload_length);

	/* Reports the frame's length, header included. */
	return BCM43455_SDPCM_HEADER_LENGTH + BCM43455_BCDC_CONTROL_LENGTH + payload_length;
}

/*
 * Runs a control request on the caller's own thread, which owns the bus
 * in direct mode.
 */
static int
direct_control(
	struct bcm43455_bus *bus,
	uint32_t command,
	int set,
	const uint8_t *input,
	size_t input_length,
	size_t request_length,
	uint8_t *answer,
	size_t answer_capacity,
	size_t *answer_length)
{
	int outcome;
	int error;

	/* Sends it and reads until the answer comes. */
	error = own_request(bus, command, set, input, input_length, request_length, answer, answer_capacity, answer_length, &outcome);
	if (error != 0)
		return error;

	/* Reports a command without an answer, or one the firmware refused. */
	if (outcome != 0)
		return outcome;

	/* Succeeded: answer holds what came back. */
	return 0;
}

/*
 * Sends a control request from whoever owns the bus (the lifecycle owner
 * in direct mode, or the bus thread for its work) and reads what the chip
 * sends until the answer comes.  The return value is the bus's error;
 * outcome receives the request's own: 0, ETIMEDOUT, ECANCELED, or the
 * firmware's refusal.
 */
static int
own_request(
	struct bcm43455_bus *bus,
	uint32_t command,
	int set,
	const uint8_t *input,
	size_t input_length,
	size_t request_length,
	uint8_t *answer,
	size_t answer_capacity,
	size_t *answer_length,
	int *outcome)
{
	unsigned long enabled;
	uint64_t deadline;
	uint16_t id;
	size_t length;
	unsigned window;
	int error;

	/* No refusal yet. */
	*outcome = 0;

	/* Takes the request's id. */
	enabled = spin_lock_irqsave(&bus->lock);

	id = take_id_locked(bus);

	spin_unlock_irqrestore(&bus->lock, enabled);

	/* Waits for credit, reading what the chip sends meanwhile. */
	deadline = deadline_after(BCM43455_BUS_CONTROL_TIMEOUT_MS);
	for (;;) {
		window = bcm43455_sdpcm_window(bus->tx_credit_limit, bus->tx_sequence);
		if (window != 0)
			break;
		error = direct_wait(bus, deadline, outcome);
		if (error != 0)
			return error;
		if (*outcome != 0)
			return 0;
	}

	/* Marks the request as the one the receive path completes. */
	bus->direct.active = 1;
	bus->direct.done = 0;
	bus->direct.id = id;
	bus->direct.command = command;
	bus->direct.answer = answer;
	bus->direct.answer_capacity = answer_capacity;
	bus->direct.answer_length = 0;
	bus->direct.error = 0;

	/*
	 * Builds the frame: the input, then zeros up to the room for the
	 * answer.
	 */
	length = build_control(bus, command, set, id, input, input_length);
	if (request_length > input_length) {
		kern_memset(bus->tx_frame + length, 0, request_length - input_length);
		bcm43455_bcdc_encode_control(
			bus->tx_frame + BCM43455_SDPCM_HEADER_LENGTH,
			command,
			(uint32_t)request_length,
			set,
			id);
		length += request_length - input_length;
	}

	/* Sends the request on the control channel. */
	error = send_frame(bus, BCM43455_SDPCM_CHANNEL_CONTROL, length);
	if (error != 0) {
		bus->direct.active = 0;
		bus->direct.answer = NULL;
		return error;
	}

	/*
	 * Reads what the chip sends until the answer comes, the time is spent,
	 * or a stop comes.
	 */
	error = 0;
	while (!bus->direct.done) {
		error = direct_wait(bus, deadline, outcome);
		if (error != 0)
			break;
		if (*outcome != 0)
			break;
	}

	/* Ends the direct request, whatever came of it. */
	bus->direct.active = 0;
	bus->direct.answer = NULL;
	if (error != 0)
		return error;
	if (*outcome != 0)
		return 0;

	/* The firmware's refusal is the request's outcome. */
	if (bus->direct.error != 0) {
		*outcome = bus->direct.error;
		return 0;
	}

	/* Succeeded: answer holds what came back. */
	if (answer_length != NULL)
		*answer_length = bus->direct.answer_length;
	return 0;
}

/*
 * Hands a control request to the bus thread through the slot and waits
 * for its answer.  The caller holds control_lock, so the slot is free of
 * any other live request.
 */
static int
slot_control(
	struct bcm43455_bus *bus,
	uint32_t command,
	int set,
	const uint8_t *input,
	size_t input_length,
	size_t request_length,
	uint8_t *answer,
	size_t answer_capacity,
	size_t *answer_length)
{
	unsigned long enabled;
	uint64_t deadline;
	uint64_t observed;
	uint64_t now;
	size_t copied;
	int error;

	/* Fills the slot; a request abandoned earlier is simply replaced. */
	enabled = spin_lock_irqsave(&bus->lock);

	if (!bus->thread_running || bus->stop_requested) {
		spin_unlock_irqrestore(&bus->lock, enabled);
		return ENETDOWN;
	}

	/* Copies the request in and marks it queued for the thread. */
	kern_memset(bus->slot.request, 0, sizeof(bus->slot.request));
	if (input_length != 0)
		kern_memcpy(bus->slot.request, input, input_length);
	bus->slot.id = take_id_locked(bus);
	bus->slot.command = command;
	bus->slot.set = 0;
	if (set)
		bus->slot.set = 1;
	bus->slot.request_length = request_length;
	bus->slot.answer_capacity = answer_capacity;
	bus->slot.answer_length = 0;
	bus->slot.error = 0;
	bus->slot.state = BCM43455_SLOT_QUEUED;

	spin_unlock_irqrestore(&bus->lock, enabled);

	/* Wakes the thread from its wait for the card. */
	bus->chip.ops->interrupt_kick(bus->chip.context);

	/* Waits for the answer or the deadline. */
	deadline = deadline_after(BCM43455_BUS_CONTROL_TIMEOUT_MS);
	enabled = spin_lock_irqsave(&bus->lock);

	while (bus->slot.state != BCM43455_SLOT_ANSWERED) {
		/* Gives up at the deadline. */
		now = sched_ticks();
		if (now >= deadline)
			break;

		/*
		 * Sleeps on the thread's wakeup; EAGAIN means one came
		 * meanwhile.
		 */
		observed = waitq_sequence(&bus->answer_wait);
		(void)waitq_sleep(&bus->answer_wait, &bus->lock, observed, deadline, 0);
	}

	/*
	 * Takes the answer, or leaves the slot so a late answer is recognized
	 * and dropped.
	 */
	copied = 0;
	if (bus->slot.state == BCM43455_SLOT_ANSWERED) {
		error = bus->slot.error;
		if (error == 0) {
			copied = bus->slot.answer_length;
			if (copied != 0)
				kern_memcpy(answer, bus->slot.answer, copied);
		}

		/* Clears the slot's copy and frees the slot. */
		kern_memset(bus->slot.answer, 0, sizeof(bus->slot.answer));
		bus->slot.state = BCM43455_SLOT_EMPTY;
	} else if (bus->slot.state == BCM43455_SLOT_QUEUED) {
		/* Never sent: the request is withdrawn. */
		kern_memset(bus->slot.request, 0, sizeof(bus->slot.request));
		bus->slot.state = BCM43455_SLOT_EMPTY;
		error = ETIMEDOUT;
	} else {
		/*
		 * Sent and unanswered: the thread drops the answer if it comes.
		 */
		bus->slot.state = BCM43455_SLOT_ABANDONED;
		error = ETIMEDOUT;
	}

	spin_unlock_irqrestore(&bus->lock, enabled);

	/* Reports why there is no answer. */
	if (error != 0)
		return error;

	/* Succeeded: answer holds what came back. */
	if (answer_length != NULL)
		*answer_length = copied;
	return 0;
}

/*
 * One step of a wait for a request's answer: reads what the chip has,
 * then sleeps until its interrupt or a short step.  outcome receives
 * ETIMEDOUT at the deadline, and ECANCELED when the bus thread waits and
 * is asked to stop; the return value is the bus's error.
 */
static int
direct_wait(
	struct bcm43455_bus *bus,
	uint64_t deadline,
	int *outcome)
{
	unsigned long enabled;
	unsigned stopping;
	uint64_t step;
	uint64_t now;
	int waited;
	int error;

	/* Reads what the chip has. */
	error = service(bus);
	if (error != 0)
		return error;
	if (bus->direct.active && bus->direct.done)
		return 0;

	/*
	 * The bus thread gives up when it is asked to stop; direct mode has no
	 * thread to stop.
	 */
	enabled = spin_lock_irqsave(&bus->lock);

	stopping = 0;
	if (bus->thread_running && bus->stop_requested)
		stopping = 1;

	spin_unlock_irqrestore(&bus->lock, enabled);

	/* A stop request ends the wait. */
	if (stopping) {
		*outcome = ECANCELED;
		return 0;
	}

	/* Gives up at the deadline. */
	now = sched_ticks();
	if (now >= deadline) {
		*outcome = ETIMEDOUT;
		return 0;
	}

	/* Sleeps until the interrupt or the next step, whichever is first. */
	step = deadline_after(DIRECT_STEP_MS);
	if (step > deadline)
		step = deadline;
	waited = bus->chip.ops->interrupt_wait(bus->chip.context, step);
	if (waited == 0) {
		bus->counters.interrupts++;
		bus->chip.ops->interrupt_rearm(bus->chip.context);
	}

	/* Succeeded: the caller looks again. */
	return 0;
}

/* Takes the next request id, never zero; the caller holds lock. */
static uint16_t
take_id_locked(
	struct bcm43455_bus *bus)
{
	uint16_t id;

	/* The id wraps around, skipping zero. */
	id = bus->next_id;
	bus->next_id++;
	if (bus->next_id == 0)
		bus->next_id = 1;

	/* Succeeded: the id names one request. */
	return id;
}

/* The scheduler tick a number of milliseconds from now. */
static uint64_t
deadline_after(
	unsigned milliseconds)
{
	uint64_t now;
	uint64_t ticks;

	/* Adds the milliseconds in ticks to the scheduler's clock. */
	now = sched_ticks();
	ticks = kern_ms_to_ticks(milliseconds);

	/* Reports the tick of the deadline. */
	return now + ticks;
}
