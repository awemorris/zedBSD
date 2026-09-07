/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#define ASYNC_ENDPOINTS 4U
#define ASYNC_SLOTS 4U
#define ASYNC_QUEUE_LIMIT 2U
#define ASYNC_OFF 0U
#define ASYNC_BUILDING 1U
#define ASYNC_LIVE 2U
#define ASYNC_STOPPING 3U
#define REQUEST_FREE 0U
#define REQUEST_PREPARING 1U
#define REQUEST_READY 2U
#define REQUEST_QUEUED 3U
#define REQUEST_RUNNING 4U
#define REQUEST_DONE 5U
#define REQUEST_RELEASING 6U

struct bio_async_endpoint;
struct bio_async_request {
	struct bio bio;
	struct bio_async_endpoint *endpoint;
	struct bio_async_request *next;
	struct backing_mutation_guard guard;
	const struct backing_claim *authorization;
	struct disk *disk;
	struct disk *cache_token;
	struct io_context context;
	refcount_t refs;
	void *payload;
	bio_async_callback callback;
	void *argument;
	uint64_t epoch;
	size_t transferred;
	int error;
	unsigned state;
	unsigned callback_active;
};

struct bio_async_endpoint {
	struct spinlock lock;
	struct wait_queue wake;
	struct disk *leaf;
	struct disk *cache_token;
	struct hal_pmem memory;
	struct bio_async_request *requests;
	struct bio_async_request *head;
	struct bio_async_request *tail;
	unsigned state;
	unsigned used;
	unsigned queued;
	unsigned started;
};

static atomic_uint_t async_initialized;
static struct spinlock async_registry;
static struct bio_async_endpoint async_endpoints[ASYNC_ENDPOINTS];

static int async_initialize(void);
static void async_worker(void *argument);
static void async_complete(struct bio *bio);
static void async_unlink_locked(struct bio_async_endpoint *endpoint, struct bio_async_request *request);

/* Initializes synchronization only; device buffers are allocated on enable. */
static int
async_initialize(void)
{
	unsigned expected;
	unsigned index;

	if (atomic_load_acquire(&async_initialized) == 2)
		return 0;
	expected = 0;
	if (!atomic_compare_exchange(&async_initialized, &expected, 1))
		return EAGAIN;
	spin_init(&async_registry, LOCK_RANK_BIO_REGISTRY, "BIO endpoint registry");
	for (index = 0; index < ASYNC_ENDPOINTS; index++) {
		spin_init(&async_endpoints[index].lock, LOCK_RANK_BIO_QUEUE, "BIO worker queue");
		waitq_init(&async_endpoints[index].wake, "BIO worker");
	}
	atomic_store_release(&async_initialized, 2);
	return 0;
}

