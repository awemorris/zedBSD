/* WS025-p027: production slot, CID, IRQ, BIO and pipeline fault injection. */
#include "nvme-production.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } checks++; } while (0)
static unsigned checks;
static struct nvme_controller ctl;
static struct drv_nvme_command sq[64];
static struct drv_nvme_completion cq[64];
static unsigned char bounce[63][4096] __attribute__((aligned(4096)));
static unsigned char media[40 * 4096];
static unsigned char buffer[sizeof(media)];
static unsigned post_count, recover_count, wait_count;
static unsigned fail_post, fail_chunk, timeout_wait, stale_completion;
static unsigned reverse_order, release_external, flush_wait, data_wait;
static int completed_error;
static size_t completed_bytes;
static unsigned completions;
static unsigned long ticks;

unsigned long spin_lock_irqsave(struct spinlock *lock)
{
	CHECK(!lock->held.value);
	lock->held.value = 1;
	return 0;
}
void spin_unlock_irqrestore(struct spinlock *lock, unsigned long irq)
{
	(void)irq;
	CHECK(lock->held.value);
	lock->held.value = 0;
}
uint64_t sched_ticks(void) { return ticks++; }
void waitq_wake_all(struct wait_queue *q) { (void)q; }
void hal_io_wmb(void) {}
void hal_io_rmb(void) {}
void bio_complete(struct bio *bio, int error, size_t bytes)
{
	(void)bio;
	completed_error = error;
	completed_bytes = bytes;
	completions++;
	CHECK(ctl.io_owned == 0);
	CHECK(ctl.io_data_active == 0 && ctl.io_flush_active == 0);
}

static void nvme_write32(struct nvme_controller *c, size_t offset, uint32_t value)
{
	(void)value;
	if (offset != c->io_submission_doorbell)
		return;
	post_count++;
	if (post_count == fail_post)
		nvme_io_fail_all_locked(c, EIO);
}

static void complete_slot(struct nvme_controller *c, struct nvme_io_slot *slot)
{
	struct drv_nvme_completion *entry = &cq[c->io_completion_cursor.head];
	struct drv_nvme_command *command = NULL;
	uint64_t block;
	unsigned i, opcode, chunk;
	size_t size;
	for (i = 0; i < 64; i++) {
		if ((sq[i].cdw0 >> 16) == slot->command_id)
			command = &sq[i];
	}
	CHECK(command != NULL);
	block = command->cdw10 | ((uint64_t)command->cdw11 << 32);
	opcode = command->cdw0 & 255U;
	size = ((command->cdw12 & 65535U) + 1U) * 512U;
	chunk = (unsigned)(block / 8U) + 1U;
	if (opcode != DRV_NVME_NVM_FLUSH) {
		CHECK(block * 512U + size <= sizeof(media));
		if (chunk != fail_chunk) {
			if (opcode == DRV_NVME_NVM_READ)
				memcpy(slot->bounce_dma.address, media + block * 512U, size);
			else
				memcpy(media + block * 512U, slot->bounce_dma.address, size);
		}
	}
	memset(entry, 0, sizeof(*entry));
	entry->command_id = stale_completion ? (uint16_t)(slot->command_id + 1000) : slot->command_id;
	entry->submission_id = NVME_IO_QUEUE_ID;
	entry->submission_head = c->io_submission_tail;
	entry->status = c->io_completion_cursor.phase;
	if (chunk == fail_chunk)
		entry->status |= 2U; /* Invalid opcode -> EOPNOTSUPP. */
	CHECK(nvme_irq_io_locked(c) == 1);
}

