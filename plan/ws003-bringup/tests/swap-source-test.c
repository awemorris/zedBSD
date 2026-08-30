/* BR-T45: production swap header, source, aggregation, and lifecycle fixture. */
#include <kern/disk.h>
#include <kern/fat.h>
#include <kern/file.h>
#include <kern/inode.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/mount.h>
#include <kern/swap-source.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define TEST_IO_RECORDS 128U
#define TEST_FILE_EXTENTS 1025U

const struct filesystem_type fat_filesystem_type;

struct test_io_record {
	int write;
	uint64_t block;
	uint32_t count;
};

struct test_disk_state {
	uint8_t *bytes;
	size_t byte_count;
	struct test_io_record record[TEST_IO_RECORDS];
	unsigned record_count;
	unsigned opens;
	unsigned closes;
	unsigned open_handles;
	unsigned refs;
	unsigned releases;
	unsigned flushes;
	int open_error;
	int read_error;
	int write_error;
	int flush_error;
};

struct test_extent {
	uint64_t file_block;
	uint64_t disk_block;
	uint32_t count;
};

struct test_file_state {
	uint8_t header[ZEDBSD_SWAP_HEADER_SIZE];
	struct test_extent extent[TEST_FILE_EXTENTS];
	unsigned extent_count;
	unsigned opens;
	unsigned closes;
	unsigned refs;
	unsigned releases;
};

void *
kern_malloc(size_t size)
{
	return malloc(size);
}

void *
kern_calloc(size_t count, size_t size)
{
	return calloc(count, size);
}

void
kern_free(void *pointer)
{
	free(pointer);
}

void
kern_logf(const char *format, ...)
{
	(void)format;
}

static uint64_t test_commit_swap_pages;
static int test_commit_resize_error;
static unsigned test_drain_calls;
static int test_drain_error;
static int test_drain_block;
static int test_drain_entered;
static int test_drain_release;
static pthread_mutex_t test_drain_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_drain_condition = PTHREAD_COND_INITIALIZER;

int
vm_commit_resize_swap(uint64_t expected_pages, uint64_t replacement_pages)
{
	if (test_commit_swap_pages != expected_pages)
		fprintf(stderr, "commit swap mismatch: have=%llu expected=%llu "
		    "replacement=%llu\n",
		    (unsigned long long)test_commit_swap_pages,
		    (unsigned long long)expected_pages,
		    (unsigned long long)replacement_pages);
	assert(test_commit_swap_pages == expected_pages);
	if (test_commit_resize_error != 0) {
		int error = test_commit_resize_error;

		test_commit_resize_error = 0;
		return error;
	}
	test_commit_swap_pages = replacement_pages;
	return 0;
}

int
vm_reclaim_drain_swap_source(unsigned source_id)
{
	(void)source_id;
	test_drain_calls++;
	assert(pthread_mutex_lock(&test_drain_gate) == 0);
	test_drain_entered = 1;
	assert(pthread_cond_broadcast(&test_drain_condition) == 0);
	while (test_drain_block && !test_drain_release)
		assert(pthread_cond_wait(&test_drain_condition,
		    &test_drain_gate) == 0);
	assert(pthread_mutex_unlock(&test_drain_gate) == 0);
	if (test_drain_error != 0) {
		int error = test_drain_error;

		test_drain_error = 0;
		return error;
	}
	return 0;
}

int
vm_reclaim_drain_swap_source_cancelable(unsigned source_id,
	int (*cancel)(void *), void *argument)
{
	int error = cancel != NULL ? cancel(argument) : 0;

	if (error != 0)
		return error;
	return vm_reclaim_drain_swap_source(source_id);
}

static int
test_remove_interrupt(void *argument)
{
	(void)argument;
	return EINTR;
}

int
mount_disk_writable_busy(struct disk *disk)
{
	(void)disk;
	return 0;
}

void
hal_fatal(const char *file, int line, const char *message)
{
	fprintf(stderr, "HAL_FATAL %s:%d: %s\n", file, line, message);
	abort();
}

static struct test_disk_state *
disk_state(struct disk *disk)
{
	return disk != NULL ? disk->d_data : NULL;
}

int
disk_open(struct disk *disk)
{
	struct test_disk_state *state = disk_state(disk);

	if (state == NULL)
		return ENXIO;
	state->opens++;
	if (state->open_error != 0)
		return state->open_error;
	state->open_handles++;
	return 0;
}

void
disk_close(struct disk *disk)
{
	struct test_disk_state *state = disk_state(disk);

	assert(state != NULL && state->open_handles != 0);
	state->open_handles--;
	state->closes++;
}

void
disk_ref(struct disk *disk)
{
	struct test_disk_state *state = disk_state(disk);

	assert(state != NULL);
	state->refs++;
}

void
disk_release(struct disk *disk)
{
	struct test_disk_state *state = disk_state(disk);

	assert(state != NULL && state->releases < state->refs);
	state->releases++;
}

static int
disk_transfer(struct disk *disk, uint64_t block, uint32_t count, void *data,
	      int write)
{
	struct test_disk_state *state = disk_state(disk);
	uint64_t offset, amount;

	if (state == NULL || data == NULL || count == 0 ||
	    disk->d_block_size == 0 ||
	    block > UINT64_MAX / disk->d_block_size ||
	    count > UINT64_MAX / disk->d_block_size)
		return EINVAL;
	offset = block * disk->d_block_size;
	amount = (uint64_t)count * disk->d_block_size;
	if (offset > state->byte_count || amount > state->byte_count - offset)
		return EOVERFLOW;
	if (state->record_count < ARRAY_COUNT(state->record)) {
		struct test_io_record *record =
		    &state->record[state->record_count++];

		record->write = write;
		record->block = block;
		record->count = count;
	}
	if (write && state->write_error != 0)
		return state->write_error;
	if (!write && state->read_error != 0)
		return state->read_error;
	if (write)
		memcpy(state->bytes + (size_t)offset, data, (size_t)amount);
	else
		memcpy(data, state->bytes + (size_t)offset, (size_t)amount);
	return 0;
}

int
disk_read_direct(struct disk *disk, uint64_t block, uint32_t count, void *data)
{
	return disk_transfer(disk, block, count, data, 0);
}

int
disk_write_direct(struct disk *disk, uint64_t block, uint32_t count,
		  const void *data)
{
	return disk_transfer(disk, block, count, (void *)data, 1);
}

int
bio_flush(struct disk *disk)
{
	struct test_disk_state *state = disk_state(disk);

	assert(state != NULL);
	state->flushes++;
	return state->flush_error;
}

int
disk_resolve_range(struct disk *disk, uint64_t block, uint32_t count,
	struct disk **leaf_out, uint64_t *mapped_out)
{
	struct disk *leaf;
	uint64_t mapped;

	if (disk == NULL || leaf_out == NULL || mapped_out == NULL || count == 0 ||
	    block >= disk->d_block_count ||
	    count > disk->d_block_count - block)
		return EINVAL;
	leaf = disk;
	mapped = block;
	while (leaf->d_parent != NULL) {
		if (mapped > UINT64_MAX - leaf->d_parent_offset)
			return EOVERFLOW;
		mapped += leaf->d_parent_offset;
		leaf = leaf->d_parent;
	}
	if (mapped >= leaf->d_block_count ||
	    count > leaf->d_block_count - mapped)
		return EOVERFLOW;
	*leaf_out = leaf;
	*mapped_out = mapped;
	return 0;
}