int
bio_async_enable(struct disk *disk)
{
	struct bio_async_endpoint *endpoint;
	struct bio_async_endpoint *candidate;
	struct disk *token;
	struct thread *thread;
	struct hal_pmem_request allocation;
	struct hal_pmem memory;
	unsigned long registry_irq;
	unsigned long irq;
	unsigned index;
	size_t controls;
	int error;

	error = async_initialize();
	if (error != 0)
		return error;
	error = disk_cache_acquire(disk, &token);
	if (error != 0)
		return error;
	endpoint = NULL;
	registry_irq = spin_lock_irqsave(&async_registry);
	for (index = 0; index < ASYNC_ENDPOINTS; index++) {
		candidate = &async_endpoints[index];
		if (candidate->leaf == token && candidate->state != ASYNC_OFF) {
			error = candidate->state == ASYNC_LIVE ? 0 : EBUSY;
			spin_unlock_irqrestore(&async_registry, registry_irq);
			disk_cache_release(token);
			return error;
		}
		if (candidate->state == ASYNC_OFF && endpoint == NULL)
			endpoint = candidate;
	}
	if (endpoint == NULL) {
		spin_unlock_irqrestore(&async_registry, registry_irq);
		disk_cache_release(token);
		return EAGAIN;
	}
	irq = spin_lock_irqsave(&endpoint->lock);
	endpoint->leaf = token;
	endpoint->state = ASYNC_BUILDING;
	spin_unlock_irqrestore(&endpoint->lock, irq);
	spin_unlock_irqrestore(&async_registry, registry_irq);

	controls = (ASYNC_SLOTS * sizeof(struct bio_async_request) + ZEDBSD_PAGE_SIZE - 1U) &
	    ~(size_t)(ZEDBSD_PAGE_SIZE - 1U);
	memset(&allocation, 0, sizeof(allocation));
	allocation.size = controls + ASYNC_SLOTS * KERN_IO_BATCH_MAX;
	allocation.alignment = ZEDBSD_PAGE_SIZE;
	allocation.type = HAL_PMEM_TYPE_RAM;
	allocation.paddr = HAL_PMEM_PADDR_ANY;
	memset(&memory, 0, sizeof(memory));
	error = hal_pmem_alloc(&allocation, &memory);
	if (error != HAL_OK || memory.vaddr == NULL || memory.size < allocation.size) {
		error = ENOMEM;
		goto failed_memory;
	}
	error = cache_memory_reserve(CACHE_MEMORY_IO_POOL, memory.size, 0);
	if (error != 0)
		goto failed_memory;
	cache_memory_commit(CACHE_MEMORY_IO_POOL, memory.size);
	memset(memory.vaddr, 0, memory.size);
	if (!endpoint->started) {
		error = kthread_create(async_worker, endpoint, SCHED_PRIORITY_DEFAULT, &thread);
		if (error != 0) {
			cache_memory_release(CACHE_MEMORY_IO_POOL, memory.size);
			goto failed_memory;
		}
		endpoint->started = 1;
		thread_start(thread);
	}
	registry_irq = spin_lock_irqsave(&async_registry);
	irq = spin_lock_irqsave(&endpoint->lock);
	endpoint->memory = memory;
	endpoint->cache_token = token;
	endpoint->requests = memory.vaddr;
	for (index = 0; index < ASYNC_SLOTS; index++) {
		endpoint->requests[index].endpoint = endpoint;
		endpoint->requests[index].payload = (char *)memory.vaddr + controls + index * KERN_IO_BATCH_MAX;
	}
	endpoint->state = ASYNC_LIVE;
	spin_unlock_irqrestore(&endpoint->lock, irq);
	spin_unlock_irqrestore(&async_registry, registry_irq);
	return 0;

failed_memory:
	if (memory.size != 0 && hal_pmem_free(&memory) != HAL_OK)
		HAL_FATAL("BIO endpoint rollback failed");
	registry_irq = spin_lock_irqsave(&async_registry);
	irq = spin_lock_irqsave(&endpoint->lock);
	endpoint->leaf = NULL;
	endpoint->state = ASYNC_OFF;
	spin_unlock_irqrestore(&endpoint->lock, irq);
	spin_unlock_irqrestore(&async_registry, registry_irq);
	disk_cache_release(token);
	return error;
}

int
bio_async_disable(struct disk *disk)
{
	struct bio_async_endpoint *endpoint;
	struct disk *leaf;
	struct disk *token;
	struct hal_pmem memory;
	unsigned long registry_irq;
	unsigned long irq;
	unsigned index;
	int error;

	if (disk == NULL || atomic_load_acquire(&async_initialized) != 2)
		return EINVAL;
	leaf = disk_leaf(disk);
	registry_irq = spin_lock_irqsave(&async_registry);
	for (index = 0; index < ASYNC_ENDPOINTS; index++) {
		endpoint = &async_endpoints[index];
		if (endpoint->leaf != leaf)
			continue;
		irq = spin_lock_irqsave(&endpoint->lock);
		if (endpoint->state != ASYNC_LIVE || endpoint->used != 0) {
			spin_unlock_irqrestore(&endpoint->lock, irq);
			spin_unlock_irqrestore(&async_registry, registry_irq);
			return EBUSY;
		}
		endpoint->state = ASYNC_STOPPING;
		memory = endpoint->memory;
		token = endpoint->cache_token;
		spin_unlock_irqrestore(&endpoint->lock, irq);
		spin_unlock_irqrestore(&async_registry, registry_irq);
		error = hal_pmem_free(&memory);
		registry_irq = spin_lock_irqsave(&async_registry);
		irq = spin_lock_irqsave(&endpoint->lock);
		if (error != HAL_OK) {
			endpoint->state = ASYNC_LIVE;
			spin_unlock_irqrestore(&endpoint->lock, irq);
			spin_unlock_irqrestore(&async_registry, registry_irq);
			return EIO;
		}
		cache_memory_release(CACHE_MEMORY_IO_POOL, endpoint->memory.size);
		memset(&endpoint->memory, 0, sizeof(endpoint->memory));
		endpoint->requests = NULL;
		endpoint->cache_token = NULL;
		endpoint->leaf = NULL;
		endpoint->state = ASYNC_OFF;
		spin_unlock_irqrestore(&endpoint->lock, irq);
		spin_unlock_irqrestore(&async_registry, registry_irq);
		disk_cache_release(token);
		return 0;
	}
	spin_unlock_irqrestore(&async_registry, registry_irq);
	return ENOENT;
}