static int nvme_io_wait_locked(struct nvme_controller *c, struct wait_queue *q,
    uint64_t deadline, unsigned long *irq)
{
	unsigned i, n;
	(void)q; (void)deadline;
	CHECK(++wait_count < 200);
	if (timeout_wait)
		return ETIMEDOUT;
	if (release_external) {
		/* Emulate the other owner retiring after we sleep with no owned work. */
		release_external = 0;
		spin_unlock_irqrestore(&c->command_lock, *irq);
		nvme_io_slot_release(c, &c->io_slots[0]);
		*irq = spin_lock_irqsave(&c->command_lock);
		return 0;
	}
	if (flush_wait || data_wait) {
		spin_unlock_irqrestore(&c->command_lock, *irq);
		if (flush_wait) {
			CHECK(c->io_flush_waiting == 1 && c->io_data_active == 1);
			nvme_io_end_bio(c, BIO_WRITE);
			flush_wait = 0;
		} else {
			CHECK(c->io_flush_active == 1 && c->io_data_active == 0);
			nvme_io_end_bio(c, BIO_FLUSH);
			data_wait = 0;
		}
		*irq = spin_lock_irqsave(&c->command_lock);
		return 0;
	}
	for (n = 0; n < c->io_slot_count; n++) {
		i = reverse_order ? c->io_slot_count - n - 1 : n;
		if (c->io_slots[i].posted)
			complete_slot(c, &c->io_slots[i]);
	}
	return 0;
}

static int nvme_io_recover(struct nvme_controller *c)
{
	unsigned i;
	CHECK(c->io_owned == 0 && c->io_pending == 0);
	CHECK(c->io_recovery_busy && c->io_recovery_needed);
	for (i = 0; i < c->io_slot_count; i++) {
		(void)drv_nvme_io_lifecycle_stop(&c->io_slots[i].lifecycle);
		CHECK(drv_nvme_io_lifecycle_quiesced(&c->io_slots[i].lifecycle) == 0);
		CHECK(drv_nvme_io_lifecycle_online(&c->io_slots[i].lifecycle) == 0);
	}
	c->io_recovery_busy = c->io_recovery_needed = c->stopping = 0;
	c->io_fault = 0;
	c->io_epoch++;
	recover_count++;
	return 0;
}

static void setup(unsigned slots)
{
	unsigned i;
	memset(&ctl, 0, sizeof(ctl));
	memset(sq, 0, sizeof(sq));
	memset(cq, 0, sizeof(cq));
	memset(buffer, 0xa5, sizeof(buffer));
	for (i = 0; i < sizeof(media); i++) media[i] = (unsigned char)(i * 17 + i / 4096);
	ctl.io_submission = sq;
	ctl.io_completion = cq;
	ctl.io_slot_count = slots;
	ctl.io_queue_depth = 64;
	ctl.io_completion_cursor.depth = 64;
	ctl.io_completion_cursor.phase = 1;
	ctl.io_submission_doorbell = 4096;
	ctl.io_completion_doorbell = 4100;
	ctl.io_queue_ready = 1;
	ctl.namespace_id = 1;
	ctl.namespace_blocks = sizeof(media) / 512;
	ctl.namespace_block_size = 512;
	ctl.maximum_transfer_bytes = 4096;
	ctl.page_size = 4096;
	ctl.timeout_ticks = 1000;
	for (i = 0; i < slots; i++) {
		ctl.io_slots[i].bounce_dma.address = bounce[i];
		ctl.io_slots[i].bounce_dma.device_address = 0x10000 + i * 4096;
		drv_nvme_io_lifecycle_init(&ctl.io_slots[i].lifecycle);
		CHECK(drv_nvme_io_lifecycle_online(&ctl.io_slots[i].lifecycle) == 0);
	}
	post_count = recover_count = wait_count = completions = 0;
	fail_post = fail_chunk = timeout_wait = stale_completion = 0;
	reverse_order = release_external = flush_wait = data_wait = 0;
}

static void submit(unsigned op, unsigned blocks)
{
	struct disk disk;
	struct bio bio;
	memset(&disk, 0, sizeof(disk));
	memset(&bio, 0, sizeof(bio));
	disk.d_data = &ctl;
	bio.b_op = op;
	bio.b_block_count = blocks;
	bio.b_data = buffer;
	CHECK(nvme_disk_submit(&disk, &bio) == 0);
	CHECK(ctl.io_calls == 0 && ctl.io_owned == 0);
}