void
inode_ref(struct inode *inode)
{
	struct test_file_state *state;

	assert(inode != NULL && inode->i_data != NULL);
	state = inode->i_data;
	state->refs++;
}

void
inode_release(struct inode *inode)
{
	struct test_file_state *state;

	assert(inode != NULL && inode->i_data != NULL);
	state = inode->i_data;
	assert(state->releases < state->refs);
	state->releases++;
}

void
mutex_lock(struct mutex *mutex)
{
	while (__atomic_exchange_n(&mutex->locked, 1U, __ATOMIC_ACQUIRE) != 0U)
		;
}

void
mutex_unlock(struct mutex *mutex)
{
	assert(__atomic_load_n(&mutex->locked, __ATOMIC_RELAXED) != 0U);
	__atomic_store_n(&mutex->locked, 0U, __ATOMIC_RELEASE);
}

int
file_open_resolved(const struct path *resolved, int flags, struct file **result)
{
	struct test_file_state *state;
	struct file *file;

	if (resolved == NULL || resolved->p_inode == NULL || result == NULL ||
	    flags != O_RDWR)
		return EINVAL;
	state = resolved->p_inode->i_data;
	if (state == NULL)
		return EIO;
	file = calloc(1, sizeof(*file));
	if (file == NULL)
		return ENOMEM;
	file->f_inode = resolved->p_inode;
	state->opens++;
	*result = file;
	return 0;
}

ssize_t
file_pread(struct file *file, void *buffer, size_t length, off_t offset)
{
	struct test_file_state *state;

	if (file == NULL || file->f_inode == NULL || buffer == NULL ||
	    offset < 0)
		return -EINVAL;
	state = file->f_inode->i_data;
	if (state == NULL || (uint64_t)offset > sizeof(state->header) ||
	    length > sizeof(state->header) - (size_t)offset)
		return -EIO;
	memcpy(buffer, state->header + (size_t)offset, length);
	return (ssize_t)length;
}

int
file_close(struct file *file)
{
	struct test_file_state *state;

	assert(file != NULL && file->f_inode != NULL);
	state = file->f_inode->i_data;
	assert(state != NULL && state->closes < state->opens);
	state->closes++;
	free(file);
	return 0;
}

int
fat_file_extents(struct file *file, fat_extent_cb callback, void *context)
{
	struct test_file_state *state;
	unsigned index;

	if (file == NULL || file->f_inode == NULL || callback == NULL)
		return EINVAL;
	state = file->f_inode->i_data;
	if (state == NULL)
		return EIO;
	for (index = 0; index < state->extent_count; index++) {
		int error = callback(state->extent[index].file_block,
		    state->extent[index].disk_block,
		    state->extent[index].count, context);

		if (error != 0)
			return error;
	}
	return 0;
}

void
spin_lock(struct spinlock *lock)
{
	while (__atomic_exchange_n(&lock->held.value, 1U, __ATOMIC_ACQUIRE))
		;
}