int
bio_async_prepare(struct disk *disk, enum bio_op op, uint64_t block,
    uint32_t count, const void *write_data, const struct backing_claim *claim,
    const struct io_context *context, bio_async_callback callback, void *argument,
    struct bio_async_request **result)
{
	struct bio_async_request *request;
	struct bio_async_endpoint *endpoint;
	struct io_context inherited;
	struct disk *token;
	unsigned long irq;
	unsigned index;
	unsigned slot;
	size_t bytes;
	int error;

	if (result == NULL)
		return EINVAL;
	*result = NULL;
	if (disk == NULL || disk->d_block_size == 0 ||
	    (op != BIO_READ && op != BIO_WRITE && op != BIO_FLUSH) ||
	    (op == BIO_WRITE && write_data == NULL) ||
	    (op != BIO_WRITE && write_data != NULL) ||
	    (op == BIO_FLUSH ? count != 0 : count == 0) ||
	    count > KERN_IO_BATCH_MAX / disk->d_block_size)
		return EINVAL;
	error = io_context_child(&inherited, context, IO_CONTEXT_DRAIN);
	if (error != 0)
		return error;
	if (atomic_load_acquire(&async_initialized) != 2)
		return EOPNOTSUPP;
	error = disk_cache_acquire(disk, &token);
	if (error != 0)
		return error;
	if ((disk->d_max_transfer_blocks != 0 && count > disk->d_max_transfer_blocks) ||
	    (token->d_max_transfer_blocks != 0 && count > token->d_max_transfer_blocks)) {
		disk_cache_release(token);
		return E2BIG;
	}
	request = NULL;
	for (index = 0; index < ASYNC_ENDPOINTS && request == NULL; index++) {
		endpoint = &async_endpoints[index];
		irq = spin_lock_irqsave(&endpoint->lock);
		if (endpoint->state == ASYNC_LIVE && endpoint->leaf == token) {
			for (slot = 0; slot < ASYNC_SLOTS; slot++) {
				if (endpoint->requests[slot].state == REQUEST_FREE) {
					request = &endpoint->requests[slot];
					request->state = REQUEST_PREPARING;
					endpoint->used++;
					break;
				}
			}
		}
		spin_unlock_irqrestore(&endpoint->lock, irq);
	}
	if (request == NULL) {
		disk_cache_release(token);
		return EAGAIN;
	}
	endpoint = request->endpoint;
	refcount_init(&request->refs, 1);
	memset(&request->bio, 0, sizeof(request->bio));
	memset(&request->guard, 0, sizeof(request->guard));
	request->disk = disk;
	disk_ref(disk);
	request->cache_token = token;
	request->context = inherited;
	request->authorization = claim;
	if (inherited.origin_inode != NULL)
		inode_ref(inherited.origin_inode);
	backing_claim_ref(inherited.claim);
	backing_claim_ref(claim);
	request->callback = callback;
	request->argument = argument;
	request->next = NULL;
	request->error = 0;
	request->transferred = 0;
	request->callback_active = 0;
	irq = spin_lock_irqsave(&token->d_lock);
	request->epoch = token->d_media_epoch;
	spin_unlock_irqrestore(&token->d_lock, irq);
	if (request->epoch == UINT64_MAX) {
		bio_async_release(request);
		return EOVERFLOW;
	}
	if (op == BIO_WRITE) {
		error = backing_mutation_begin_disk(disk, block, count, claim, &request->guard);
		if (error != 0) {
			bio_async_release(request);
			return error;
		}
	}
	bytes = (size_t)count * disk->d_block_size;
	if (op == BIO_WRITE)
		memcpy(request->payload, write_data, bytes);
	request->bio.b_context = inherited;
	request->bio.b_op = op;
	request->bio.b_block = block;
	request->bio.b_block_count = count;
	request->bio.b_data = op == BIO_FLUSH ? NULL : request->payload;
	request->bio.b_done = async_complete;
	irq = spin_lock_irqsave(&endpoint->lock);
	request->state = REQUEST_READY;
	spin_unlock_irqrestore(&endpoint->lock, irq);
	*result = request;
	return 0;
}