int main(int argc, char **argv)
{
	unsigned reverse, slots, i;
	int owned;
	if (argc == 2 && strcmp(argv[1], "--layout") == 0) {
		printf("{\"size\":%zu,\"posted\":%zu,\"high_water\":%zu}\n",
		    sizeof(ctl), offsetof(struct nvme_controller, io_commands_posted),
		    offsetof(struct nvme_controller, io_pending_high_water));
		return 0;
	}
	for (reverse = 0; reverse < 2; reverse++) {
		for (slots = 1; slots <= 8; slots *= 2) {
			setup(slots); reverse_order = reverse;
			submit(BIO_READ, 319); /* Ring wrap and a final short chunk. */
			CHECK(completed_error == 0 && completed_bytes == 319 * 512);
			CHECK(memcmp(buffer, media, completed_bytes) == 0);
			CHECK(buffer[completed_bytes] == 0xa5);
			CHECK(post_count == 40 && ctl.io_commands_posted == 40);
			CHECK(ctl.io_pending_high_water == (slots < NVME_IO_PIPELINE_DEPTH ? slots : NVME_IO_PIPELINE_DEPTH));
			submit(BIO_FLUSH, 0);
			CHECK(completed_error == 0 && completed_bytes == 0);
			memset(buffer, 0x53, sizeof(buffer));
			submit(BIO_WRITE, 320);
			CHECK(completed_error == 0 && memcmp(buffer, media, sizeof(media)) == 0);
		}
	}
	for (i = 1; i <= 9; i++) {
		setup(8); reverse_order = 1; fail_chunk = i;
		submit(BIO_READ, 320);
		CHECK(completed_error != 0 && completed_bytes == (i - 1) * 4096);
		CHECK(memcmp(buffer, media, completed_bytes) == 0);
		CHECK(buffer[completed_bytes] == 0xa5 && recover_count == 0);
		CHECK(post_count <= ((i - 1) / NVME_IO_PIPELINE_DEPTH + 1) * NVME_IO_PIPELINE_DEPTH);
	}
	setup(8); fail_post = 2;
	submit(BIO_READ, 320);
	CHECK(completed_error == EIO && recover_count == 1 && post_count == 2);
	fail_post = 0;
	submit(BIO_READ, 320);
	CHECK(completed_error == 0 && memcmp(buffer, media, sizeof(media)) == 0);
	if (NVME_IO_PIPELINE_DEPTH > 1) {
		setup(8);
		ctl.io_slots[1].bounce_dma.device_address++;
		submit(BIO_READ, 320);
		CHECK(completed_error == EINVAL && recover_count == 1);
		CHECK(post_count == 1 && completed_bytes == 0);
	}
	setup(8); timeout_wait = 1;
	submit(BIO_READ, 320);
	CHECK(completed_error == ETIMEDOUT && recover_count == 1 && completed_bytes == 0);
	setup(8); stale_completion = 1;
	submit(BIO_READ, 320);
	CHECK(completed_error == EIO && recover_count == 1 && completed_bytes == 0);
	setup(1);
	CHECK(drv_nvme_io_lifecycle_begin_bio(&ctl.io_slots[0].lifecycle) == 0);
	ctl.io_slots[0].state = NVME_IO_SLOT_COPYING; ctl.io_owned = 1;
	release_external = 1;
	submit(BIO_READ, 8);
	CHECK(completed_error == 0 && release_external == 0);
	setup(8);
	CHECK(nvme_io_begin_bio(&ctl, BIO_WRITE, &owned) == 0 && owned);
	flush_wait = 1;
	submit(BIO_FLUSH, 0);
	CHECK(completed_error == 0 && flush_wait == 0);
	CHECK(nvme_io_begin_bio(&ctl, BIO_FLUSH, &owned) == 0 && owned);
	data_wait = 1;
	submit(BIO_READ, 8);
	CHECK(completed_error == 0 && data_wait == 0);
	printf("NVMe pipeline depth %u PASS: %u checks\n", NVME_IO_PIPELINE_DEPTH, checks);
	return 0;
}