void
spin_unlock(struct spinlock *lock)
{
	__atomic_store_n(&lock->held.value, 0U, __ATOMIC_RELEASE);
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	spin_lock(lock);
	return 0;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{
	(void)enabled;
	spin_unlock(lock);
}

static void
put16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static void
put32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static void
put64(uint8_t *p, uint64_t value)
{
	put32(p, (uint32_t)value);
	put32(p + 4U, (uint32_t)(value >> 32));
}

static void
make_v1(uint8_t header[ZEDBSD_SWAP_HEADER_SIZE], uint32_t bytes)
{
	memset(header, 0, ZEDBSD_SWAP_HEADER_SIZE);
	memcpy(header, "ZEDSWAP1", 8U);
	put32(header + 8U, 1U);
	put32(header + 12U, ZEDBSD_SWAP_HEADER_SIZE);
	put32(header + 16U, SWAP_PAGE_SIZE);
	put32(header + 20U, bytes);
	put32(header + 24U, bytes / SWAP_PAGE_SIZE - 1U);
	put32(header + 28U, swap_header_checksum(header));
}

static void
make_v2(uint8_t header[ZEDBSD_SWAP_HEADER_SIZE], uint64_t bytes)
{
	static const uint8_t uuid[ZEDBSD_SWAP_V2_UUID_SIZE] = {
		0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef
	};

	memset(header, 0, ZEDBSD_SWAP_HEADER_SIZE);
	memcpy(header, "ZEDSWAP2", 8U);
	put16(header + 8U, 2U);
	put16(header + 10U, ZEDBSD_SWAP_HEADER_SIZE);
	put32(header + 12U, SWAP_PAGE_SIZE);
	put64(header + 16U, bytes);
	put64(header + 24U, bytes / SWAP_PAGE_SIZE - 1U);
	memcpy(header + 32U, uuid, sizeof(uuid));
	memcpy(header + 40U, "TESTSWAP", 8U);
	put32(header + 60U, swap_header_checksum(header));
}

static void
test_headers(void)
{
	uint8_t header[ZEDBSD_SWAP_HEADER_SIZE];
	struct swap_header_info info;
	char uuid[17];
	uint32_t checksum;

	make_v1(header, ZEDBSD_SWAP_FILE_MAX_BYTES);
	assert(swap_header_parse(header, ZEDBSD_SWAP_FILE_MAX_BYTES, &info) == 0);
	assert(info.version == 1U);
	assert(info.slot_count == ZEDBSD_SWAP_FILE_MAX_BYTES / SWAP_PAGE_SIZE - 1U);
	assert(swap_header_uuid_format(&info, uuid, sizeof(uuid)) == ENOENT);
	assert(swap_header_validate(header, ZEDBSD_SWAP_FILE_MIN_BYTES) == EINVAL);

	make_v2(header, 128ULL * 1024U * 1024U);
	assert(swap_header_parse(header, 128ULL * 1024U * 1024U, &info) == 0);
	assert(info.version == 2U && info.slot_count == 32767U);
	assert(strcmp(info.label, "TESTSWAP") == 0);
	assert(swap_header_uuid_format(&info, uuid, sizeof(uuid)) == 0);
	assert(strcmp(uuid, "0123456789ABCDEF") == 0);
	assert(swap_header_validate(header, 64ULL * 1024U * 1024U) == EINVAL);
	checksum = swap_header_checksum(header);
	header[24] ^= 1U;
	assert(swap_header_validate(header, 128ULL * 1024U * 1024U) == EINVAL);
	header[24] ^= 1U;
	assert(swap_header_checksum(header) == checksum);
	header[59] = 'X';
	put32(header + 60U, swap_header_checksum(header));
	assert(swap_header_validate(header, 128ULL * 1024U * 1024U) == EINVAL);
}

static void
test_generated_header(const char *path, unsigned expected_version)
{
	uint8_t header[ZEDBSD_SWAP_HEADER_SIZE];
	struct swap_header_info info;
	FILE *file;
	long length;

	file = fopen(path, "rb");
	assert(file != NULL);
	assert(fread(header, 1U, sizeof(header), file) == sizeof(header));
	assert(fseek(file, 0L, SEEK_END) == 0);
	length = ftell(file);
	assert(length > 0);
	assert(fclose(file) == 0);
	assert(swap_header_parse(header, (uint64_t)length, &info) == 0);
	assert(info.version == expected_version);
	if (expected_version == 2U) {
		char uuid[17];

		assert(strcmp(info.label, "TESTSWAP") == 0);
		assert(swap_header_uuid_format(&info, uuid, sizeof(uuid)) == 0);
		assert(strcmp(uuid, "0123456789ABCDEF") == 0);
	}
}

static void
test_disk_init(struct disk *disk, struct test_disk_state *state, dev_t dev,
	       uint32_t block_size, uint64_t block_count, unsigned flags)
{
	assert(block_size != 0 && block_count <= SIZE_MAX / block_size);
	memset(disk, 0, sizeof(*disk));
	memset(state, 0, sizeof(*state));
	state->byte_count = (size_t)block_count * block_size;
	state->bytes = calloc(1, state->byte_count);
	assert(state->bytes != NULL);
	disk->d_dev = dev;
	disk->d_flags = flags;
	disk->d_block_size = block_size;
	disk->d_block_count = block_count;
	disk->d_data = state;
}

static void
test_disk_fini(struct test_disk_state *state)
{
	assert(state->open_handles == 0U);
	free(state->bytes);
	state->bytes = NULL;
}

struct fake_source {
	unsigned id;
	unsigned reads;
	unsigned writes;
	unsigned flushes;
	unsigned destroys;
	int flush_error;
	int block_reads;
	int read_entered;
	int read_release;
	pthread_mutex_t gate;
	pthread_cond_t condition;
};

static unsigned lifecycle_trace[64];
static unsigned lifecycle_trace_count;

static void
trace_event(unsigned event)
{
	assert(lifecycle_trace_count < ARRAY_COUNT(lifecycle_trace));
	lifecycle_trace[lifecycle_trace_count++] = event;
}

static int
fake_read(void *argument, uint32_t slot, void *page)
{
	struct fake_source *fake = argument;
	uint32_t *value = page;

	(void)__atomic_fetch_add(&fake->reads, 1U, __ATOMIC_RELAXED);
	if (fake->block_reads) {
		assert(pthread_mutex_lock(&fake->gate) == 0);
		fake->read_entered = 1;
		assert(pthread_cond_broadcast(&fake->condition) == 0);
		while (!fake->read_release)
			assert(pthread_cond_wait(&fake->condition, &fake->gate) == 0);
		assert(pthread_mutex_unlock(&fake->gate) == 0);
	}
	*value = fake->id * 100U + slot;
	return 0;
}

static int
fake_write(void *argument, uint32_t slot, const void *page)
{
	struct fake_source *fake = argument;
	const uint32_t *value = page;

	assert(*value == fake->id * 100U + slot);
	(void)__atomic_fetch_add(&fake->writes, 1U, __ATOMIC_RELAXED);
	return 0;
}

static int
fake_flush(void *argument)
{
	struct fake_source *fake = argument;

	(void)__atomic_fetch_add(&fake->flushes, 1U, __ATOMIC_RELAXED);
	trace_event(100U + fake->id);
	return fake->flush_error;
}

static void
fake_destroy(void *argument)
{
	struct fake_source *fake = argument;

	(void)__atomic_fetch_add(&fake->destroys, 1U, __ATOMIC_RELAXED);
	trace_event(200U + fake->id);
}

static const struct swap_backend_ops fake_ops = {
	.read_page = fake_read,
	.write_page = fake_write,
	.flush = fake_flush,
	.destroy = fake_destroy,
};

static void
make_source(struct kern_swap_source *source, struct fake_source *fake,
	    unsigned parameter_index, uint32_t slots, struct disk *identity_disk,
	    struct inode *identity_inode)
{
	kern_swap_source_init(source);
	source->ops = &fake_ops;
	source->data = fake;
	source->identity_disk = identity_disk;
	source->identity_inode = identity_inode;
	source->slot_count = slots;
	source->parameter_index = parameter_index;
}

static void
test_prepared_publication(void)
{
	struct swap_backend backend;
	struct swap_backend fresh;
	struct swap_backend already_enabled;
	struct fake_source fake = { .id = 9U };
	struct swap_source_stats source_stats;
	uint32_t slot, total, free_slots;

	/* The convenience add must undo an enable transition that it performed
	 * when prepare fails.  A manager which was already enabled remains so. */
	swap_init(&fresh);
	assert(swap_source_add(&fresh, 0U, NULL, &fake, SWAP_PAGE_SIZE, 1U) ==
	    EINVAL);
	assert(swap_get_stats(&fresh, &total, &free_slots) == ENXIO);
	assert(fake.flushes == 0U && fake.destroys == 0U);

	swap_init(&already_enabled);
	assert(swap_manager_enable(&already_enabled) == 0);
	assert(swap_source_add(&already_enabled, 0U, NULL, &fake,
	    SWAP_PAGE_SIZE, 1U) == EINVAL);
	assert(swap_get_stats(&already_enabled, &total, &free_slots) == 0);
	assert(total == 0U && free_slots == 0U);
	assert(swap_shutdown(&already_enabled) == 0);
	assert(fake.flushes == 0U && fake.destroys == 0U);

	swap_init(&backend);
	assert(swap_manager_enable(&backend) == 0);
	assert(swap_get_stats(&backend, &total, &free_slots) == 0);
	assert(total == 0U && free_slots == 0U);
	assert(swap_source_prepare(&backend, 1U, &fake_ops, &fake,
	    SWAP_PAGE_SIZE, 2U) == 0);
	assert(swap_source_get_stats(&backend, 1U, &source_stats) == 0);
	assert(source_stats.state == SWAP_SOURCE_STATE_PREPARED &&
	    source_stats.total_slots == 2U && source_stats.allocated_slots == 0U);
	assert(swap_get_stats(&backend, &total, &free_slots) == 0);
	assert(total == 0U && free_slots == 0U);
	assert(swap_alloc_slot(&backend, &slot) == ENOSPC);
	assert(swap_source_begin_drain(&backend, 1U) == EBUSY);
	assert(swap_shutdown(&backend) == EBUSY);
	assert(swap_source_cancel_prepare(&backend, 1U) == 0);
	assert(fake.flushes == 0U && fake.destroys == 0U);

	assert(swap_source_prepare(&backend, 1U, &fake_ops, &fake,
	    SWAP_PAGE_SIZE, 2U) == 0);
	assert(swap_source_publish(&backend, 1U) == 0);
	assert(swap_get_stats(&backend, &total, &free_slots) == 0);
	assert(total == 2U && free_slots == 2U);
	assert(swap_alloc_slot(&backend, &slot) == 0 &&
	    slot == (1U << SWAP_SLOT_SOURCE_SHIFT));
	swap_free_slot(&backend, slot);
	assert(swap_source_begin_drain(&backend, 1U) == 0);
	assert(swap_source_remove(&backend, 1U) == 0);
	assert(fake.flushes == 1U && fake.destroys == 1U);
	assert(swap_shutdown(&backend) == 0);
}

struct allocation_context {
	struct swap_backend *backend;
	unsigned loops;
};

static void *
allocate_worker(void *argument)
{
	struct allocation_context *context = argument;
	unsigned loop;

	for (loop = 0; loop < context->loops; loop++) {
		uint32_t slot;
		while (swap_alloc_slot(context->backend, &slot) == ENOSPC)
			;
		swap_free_slot(context->backend, slot);
	}
	return NULL;
}

struct io_context {
	struct swap_backend *backend;
	uint32_t slot;
	uint32_t expected;
	unsigned loops;
	int error;
};

static void *
io_worker(void *argument)
{
	struct io_context *context = argument;
	unsigned loop;

	for (loop = 0; loop < context->loops; loop++) {
		uint32_t value = context->expected;
		int error = swap_write_page(context->backend, context->slot,
		    &value);

		if (error == 0) {
			value = 0;
			error = swap_read_page(context->backend, context->slot,
			    &value);
		}
		if (error != 0 || value != context->expected) {
			context->error = error != 0 ? error : EIO;
			break;
		}
	}
	return NULL;
}

static void
test_sparse_cardinality_and_order(void)
{
	struct kern_swap_source_set set;
	struct kern_swap_source source;
	struct fake_source fake[4];
	struct disk identity[4];
	uint32_t slots[4], value;
	unsigned index;

	kern_swap_source_set_init(&set);
	assert(kern_swap_source_set_activate(&set) == 0);
	assert(swap_system_backend() == &set.backend);
	assert(kern_swap_source_set_abort(&set) == 0);
	assert(swap_system_backend() == NULL);

	memset(fake, 0, sizeof(fake));
	memset(identity, 0, sizeof(identity));
	fake[0].id = 3U;
	identity[0].d_dev = 30U;
	kern_swap_source_set_init(&set);
	make_source(&source, &fake[0], 3U, 2U, &identity[0], NULL);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	assert(kern_swap_source_set_activate(&set) == 0);
	assert(swap_alloc_slot(&set.backend, &slots[0]) == 0 &&
	    slots[0] == (3U << SWAP_SLOT_SOURCE_SHIFT));
	swap_free_slot(&set.backend, slots[0]);
	lifecycle_trace_count = 0;
	assert(kern_swap_source_set_abort(&set) == 0);
	assert(fake[0].flushes == 1U && fake[0].destroys == 1U);

	memset(fake, 0, sizeof(fake));
	memset(identity, 0, sizeof(identity));
	kern_swap_source_set_init(&set);
	for (index = 0; index < 4U; index++) {
		fake[index].id = index + 1U;
		identity[index].d_dev = 40U + index;
		make_source(&source, &fake[index], index, 1U,
		    &identity[index], NULL);
		assert(kern_swap_source_set_add(&set, &source) == 0);
	}
	assert(kern_swap_source_set_activate(&set) == 0);
	for (index = 0; index < 4U; index++) {
		assert(swap_alloc_slot(&set.backend, &slots[index]) == 0);
		assert(slots[index] == index << SWAP_SLOT_SOURCE_SHIFT);
		value = (index + 1U) * 100U;
		assert(swap_write_page(&set.backend, slots[index], &value) == 0);
	}
	for (index = 0; index < 4U; index++)
		swap_free_slot(&set.backend, slots[index]);
	lifecycle_trace_count = 0;
	assert(kern_swap_source_set_abort(&set) == 0);
	assert(lifecycle_trace_count == 8U);
	for (index = 0; index < 4U; index++) {
		assert(lifecycle_trace[index] == 101U + index);
		assert(lifecycle_trace[4U + index] == 201U + index);
	}
}

static void
test_aggregate_and_concurrency(void)
{
	struct kern_swap_source_set set;
	struct kern_swap_source source;
	struct fake_source first = { .id = 1U, .flush_error = EIO };
	struct fake_source second = { .id = 2U };
	struct disk first_disk = { .d_dev = 1U };
	struct disk second_disk = { .d_dev = 2U };
	struct allocation_context allocation;
	struct io_context io[4];
	pthread_t threads[4];
	uint32_t slots[5];
	uint32_t value;
	uint32_t total, free_slots;
	unsigned index, mapped;

	kern_swap_source_set_init(&set);
	make_source(&source, &first, 0U, 2U, &first_disk, NULL);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	make_source(&source, &second, 2U, 3U, &second_disk, NULL);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	assert(kern_swap_source_set_map(&set, 0U, &index, &mapped) == 0);
	assert(index == 0U && mapped == 0U);
	assert(kern_swap_source_set_map(&set,
	    2U << SWAP_SLOT_SOURCE_SHIFT, &index, &mapped) == 0);
	assert(index == 2U && mapped == 0U);
	assert(kern_swap_source_set_map(&set,
	    1U << SWAP_SLOT_SOURCE_SHIFT, NULL, NULL) == ERANGE);
	assert(kern_swap_source_set_activate(&set) == 0);
	assert(swap_system_backend() == &set.backend);
	for (index = 0; index < 5U; index++) {
		uint32_t expected = index < 2U ? index :
		    (2U << SWAP_SLOT_SOURCE_SHIFT) | (index - 2U);

		assert(swap_alloc_slot(&set.backend, &slots[index]) == 0 &&
		    slots[index] == expected);
	}
	assert(swap_alloc_slot(&set.backend, &mapped) == ENOSPC);
	value = 100U;
	assert(swap_write_page(&set.backend, slots[0], &value) == 0);
	value = 201U;
	assert(swap_write_page(&set.backend, slots[3], &value) == 0);
	value = 0;
	assert(swap_read_page(&set.backend, slots[2], &value) == 0);
	assert(value == 200U);

	for (index = 0; index < 4U; index++) {
		io[index].backend = &set.backend;
		io[index].slot = slots[index];
		io[index].expected = index < 2U ? 100U + index :
		    200U + index - 2U;
		io[index].loops = 500U;
		io[index].error = 0;
		assert(pthread_create(&threads[index], NULL, io_worker,
		    &io[index]) == 0);
	}
	for (index = 0; index < 4U; index++) {
		assert(pthread_join(threads[index], NULL) == 0);
		assert(io[index].error == 0);
	}
	for (index = 0; index < 5U; index++)
		swap_free_slot(&set.backend, slots[index]);
	assert(swap_get_stats(&set.backend, &total, &free_slots) == 0);
	assert(total == 5U && free_slots == 5U);

	allocation.backend = &set.backend;
	allocation.loops = 2000U;
	for (index = 0; index < 4U; index++)
		assert(pthread_create(&threads[index], NULL, allocate_worker,
		    &allocation) == 0);
	for (index = 0; index < 4U; index++)
		assert(pthread_join(threads[index], NULL) == 0);
	assert(swap_get_stats(&set.backend, &total, &free_slots) == 0 &&
	    free_slots == total);
	assert(swap_flush(&set.backend) == EIO);
	assert(first.flushes == 1U && second.flushes == 1U);
	lifecycle_trace_count = 0;
	assert(kern_swap_source_set_abort(&set) == EIO);
	assert(first.flushes == 2U && second.flushes == 2U);
	assert(first.destroys == 1U && second.destroys == 1U);
	assert(lifecycle_trace_count == 4U);
	assert(lifecycle_trace[0] == 101U && lifecycle_trace[1] == 102U);
	assert(lifecycle_trace[2] == 201U && lifecycle_trace[3] == 202U);
	assert(swap_system_backend() == NULL);
}

struct blocking_context {
	struct swap_backend *backend;
	uint32_t slot;
	int error;
};

static void *
blocking_read_worker(void *argument)
{
	struct blocking_context *context = argument;
	uint32_t value = 0;

	context->error = swap_read_page(context->backend, context->slot, &value);
	return NULL;
}

static void
test_concurrent_lifecycle(void)
{
	struct kern_swap_source_set set;
	struct kern_swap_source source;
	struct fake_source fake = { .id = 7U, .block_reads = 1 };
	struct disk identity = { .d_dev = 70U };
	struct blocking_context context;
	pthread_t thread;
	uint32_t slot;

	assert(pthread_mutex_init(&fake.gate, NULL) == 0);
	assert(pthread_cond_init(&fake.condition, NULL) == 0);
	kern_swap_source_set_init(&set);
	make_source(&source, &fake, 1U, 1U, &identity, NULL);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	assert(kern_swap_source_set_activate(&set) == 0);
	assert(swap_alloc_slot(&set.backend, &slot) == 0);
	context.backend = &set.backend;
	context.slot = slot;
	context.error = EIO;
	assert(pthread_create(&thread, NULL, blocking_read_worker, &context) == 0);
	assert(pthread_mutex_lock(&fake.gate) == 0);
	while (!fake.read_entered)
		assert(pthread_cond_wait(&fake.condition, &fake.gate) == 0);
	assert(pthread_mutex_unlock(&fake.gate) == 0);
	assert(kern_swap_source_set_abort(&set) == EBUSY);
	assert(set.active != 0 && fake.destroys == 0U);
	assert(pthread_mutex_lock(&fake.gate) == 0);
	fake.read_release = 1;
	assert(pthread_cond_broadcast(&fake.condition) == 0);
	assert(pthread_mutex_unlock(&fake.gate) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(context.error == 0);
	assert(kern_swap_source_set_abort(&set) == EBUSY);
	assert(set.active != 0 && fake.destroys == 0U);
	swap_free_slot(&set.backend, slot);
	assert(kern_swap_source_set_abort(&set) == 0);
	assert(set.active == 0 && fake.destroys == 1U);
	assert(pthread_cond_destroy(&fake.condition) == 0);
	assert(pthread_mutex_destroy(&fake.gate) == 0);
}

static void
test_raw_source(void)
{
	struct disk disk;
	struct test_disk_state state;
	struct kern_swap_source source;
	struct kern_swap_source_set set;
	uint8_t input[SWAP_PAGE_SIZE], output[SWAP_PAGE_SIZE];
	uint32_t slot, boundary_slot;
	unsigned index;

	test_disk_init(&disk, &state, 80U, 512U, 24U, DISK_PARTITION);
	make_v2(state.bytes, state.byte_count);
	kern_swap_source_init(&source);
	assert(kern_swap_source_prepare_raw(&disk, 2U, &source) == 0);
	assert(source.slot_count == 2U && source.parameter_index == 2U);
	assert(state.opens == 1U && state.open_handles == 1U && state.refs == 1U);
	kern_swap_source_set_init(&set);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	assert(kern_swap_source_set_activate(&set) == 0);
	assert(swap_alloc_slot(&set.backend, &slot) == 0 &&
	    slot == (2U << SWAP_SLOT_SOURCE_SHIFT));
	for (index = 0; index < sizeof(input); index++)
		input[index] = (uint8_t)(index ^ 0xa5U);
	state.record_count = 0;
	assert(swap_write_page(&set.backend, slot, input) == 0);
	assert(state.record_count == 1U && state.record[0].write != 0 &&
	    state.record[0].block == 8U && state.record[0].count == 8U);
	assert(memcmp(state.bytes + SWAP_PAGE_SIZE, input, sizeof(input)) == 0);
	memset(output, 0, sizeof(output));
	assert(swap_read_page(&set.backend, slot, output) == 0);
	assert(memcmp(output, input, sizeof(output)) == 0);
	assert(state.record_count == 2U && state.record[1].write == 0 &&
	    state.record[1].block == 8U && state.record[1].count == 8U);
	assert(swap_alloc_slot(&set.backend, &boundary_slot) == 0 &&
	    boundary_slot == ((2U << SWAP_SLOT_SOURCE_SHIFT) | 1U));
	state.record_count = 0;
	assert(swap_write_page(&set.backend, boundary_slot, input) == 0);
	assert(state.record_count == 1U && state.record[0].write != 0 &&
	    state.record[0].block == 16U && state.record[0].count == 8U);
	swap_free_slot(&set.backend, slot);
	swap_free_slot(&set.backend, boundary_slot);
	assert(kern_swap_source_set_abort(&set) == 0);
	assert(state.flushes == 1U && state.closes == 1U &&
	    state.releases == 1U && state.open_handles == 0U);
	test_disk_fini(&state);

	test_disk_init(&disk, &state, 81U, 4096U, 3U, DISK_PARTITION);
	make_v2(state.bytes, state.byte_count);
	assert(kern_swap_source_prepare_raw(&disk, 0U, &source) == 0);
	kern_swap_source_set_init(&set);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	assert(kern_swap_source_set_activate(&set) == 0);
	assert(swap_alloc_slot(&set.backend, &slot) == 0 && slot == 0U);
	state.record_count = 0;
	assert(swap_write_page(&set.backend, slot, input) == 0);
	assert(state.record_count == 1U && state.record[0].block == 1U &&
	    state.record[0].count == 1U);
	swap_free_slot(&set.backend, slot);
	assert(kern_swap_source_set_abort(&set) == 0);
	test_disk_fini(&state);

	test_disk_init(&disk, &state, 82U, 4096U, 3U,
	    DISK_PARTITION | DISK_READ_ONLY);
	make_v2(state.bytes, state.byte_count);
	kern_swap_source_init(&source);
	assert(kern_swap_source_prepare_raw(&disk, 0U, &source) == EINVAL);
	assert(state.opens == 0U);
	test_disk_fini(&state);
}

static void
test_file_source(void)
{
	struct disk disk;
	struct test_disk_state disk_state_value;
	struct mount mount;
	struct inode inode;
	struct path path;
	struct test_file_state file_state;
	struct kern_swap_source source, duplicate;
	struct kern_swap_source_set set;
	uint8_t input[SWAP_PAGE_SIZE], output[SWAP_PAGE_SIZE];
	uint32_t slot, boundary_slot;
	unsigned index;

	test_disk_init(&disk, &disk_state_value, 90U, 512U, 512U, 0U);
	memset(&mount, 0, sizeof(mount));
	mount.m_disk = &disk;
	mount.m_type = &fat_filesystem_type;
	memset(&inode, 0, sizeof(inode));
	memset(&file_state, 0, sizeof(file_state));
	inode.i_type = INODE_REG;
	inode.i_size = 3U * SWAP_PAGE_SIZE;
	inode.i_mode = S_IFREG | 0600U;
	inode.i_mount = &mount;
	inode.i_data = &file_state;
	path.p_mount = &mount;
	path.p_inode = &inode;
	make_v2(file_state.header, (uint64_t)inode.i_size);
	file_state.extent_count = 2U;
	file_state.extent[0].file_block = 0U;
	file_state.extent[0].disk_block = 100U;
	file_state.extent[0].count = 10U;
	file_state.extent[1].file_block = 10U;
	file_state.extent[1].disk_block = 200U;
	file_state.extent[1].count = 14U;

	kern_swap_source_init(&source);
	assert(kern_swap_source_prepare_file(&path, 1U, &source) == 0);
	assert(source.slot_count == 2U && (inode.i_flags & INODE_SWAPFILE) != 0);
	assert(file_state.opens == 1U && file_state.closes == 1U &&
	    file_state.refs == 1U && disk_state_value.open_handles == 1U);
	kern_swap_source_init(&duplicate);
	assert(kern_swap_source_prepare_file(&path, 3U, &duplicate) == EBUSY);
	assert(duplicate.data == NULL);
	kern_swap_source_set_init(&set);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	assert(kern_swap_source_set_activate(&set) == 0);
	assert(swap_alloc_slot(&set.backend, &slot) == 0 &&
	    slot == (1U << SWAP_SLOT_SOURCE_SHIFT));
	for (index = 0; index < sizeof(input); index++)
		input[index] = (uint8_t)(index * 3U + 1U);
	disk_state_value.record_count = 0;
	assert(swap_write_page(&set.backend, slot, input) == 0);
	assert(disk_state_value.record_count == 2U);
	assert(disk_state_value.record[0].write != 0 &&
	    disk_state_value.record[0].block == 108U &&
	    disk_state_value.record[0].count == 2U);
	assert(disk_state_value.record[1].write != 0 &&
	    disk_state_value.record[1].block == 200U &&
	    disk_state_value.record[1].count == 6U);
	memset(output, 0, sizeof(output));
	assert(swap_read_page(&set.backend, slot, output) == 0);
	assert(memcmp(input, output, sizeof(input)) == 0);
	assert(swap_alloc_slot(&set.backend, &boundary_slot) == 0 &&
	    boundary_slot == ((1U << SWAP_SLOT_SOURCE_SHIFT) | 1U));
	disk_state_value.record_count = 0;
	assert(swap_write_page(&set.backend, boundary_slot, input) == 0);
	assert(disk_state_value.record_count == 1U &&
	    disk_state_value.record[0].write != 0 &&
	    disk_state_value.record[0].block == 206U &&
	    disk_state_value.record[0].count == 8U);
	swap_free_slot(&set.backend, slot);
	swap_free_slot(&set.backend, boundary_slot);
	assert(kern_swap_source_set_abort(&set) == 0);
	assert((inode.i_flags & INODE_SWAPFILE) == 0);
	assert(file_state.refs == 2U && file_state.releases == 2U);
	assert(disk_state_value.closes == 2U &&
	    disk_state_value.open_handles == 0U);
	assert(kern_swap_source_file_extent_count() == 0U);

	inode.i_mode = S_IFREG | 0400U;
	kern_swap_source_init(&source);
	assert(kern_swap_source_prepare_file(&path, 0U, &source) == EROFS);
	mount.m_flags = MOUNT_READ_ONLY;
	inode.i_mode = S_IFREG | 0600U;
	assert(kern_swap_source_prepare_file(&path, 0U, &source) == EROFS);
	mount.m_flags = 0U;
	disk.d_flags = DISK_READ_ONLY;
	assert(kern_swap_source_prepare_file(&path, 0U, &source) == EROFS);
	disk.d_flags = 0U;
	inode.i_flags = INODE_LOOPFILE;
	assert(kern_swap_source_prepare_file(&path, 0U, &source) == EBUSY);
	inode.i_flags = 0U;
	mount.m_type = NULL;
	assert(kern_swap_source_prepare_file(&path, 0U, &source) == EOPNOTSUPP);
	mount.m_type = &fat_filesystem_type;
	inode.i_size = 129U * SWAP_PAGE_SIZE;
	make_v2(file_state.header, (uint64_t)inode.i_size);
	file_state.extent_count = TEST_FILE_EXTENTS;
	for (index = 0; index < file_state.extent_count; index++) {
		file_state.extent[index].file_block = index;
		file_state.extent[index].disk_block = index + 100U;
		file_state.extent[index].count = 1U;
	}
	assert(kern_swap_source_prepare_file(&path, 0U, &source) == E2BIG);
	assert(file_state.opens == file_state.closes);
	test_disk_fini(&disk_state_value);
}

static void
test_duplicate_and_partial_unwind(void)
{
	struct kern_swap_source_set set, blocker;
	struct kern_swap_source source, second;
	struct fake_source fake = { .id = 4U }, other = { .id = 5U };
	struct disk identity_a = { .d_dev = 100U };
	struct disk identity_b = { .d_dev = 101U };
	struct disk identity_alias = { .d_dev = 100U };
	struct inode inode, inode_alias;
	struct disk raw_a, raw_b, raw_partial;
	struct test_disk_state state_a, state_b, state_partial;

	memset(&inode, 0, sizeof(inode));
	kern_swap_source_set_init(&set);
	make_source(&source, &fake, 0U, 1U, &identity_a, &inode);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	make_source(&second, &other, 1U, 1U, &identity_b, &inode);
	assert(kern_swap_source_set_add(&set, &second) == EEXIST);
	kern_swap_source_init(&second);
	assert(kern_swap_source_set_abort(&set) == 0);

	memset(&inode, 0, sizeof(inode));
	memset(&inode_alias, 0, sizeof(inode_alias));
	inode.i_ino = inode_alias.i_ino = 77U;
	kern_swap_source_set_init(&set);
	make_source(&source, &fake, 0U, 1U, &identity_a, &inode);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	make_source(&second, &other, 1U, 1U, &identity_alias, &inode_alias);
	assert(kern_swap_source_set_add(&set, &second) == EEXIST);
	kern_swap_source_init(&second);
	assert(kern_swap_source_set_abort(&set) == 0);

	/* Each source owns 29 local bits.  The four maximum valid tokens retain
	 * bit 31 clear, while UINT32_MAX remains the sentinel. */
	kern_swap_source_set_init(&set);
	make_source(&source, &fake, 0U, SWAP_SOURCE_MAX_SLOTS + 1U,
	    &identity_a, NULL);
	assert(kern_swap_source_set_add(&set, &source) == EINVAL);
	kern_swap_source_destroy(&source);
	{
		uint32_t token;
		unsigned source_id;
		uint32_t local;

		assert(swap_slot_encode(3U, SWAP_SLOT_LOCAL_MASK, &token) == 0);
		assert(token == SWAP_SLOT_VALID_MASK && token != SWAP_SLOT_NONE);
		assert(swap_slot_decode(token, &source_id, &local) == 0);
		assert(source_id == 3U && local == SWAP_SLOT_LOCAL_MASK);
		assert(swap_slot_encode(4U, 0U, &token) == EINVAL);
		assert(swap_slot_decode(SWAP_SLOT_NONE, NULL, NULL) == EINVAL);
	}

	test_disk_init(&raw_a, &state_a, 110U, 512U, 16U, DISK_PARTITION);
	test_disk_init(&raw_b, &state_b, 110U, 512U, 16U, DISK_PARTITION);
	make_v2(state_a.bytes, state_a.byte_count);
	make_v2(state_b.bytes, state_b.byte_count);
	assert(kern_swap_source_prepare_raw(&raw_a, 0U, &source) == 0);
	assert(kern_swap_source_prepare_raw(&raw_b, 2U, &second) == 0);
	kern_swap_source_set_init(&set);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	assert(kern_swap_source_set_add(&set, &second) == EEXIST);
	kern_swap_source_destroy(&second);
	assert(kern_swap_source_set_abort(&set) == 0);
	assert(state_a.closes == 1U && state_a.releases == 1U);
	assert(state_b.closes == 1U && state_b.releases == 1U);
	test_disk_fini(&state_a);
	test_disk_fini(&state_b);

	/* A competing system backend is rejected before candidate metadata or data
	 * is published.  Caller-owned sources are destroyed without an unnecessary
	 * backing flush, while every claim and reference is still released. */
	kern_swap_source_set_init(&blocker);
	make_source(&source, &fake, 0U, 1U, &identity_a, NULL);
	assert(kern_swap_source_set_add(&blocker, &source) == 0);
	assert(kern_swap_source_set_activate(&blocker) == 0);
	test_disk_init(&raw_partial, &state_partial, 120U, 512U, 16U,
	    DISK_PARTITION);
	make_v2(state_partial.bytes, state_partial.byte_count);
	assert(kern_swap_source_prepare_raw(&raw_partial, 3U, &source) == 0);
	kern_swap_source_set_init(&set);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	assert(kern_swap_source_set_activate(&set) == EBUSY);
	assert(set.active == 0U && set.count == 0U);
	assert(state_partial.flushes == 0U && state_partial.closes == 1U &&
	    state_partial.releases == 1U);
	assert(kern_swap_source_set_abort(&blocker) == 0);
	test_disk_fini(&state_partial);
}

static void
test_native_root_alias_validation(void)
{
	struct kern_swap_source_set set;
	struct kern_swap_source source;
	struct fake_source fake = { .id = 8U };
	struct inode file_inode;
	struct disk leaf_a, leaf_b, root, exact, alias, overlap, adjacent;

	memset(&leaf_a, 0, sizeof(leaf_a));
	memset(&leaf_b, 0, sizeof(leaf_b));
	memset(&root, 0, sizeof(root));
	memset(&exact, 0, sizeof(exact));
	memset(&alias, 0, sizeof(alias));
	memset(&overlap, 0, sizeof(overlap));
	memset(&adjacent, 0, sizeof(adjacent));
	memset(&file_inode, 0, sizeof(file_inode));
	leaf_a.d_dev = 200U;
	leaf_a.d_block_count = 1000U;
	leaf_b.d_dev = 201U;
	leaf_b.d_block_count = 1000U;
	root.d_dev = 210U;
	root.d_parent = &leaf_a;
	root.d_parent_offset = 100U;
	root.d_block_count = 100U;
	exact = root;
	exact.d_dev = 211U;
	alias = root;
	alias.d_dev = 212U;
	overlap.d_dev = 213U;
	overlap.d_parent = &leaf_a;
	overlap.d_parent_offset = 150U;
	overlap.d_block_count = 100U;
	adjacent.d_dev = 214U;
	adjacent.d_parent = &leaf_a;
	adjacent.d_parent_offset = 200U;
	adjacent.d_block_count = 100U;

	kern_swap_source_set_init(&set);
	make_source(&source, &fake, 0U, 1U, &exact, NULL);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	assert(kern_swap_source_set_validate_native_root(&set, &root) == EEXIST);
	assert(kern_swap_source_set_abort(&set) == 0);

	kern_swap_source_set_init(&set);
	make_source(&source, &fake, 0U, 1U, &alias, NULL);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	assert(kern_swap_source_set_validate_native_root(&set, &root) == EEXIST);
	assert(kern_swap_source_set_abort(&set) == 0);

	kern_swap_source_set_init(&set);
	make_source(&source, &fake, 0U, 1U, &overlap, NULL);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	assert(kern_swap_source_set_validate_native_root(&set, &root) == EEXIST);
	assert(kern_swap_source_set_abort(&set) == 0);

	kern_swap_source_set_init(&set);
	make_source(&source, &fake, 0U, 1U, &adjacent, NULL);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	assert(kern_swap_source_set_validate_native_root(&set, &root) == 0);
	assert(kern_swap_source_set_abort(&set) == 0);

	adjacent.d_parent = &leaf_b;
	adjacent.d_parent_offset = 100U;
	kern_swap_source_set_init(&set);
	make_source(&source, &fake, 0U, 1U, &adjacent, NULL);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	assert(kern_swap_source_set_validate_native_root(&set, &root) == 0);
	assert(kern_swap_source_set_abort(&set) == 0);

	/* File-backed swap on the native-root filesystem is intentionally safe. */
	alias.d_parent = &leaf_a;
	alias.d_parent_offset = root.d_parent_offset;
	kern_swap_source_set_init(&set);
	make_source(&source, &fake, 0U, 1U, &alias, &file_inode);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	assert(kern_swap_source_set_validate_native_root(&set, &root) == 0);
	assert(kern_swap_source_set_abort(&set) == 0);
}

static void
test_runtime_source_lifecycle(void)
{
	struct kern_swap_source_set set;
	struct kern_swap_source source;
	struct fake_source boot = { .id = 2U };
	struct fake_source dynamic = { .id = 4U, .flush_error = EIO };
	struct fake_source failed = { .id = 6U };
	struct fake_source replacement = { .id = 5U };
	struct disk boot_disk = { .d_dev = 300U };
	struct disk dynamic_disk = { .d_dev = 301U };
	struct disk failed_disk = { .d_dev = 303U };
	struct disk replacement_disk = { .d_dev = 302U };
	struct swap_source_stats source_stats;
	uint32_t first, second;
	unsigned drains_before;
	unsigned source_id;

	test_drain_calls = 0;
	kern_swap_source_set_init(&set);
	make_source(&source, &boot, 2U, 1U, &boot_disk, NULL);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	assert(kern_swap_source_set_activate(&set) == 0);

	make_source(&source, &dynamic, 3U, 1U, &dynamic_disk, NULL);
	assert(kern_swap_source_set_runtime_add(&set, &source, &source_id) == 0);
	assert(source_id == 0U && source.data == NULL && set.count == 2U);
	assert(swap_alloc_slot(&set.backend, &first) == 0 && first == 0U);
	assert(swap_alloc_slot(&set.backend, &second) == 0 &&
	    second == (2U << SWAP_SLOT_SOURCE_SHIFT));
	swap_free_slot(&set.backend, first);
	swap_free_slot(&set.backend, second);

	/* A failed capacity publication leaves the prepared source caller-owned and
	 * invisible to allocation and aggregate accounting. */
	make_source(&source, &failed, 3U, 1U, &failed_disk, NULL);
	test_commit_resize_error = EAGAIN;
	assert(kern_swap_source_set_runtime_add(&set, &source, NULL) == EAGAIN);
	assert(source.data == &failed && source.parameter_index == 3U &&
	    failed.destroys == 0U && set.count == 2U);
	assert(swap_source_get_stats(&set.backend, 1U, &source_stats) == 0);
	assert(source_stats.state == SWAP_SOURCE_STATE_INACTIVE);
	kern_swap_source_destroy(&source);
	assert(failed.destroys == 1U);

	/* Commitment rejection happens before draining and leaves ACTIVE unchanged.
	 */
	drains_before = test_drain_calls;
	test_commit_resize_error = ENOMEM;
	assert(kern_swap_source_set_runtime_remove(&set, 0U) == ENOMEM);
	assert(test_drain_calls == drains_before);
	assert(swap_source_get_stats(&set.backend, 0U, &source_stats) == 0);
	assert(source_stats.state == SWAP_SOURCE_STATE_ACTIVE &&
	    dynamic.destroys == 0U && test_commit_swap_pages == 2U);

	/* An interrupted drain restores commitment and allocation eligibility. */
	assert(kern_swap_source_set_runtime_remove_cancelable(&set, 0U,
	    test_remove_interrupt, NULL) == EINTR);
	assert(swap_source_get_stats(&set.backend, 0U, &source_stats) == 0);
	assert(source_stats.state == SWAP_SOURCE_STATE_ACTIVE &&
	    dynamic.destroys == 0U && test_commit_swap_pages == 2U);

	/* A page-in/drain error restores commitment and allocation eligibility. */
	test_drain_error = EIO;
	assert(kern_swap_source_set_runtime_remove(&set, 0U) == EIO);
	assert(swap_source_get_stats(&set.backend, 0U, &source_stats) == 0);
	assert(source_stats.state == SWAP_SOURCE_STATE_ACTIVE &&
	    dynamic.destroys == 0U && test_commit_swap_pages == 2U);

	/* A flush failure restores both commitment and allocation eligibility. */
	assert(kern_swap_source_set_runtime_remove(&set, 0U) == EIO);
	assert(swap_source_get_stats(&set.backend, 0U, &source_stats) == 0);
	assert(source_stats.state == SWAP_SOURCE_STATE_ACTIVE &&
	    source_stats.total_slots == 1U && dynamic.destroys == 0U);
	dynamic.flush_error = 0;
	assert(kern_swap_source_set_runtime_remove(&set, 0U) == 0);
	assert(dynamic.destroys == 1U && set.count == 1U);

	/* The lowest free stable ID is reused only after complete removal. */
	make_source(&source, &replacement, 1U, 1U, &replacement_disk, NULL);
	assert(kern_swap_source_set_runtime_add(&set, &source, &source_id) == 0);
	assert(source_id == 0U);
	assert(kern_swap_source_set_runtime_remove(&set, 0U) == 0);
	assert(replacement.destroys == 1U);
	assert(kern_swap_source_set_abort(&set) == 0);
	assert(boot.destroys == 1U && test_commit_swap_pages == 0U);
}

static void
test_empty_runtime_source_lifecycle(void)
{
	struct kern_swap_source_set set;
	struct kern_swap_source source;
	struct fake_source first = { .id = 11U };
	struct fake_source replacement = { .id = 12U };
	struct disk first_disk = { .d_dev = 311U };
	struct disk replacement_disk = { .d_dev = 312U };
	uint32_t slot;
	unsigned source_id;

	assert(test_commit_swap_pages == 0U);
	kern_swap_source_set_init(&set);
	assert(kern_swap_source_set_activate(&set) == 0);
	assert(set.active != 0U && set.count == 0U);
	assert(swap_system_backend() == &set.backend);

	make_source(&source, &first, 3U, 1U, &first_disk, NULL);
	assert(kern_swap_source_set_runtime_add(&set, &source, &source_id) == 0);
	assert(source_id == 0U && source.data == NULL && set.count == 1U);
	assert(test_commit_swap_pages == 1U);
	assert(swap_alloc_slot(&set.backend, &slot) == 0 && slot == 0U);
	swap_free_slot(&set.backend, slot);
	assert(kern_swap_source_set_runtime_remove(&set, 0U) == 0);
	assert(set.active != 0U && set.count == 0U && first.destroys == 1U);
	assert(test_commit_swap_pages == 0U);
	assert(swap_system_backend() == &set.backend);

	/* Complete removal releases the stable numeric ID for reuse. */
	make_source(&source, &replacement, 2U, 1U, &replacement_disk, NULL);
	assert(kern_swap_source_set_runtime_add(&set, &source, &source_id) == 0);
	assert(source_id == 0U && set.count == 1U);
	assert(kern_swap_source_set_runtime_remove(&set, 0U) == 0);
	assert(set.count == 0U && replacement.destroys == 1U);
	assert(test_commit_swap_pages == 0U);
	assert(kern_swap_source_set_abort(&set) == 0);
	assert(set.active == 0U && swap_system_backend() == NULL);
}

struct runtime_remove_context {
	struct kern_swap_source_set *set;
	unsigned source_id;
	int error;
};

static void *
runtime_remove_worker(void *argument)
{
	struct runtime_remove_context *context = argument;

	context->error = kern_swap_source_set_runtime_remove(context->set,
	    context->source_id);
	return NULL;
}

static void
test_runtime_lifecycle_gate(void)
{
	struct kern_swap_source_set set;
	struct kern_swap_source source, contender;
	struct fake_source fake = { .id = 8U };
	struct fake_source other = { .id = 10U };
	struct disk identity = { .d_dev = 308U };
	struct disk other_identity = { .d_dev = 310U };
	struct runtime_remove_context context;
	pthread_t thread;

	kern_swap_source_set_init(&set);
	make_source(&source, &fake, 0U, 1U, &identity, NULL);
	assert(kern_swap_source_set_add(&set, &source) == 0);
	assert(kern_swap_source_set_activate(&set) == 0);
	test_drain_block = 1;
	test_drain_entered = 0;
	test_drain_release = 0;
	context.set = &set;
	context.source_id = 0U;
	context.error = EIO;
	assert(pthread_create(&thread, NULL, runtime_remove_worker, &context) == 0);
	assert(pthread_mutex_lock(&test_drain_gate) == 0);
	while (!test_drain_entered)
		assert(pthread_cond_wait(&test_drain_condition,
		    &test_drain_gate) == 0);
	assert(pthread_mutex_unlock(&test_drain_gate) == 0);
	/* abort must not race range/count ownership against a removal transaction. */
	assert(kern_swap_source_set_abort(&set) == EBUSY);
	assert(set.active != 0 && set.count == 1U && fake.destroys == 0U);
	make_source(&contender, &other, 3U, 1U, &other_identity, NULL);
	assert(kern_swap_source_set_runtime_add(&set, &contender, NULL) == EBUSY);
	assert(contender.data == &other && other.destroys == 0U);
	kern_swap_source_destroy(&contender);
	assert(other.destroys == 1U);
	assert(pthread_mutex_lock(&test_drain_gate) == 0);
	test_drain_release = 1;
	assert(pthread_cond_broadcast(&test_drain_condition) == 0);
	assert(pthread_mutex_unlock(&test_drain_gate) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(context.error == 0 && set.count == 0U && fake.destroys == 1U);
	test_drain_block = 0;
	assert(kern_swap_source_set_abort(&set) == 0);
	assert(set.active == 0 && test_commit_swap_pages == 0U);
}

int
main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s GENERATED-V1 GENERATED-V2\n", argv[0]);
		return 2;
	}
	test_headers();
	test_generated_header(argv[1], 1U);
	test_generated_header(argv[2], 2U);
	test_prepared_publication();
	test_sparse_cardinality_and_order();
	test_aggregate_and_concurrency();
	test_concurrent_lifecycle();
	test_raw_source();
	test_file_source();
	test_duplicate_and_partial_unwind();
	test_native_root_alias_validation();
	test_runtime_source_lifecycle();
	test_empty_runtime_source_lifecycle();
	test_runtime_lifecycle_gate();
	puts("BR-T45 swap source: PASS");
	return 0;
}