int
bio_async_submit(struct bio_async_request *request)
{
	struct bio_async_endpoint *endpoint;
	unsigned long irq;
	int error;

	if (request == NULL)
		return EINVAL;
	endpoint = request->endpoint;
	irq = spin_lock_irqsave(&endpoint->lock);
	if (request->state != REQUEST_READY || endpoint->state != ASYNC_LIVE) {
		spin_unlock_irqrestore(&endpoint->lock, irq);
		return EINVAL;
	}
	if (endpoint->queued == ASYNC_QUEUE_LIMIT) {
		spin_unlock_irqrestore(&endpoint->lock, irq);
		return EAGAIN;
	}
	error = bio_admit(request->disk, &request->bio);
	if (error != 0) {
		spin_unlock_irqrestore(&endpoint->lock, irq);
		return error;
	}
	bio_async_ref(request);
	request->state = REQUEST_QUEUED;
	if (endpoint->tail != NULL)
		endpoint->tail->next = request;
	else
		endpoint->head = request;
	endpoint->tail = request;
	endpoint->queued++;
	waitq_wake_all(&endpoint->wake);
	spin_unlock_irqrestore(&endpoint->lock, irq);
	return 0;
}

static void
async_unlink_locked(struct bio_async_endpoint *endpoint, struct bio_async_request *request)
{
	struct bio_async_request **link;
	struct bio_async_request *previous;

	previous = NULL;
	link = &endpoint->head;
	while (*link != request) {
		if (*link == NULL)
			HAL_FATAL("missing queued BIO");
		previous = *link;
		link = &previous->next;
	}
	*link = request->next;
	if (endpoint->tail == request)
		endpoint->tail = previous;
	request->next = NULL;
	endpoint->queued--;
}

int
bio_async_cancel(struct bio_async_request *request)
{
	struct bio_async_endpoint *endpoint;
	unsigned long irq;
	int error;

	if (request == NULL)
		return EINVAL;
	endpoint = request->endpoint;
	irq = spin_lock_irqsave(&endpoint->lock);
	if (request->state != REQUEST_QUEUED) {
		error = request->state == REQUEST_DONE ? EALREADY : EBUSY;
		spin_unlock_irqrestore(&endpoint->lock, irq);
		return error;
	}
	async_unlink_locked(endpoint, request);
	request->state = REQUEST_RUNNING;
	spin_unlock_irqrestore(&endpoint->lock, irq);
	bio_complete(&request->bio, ECANCELED, 0);
	bio_async_release(request);
	return 0;
}

static void
async_complete(struct bio *bio)
{
	struct bio_async_request *request;
	struct bio_async_endpoint *endpoint;
	bio_async_callback callback;
	void *argument;
	unsigned long irq;
	int error;

	request = (struct bio_async_request *)bio;
	endpoint = request->endpoint;
	bio_async_ref(request);
	error = bio->b_error;
	if (error == 0 && bio->b_op != BIO_FLUSH &&
	    bio->b_transferred != (uint64_t)bio->b_block_count * request->disk->d_block_size)
		error = EIO;
	irq = spin_lock_irqsave(&request->cache_token->d_lock);
	if (error == 0 && (request->cache_token->d_state != DISK_LIVE ||
	    request->epoch != request->cache_token->d_media_epoch))
		error = ESTALE;
	spin_unlock_irqrestore(&request->cache_token->d_lock, irq);
	backing_mutation_end(&request->guard);
	irq = spin_lock_irqsave(&endpoint->lock);
	request->error = error;
	request->transferred = bio->b_transferred;
	callback = request->callback;
	argument = request->argument;
	request->callback_active = 1;
	request->state = REQUEST_DONE;
	waitq_wake_all(&endpoint->wake);
	spin_unlock_irqrestore(&endpoint->lock, irq);
	if (callback != NULL)
		callback(request, argument);
	/* The worker retains its reference until callback return and this handoff. */
	bio_async_release(request);
	irq = spin_lock_irqsave(&endpoint->lock);
	request->callback_active = 0;
	waitq_wake_all(&endpoint->wake);
	spin_unlock_irqrestore(&endpoint->lock, irq);
}

static void
async_worker(void *argument)
{
	struct bio_async_endpoint *endpoint;
	struct bio_async_request *request;
	unsigned long irq;
	unsigned long disk_irq;
	uint64_t sequence;
	int error;

	endpoint = argument;
	for (;;) {
		irq = spin_lock_irqsave(&endpoint->lock);
		while (endpoint->head == NULL) {
			sequence = waitq_sequence(&endpoint->wake);
			(void)waitq_sleep(&endpoint->wake, &endpoint->lock, sequence, 0, 0);
		}
		request = endpoint->head;
		async_unlink_locked(endpoint, request);
		request->state = REQUEST_RUNNING;
		spin_unlock_irqrestore(&endpoint->lock, irq);
		disk_irq = spin_lock_irqsave(&request->cache_token->d_lock);
		error = request->cache_token->d_state != DISK_LIVE ? ENXIO :
		    (request->epoch != request->cache_token->d_media_epoch ? ESTALE : 0);
		spin_unlock_irqrestore(&request->cache_token->d_lock, disk_irq);
		if (error == 0)
			error = bio_dispatch(&request->bio);
		if (error != 0)
			bio_complete(&request->bio, error, 0);
		irq = spin_lock_irqsave(&endpoint->lock);
		while (request->state != REQUEST_DONE || request->callback_active) {
			sequence = waitq_sequence(&endpoint->wake);
			(void)waitq_sleep(&endpoint->wake, &endpoint->lock, sequence, 0, 0);
		}
		spin_unlock_irqrestore(&endpoint->lock, irq);
		bio_async_release(request);
	}
}

int
bio_async_wait(struct bio_async_request *request)
{
	struct bio_async_endpoint *endpoint;
	unsigned long irq;
	uint64_t sequence;
	int error;

	if (request == NULL)
		return EINVAL;
	endpoint = request->endpoint;
	irq = spin_lock_irqsave(&endpoint->lock);
	while (request->state != REQUEST_DONE) {
		if (request->state != REQUEST_QUEUED && request->state != REQUEST_RUNNING) {
			spin_unlock_irqrestore(&endpoint->lock, irq);
			return EINVAL;
		}
		sequence = waitq_sequence(&endpoint->wake);
		error = waitq_sleep(&endpoint->wake, &endpoint->lock, sequence, 0, 0);
		if (error != 0 && error != EAGAIN) {
			spin_unlock_irqrestore(&endpoint->lock, irq);
			return error;
		}
	}
	error = request->error;
	spin_unlock_irqrestore(&endpoint->lock, irq);
	return error;
}

int
bio_async_result(struct bio_async_request *request, const void **data, size_t *transferred)
{
	unsigned long irq;
	int error;

	if (request == NULL || data == NULL || transferred == NULL)
		return EINVAL;
	*data = NULL;
	*transferred = 0;
	irq = spin_lock_irqsave(&request->endpoint->lock);
	if (request->state != REQUEST_DONE) {
		spin_unlock_irqrestore(&request->endpoint->lock, irq);
		return EAGAIN;
	}
	error = request->error;
	*transferred = request->transferred;
	if (error == 0 && request->bio.b_op == BIO_READ)
		*data = request->payload;
	spin_unlock_irqrestore(&request->endpoint->lock, irq);
	return error;
}

void
bio_async_ref(struct bio_async_request *request)
{
	if (request != NULL)
		refcount_get(&request->refs);
}

void
bio_async_release(struct bio_async_request *request)
{
	struct bio_async_endpoint *endpoint;
	unsigned long irq;

	if (request == NULL || !refcount_put(&request->refs))
		return;
	endpoint = request->endpoint;
	irq = spin_lock_irqsave(&endpoint->lock);
	if (request->state == REQUEST_QUEUED || request->state == REQUEST_RUNNING)
		HAL_FATAL("releasing active BIO ownership");
	request->state = REQUEST_RELEASING;
	spin_unlock_irqrestore(&endpoint->lock, irq);
	backing_mutation_end(&request->guard);
	backing_claim_release((struct backing_claim *)request->authorization);
	backing_claim_release((struct backing_claim *)request->context.claim);
	if (request->context.origin_inode != NULL)
		inode_release(request->context.origin_inode);
	disk_release(request->disk);
	disk_cache_release(request->cache_token);
	irq = spin_lock_irqsave(&endpoint->lock);
	request->state = REQUEST_FREE;
	endpoint->used--;
	spin_unlock_irqrestore(&endpoint->lock, irq);
}
