/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The page cache's own disk I/O: one run of object pages moved to or from
 * consecutive sectors of a disk as one request, started without waiting and
 * finished later in process context.  A disk whose driver moves a list of
 * physical pages gets the pages themselves; any other disk gets one
 * contiguous bounce the run is copied through, so that a long run is still
 * one request and not one request a page.  The caller owns the pages (BUSY)
 * from the start to the finish.
 */

#include "kern/vm-object.h"
#include "kern/buf.h"
#include "kern/disk.h"
#include "kern/inode.h"
#include "kern/io-stats.h"
#include "kern/kmem.h"
#include "kern/mount.h"
#include "kern/page.h"
#include <uapi/errno.h>
#include <hal/hal.h>
#include <kern/pmem.h>
#include <kern/kcrt.h>
#include "object-internal.h"

/*
 * The write-back requests of one call kept in flight together, and the
 * mapped runs they write: a run's end is reported to the file system
 * (data_written) once every request of it has finished, in file order.
 * The requests finish in the order they started.
 */
struct write_queue {
	struct inode *inode;
	struct vm_object_io *ios;
	unsigned slots;
	unsigned head;
	unsigned busy;
	unsigned slot_run[VM_OBJECT_IO_WRITE_DEPTH];
	struct inode_data_extent runs[VM_OBJECT_IO_WRITE_DEPTH + 1U];
	unsigned run_left[VM_OBJECT_IO_WRITE_DEPTH + 1U];
	int run_error[VM_OBJECT_IO_WRITE_DEPTH + 1U];
	unsigned run_open[VM_OBJECT_IO_WRITE_DEPTH + 1U];
	unsigned run_head;
	unsigned run_count;
	int error;
};

static unsigned io_pages_per_request(struct disk *disk, int *vector);
static int io_start_bounce(struct vm_object_io *io, struct disk *disk, uint64_t block, uint32_t count, const struct io_context *context);
static void io_copy_to_bounce(struct vm_object_io *io);
static void io_copy_from_bounce(struct vm_object_io *io);
static void io_release(struct vm_object_io *io);
static unsigned extent_pages(const struct inode_data_extent *extent, uint64_t remaining);
static int read_direct(struct vm_object_io *ios, unsigned slots, struct disk *disk, uint64_t sector, struct vm_object_page *const *pages, unsigned count, unsigned limit, const struct io_context *context);
static int read_fallback(vm_object_read_fallback fallback, void *argument, off_t offset, struct vm_object_page *const *pages, unsigned count, uint64_t remaining);
static int write_queue_open(struct write_queue *queue, struct inode *inode, unsigned count, unsigned limit);
static void write_queue_close(struct write_queue *queue);
static int write_queue_run(struct write_queue *queue, struct disk *disk, const struct inode_data_extent *extent, struct vm_object_page *const *pages, unsigned count, unsigned limit, const struct io_context *context);
static void write_queue_finish_one(struct write_queue *queue);
static int write_queue_drain(struct write_queue *queue);
static void write_queue_report(struct write_queue *queue);
static int write_fallback(vm_object_write_fallback fallback, void *argument, off_t offset, struct vm_object_page *const *pages, unsigned count, uint64_t remaining);
static void zero_pages(struct vm_object_page *const *pages, unsigned count);
static void write_ordered_end(struct inode *inode, const struct inode_data_extent *extent, int error);

/*
 * Reports how many pages one page-cache request may move on a disk, and 0
 * when the disk cannot take a page-sized request at all.
 */
unsigned
vm_object_io_max_pages(
	struct disk *disk)
{
	unsigned pages;
	int vector;

	/* Measures the request the disk takes, by page list or by bounce. */
	pages = io_pages_per_request(disk, &vector);

	/* Reports the number of pages. */
	return pages;
}

/*
 * Starts moving a run of object pages to or from the disk sectors that
 * begin at sector (512-byte units), without waiting for the request.
 *
 * The pages are consecutive in the file and in the run of sectors; the
 * caller holds them BUSY until vm_object_io_finish() and keeps io alive as
 * long.  The disk is admitted as a cached access (against detach and
 * reload) from the start to the finish.  A start that fails holds nothing
 * and needs no finish.
 */
int
vm_object_io_start(
	struct vm_object_io *io,
	struct disk *disk,
	enum bio_op op,
	uint64_t sector,
	struct vm_object_page *const *pages,
	unsigned page_count,
	const struct io_context *context)
{
	uint64_t byte_offset;
	uint64_t block;
	uint32_t count;
	unsigned limit;
	unsigned index;
	int vector;
	int error;

	/* Rejects a missing operand or an operation that moves no data. */
	if (io == NULL || disk == NULL || pages == NULL || page_count == 0)
		return EINVAL;
	if (op != BIO_READ && op != BIO_WRITE)
		return EINVAL;
	kern_memset(io, 0, sizeof(*io));

	/* Refuses a run longer than one request of this disk. */
	limit = io_pages_per_request(disk, &vector);
	if (limit == 0)
		return EOPNOTSUPP;
	if (page_count > limit)
		return E2BIG;

	/* Turns the sector into a block of the disk; the run must start on one. */
	if (sector > UINT64_MAX / DISK_SECTOR_SIZE)
		return EOVERFLOW;
	byte_offset = sector * DISK_SECTOR_SIZE;
	if (byte_offset % disk->d_block_size != 0)
		return EINVAL;
	block = byte_offset / disk->d_block_size;
	count = (uint32_t)((uint64_t)page_count * PAGE_SIZE / disk->d_block_size);

	/* Records the run. */
	io->op = op;
	io->page_count = page_count;
	for (index = 0; index < page_count; index++) {
		io->pages[index] = pages[index];
		io->addresses[index] = pages[index]->pmem.paddr;
	}

	/* Admits the disk as a cached access until the finish. */
	error = disk_cache_acquire(disk, &io->token);
	if (error != 0)
		return error;

	/*
	 * A disk that moves page lists takes the pages themselves; any other
	 * disk gets one contiguous bounce for the whole run.
	 */
	if (vector) {
		error = disk_transfer_pages_start(&io->transfer, disk, op, block, count,
		    io->addresses, page_count, NULL, context);
	} else {
		error = io_start_bounce(io, disk, block, count, context);
	}

	/* Gives back what a refused start held. */
	if (error != 0) {
		io_release(io);
		return error;
	}

	/* Succeeded: the request is in flight and the caller owes a finish. */
	io->started = 1;
	return 0;
}

/*
 * Waits for a started page-cache request, fills the pages of a bounced read,
 * gives the bounce and the disk admission back and reports the result.  The
 * pages hold the disk's data only when it succeeds.  Process context.
 */
int
vm_object_io_finish(
	struct vm_object_io *io)
{
	size_t bytes;
	int bounced;
	int error;

	/* Rejects a record that holds no started request. */
	if (io == NULL || !io->started)
		return EINVAL;

	/* Waits for the disk and checks that it moved the whole run. */
	error = disk_transfer_finish(&io->transfer, NULL);
	io->started = 0;

	/* A bounced read hands the disk's data to the pages. */
	bounced = io->bounce.size != 0;
	if (error == 0 && bounced && io->op == BIO_READ)
		io_copy_from_bounce(io);

	/* Gives the bounce and the admission back. */
	io_release(io);

	/* Reports why the request failed. */
	if (error != 0)
		return error;

	/* Counts the request by direction and by the way the bytes went. */
	bytes = (size_t)io->page_count * PAGE_SIZE;
	if (io->op == BIO_READ)
		io_stats_record(IO_OBJECT_PAGE_READ, bytes);
	else
		io_stats_record(IO_OBJECT_PAGE_WRITE, bytes);
	if (bounced)
		io_stats_record(IO_OBJECT_PAGE_BOUNCE, bytes);
	else
		io_stats_record(IO_OBJECT_PAGE_VECTOR, bytes);

	/* Succeeded: the run moved. */
	return 0;
}

/*
 * The pages one request of the disk takes, at most VM_OBJECT_IO_MAX_PAGES;
 * vector says whether they go by page list.  Zero for a disk whose blocks
 * do not divide a page or whose driver takes less than a page.
 */
static unsigned
io_pages_per_request(
	struct disk *disk,
	int *vector)
{
	uint32_t blocks_per_page;
	uint32_t limit;
	unsigned pages;

	/* A disk without blocks that divide a page takes no page. */
	*vector = 0;
	if (disk == NULL || disk->d_block_size == 0)
		return 0;
	if (disk->d_block_size > PAGE_SIZE || PAGE_SIZE % disk->d_block_size != 0)
		return 0;
	blocks_per_page = PAGE_SIZE / disk->d_block_size;

	/* The page-list limit when the driver moves page lists, else the driver's limit. */
	limit = disk_transfer_pages_limit(disk);
	if (limit != 0) {
		*vector = 1;
	} else {
		limit = disk->d_max_transfer_blocks;
		if (limit == 0)
			limit = UINT32_MAX;
	}

	/* Whole pages within that limit, and no more than one bounce holds. */
	pages = (unsigned)(limit / blocks_per_page);
	if (pages > VM_OBJECT_IO_MAX_PAGES)
		pages = VM_OBJECT_IO_MAX_PAGES;

	/* Reports the number of pages. */
	return pages;
}

/* Starts the run through a contiguous bounce; the caller releases it on a failure. */
static int
io_start_bounce(
	struct vm_object_io *io,
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const struct io_context *context)
{
	void *bounce;
	int error;

	/* Allocates one physical run the size of the pages. */
	error = kern_pmem_alloc((size_t)io->page_count * PAGE_SIZE, PAGE_SIZE, &io->bounce);
	if (error != 0)
		return ENOMEM;

	/* A write copies the pages into the bounce before the request. */
	if (io->op == BIO_WRITE)
		io_copy_to_bounce(io);

	/* Sends the bounce as one request. */
	bounce = kern_pmem_to_kernel(io->bounce.paddr);
	error = disk_transfer_data_start(&io->transfer, disk, io->op, block, count, bounce, NULL, context);
	if (error != 0)
		return error;

	/* Succeeded: the bounce is the driver's until the finish. */
	return 0;
}

/* Copies the run's pages into the bounce, in order. */
static void
io_copy_to_bounce(
	struct vm_object_io *io)
{
	uint8_t *bounce;
	unsigned index;

	/* Lays the pages out one after another. */
	bounce = kern_pmem_to_kernel(io->bounce.paddr);
	for (index = 0; index < io->page_count; index++)
		kern_memcpy(bounce + (size_t)index * PAGE_SIZE, hal_pmem_to_kernel(io->pages[index]->pmem.paddr), PAGE_SIZE);
}

/* Copies the bounce out to the run's pages, in order. */
static void
io_copy_from_bounce(
	struct vm_object_io *io)
{
	const uint8_t *bounce;
	unsigned index;

	/* Hands each page its part of the bounce. */
	bounce = kern_pmem_to_kernel(io->bounce.paddr);
	for (index = 0; index < io->page_count; index++)
		kern_memcpy(hal_pmem_to_kernel(io->pages[index]->pmem.paddr), bounce + (size_t)index * PAGE_SIZE, PAGE_SIZE);
}

/* Gives back the bounce and the disk admission a run holds. */
static void
io_release(
	struct vm_object_io *io)
{
	int error;

	/* Frees the bounce of a run that had one. */
	if (io->bounce.size != 0) {
		error = kern_pmem_free(&io->bounce);
		if (error != 0)
			HAL_FATAL("VM object I/O bounce retirement failed");
		kern_memset(&io->bounce, 0, sizeof(io->bounce));
	}

	/* Ends the cached-access admission. */
	if (io->token != NULL) {
		disk_cache_release(io->token);
		io->token = NULL;
	}
}

/*
 * Fills a run of object pages with a regular file's bytes, page by page
 * from base, the way the file system maps them (the inode's data_map):
 * a hole reads as zeroes, and a mapped run comes straight from the
 * mount's disk into the pages, in requests of up to one page-cache
 * request each.  length is the file's bytes from base (at most count
 * pages, the last page short at the end of file); the rest of the last
 * page is zeroed.
 *
 * The disk's copy is the newest one only where the buffer cache holds no
 * dirty line over it: such a run, and a run the file system says shares
 * a cache line with another owner's bytes, is read through fallback (the
 * file's own read, through the buffer cache) instead.  The caller holds
 * the inode's I/O lock, which keeps writers of the file out, and owns the
 * pages, BUSY or not yet published.  The pages hold the file's bytes only
 * when it returns 0.  EOPNOTSUPP, before any page is touched, means that
 * the file's data does not go through the page cache this way.
 */
int
vm_object_io_read_pages(
	struct inode *inode,
	off_t base,
	struct vm_object_page *const *pages,
	unsigned count,
	size_t length,
	const struct io_context *context,
	vm_object_read_fallback fallback,
	void *argument)
{
	struct inode_data_extent extent;
	struct vm_object_io *ios;
	struct disk *disk;
	uint64_t remaining;
	off_t position;
	unsigned limit;
	unsigned slots;
	unsigned index;
	unsigned run;
	int dirty;
	int error;

	/* Rejects a run that does not fit its pages or starts off a page. */
	if (inode == NULL || pages == NULL || count == 0 || length == 0 ||
	    base < 0 || (base & (off_t)(PAGE_SIZE - 1U)) != 0 ||
	    length > (size_t)count * PAGE_SIZE ||
	    length <= (size_t)(count - 1U) * PAGE_SIZE || fallback == NULL)
		return EINVAL;

	/* A file without the mapping, or a disk that takes no page, is read the old way. */
	if (inode->i_op == NULL || inode->i_op->data_map == NULL ||
	    inode->i_mount == NULL || inode->i_mount->m_disk == NULL)
		return EOPNOTSUPP;
	disk = inode->i_mount->m_disk;
	limit = vm_object_io_max_pages(disk);
	if (limit == 0)
		return EOPNOTSUPP;

	/* Maps the first run before taking anything, so that a refusal touches nothing. */
	error = inode->i_op->data_map(inode, base, length, INODE_DATA_READ, context, &extent);
	if (error != 0)
		return error;

	/*
	 * Holds the records of the requests the run keeps in flight at once,
	 * off the stack (each is a few kilobytes).
	 */
	slots = (count + limit - 1U) / limit;
	if (slots > VM_OBJECT_IO_READ_DEPTH)
		slots = VM_OBJECT_IO_READ_DEPTH;
	ios = kern_malloc(sizeof(*ios) * slots);
	if (ios == NULL)
		return ENOMEM;

	/* Fills the pages run by run, in file order. */
	index = 0;
	for (;;) {
		position = base + (off_t)index * PAGE_SIZE;
		remaining = (uint64_t)length - (uint64_t)index * PAGE_SIZE;

		/* Refuses a run that is not where it was asked for, or covers no page. */
		run = extent_pages(&extent, remaining);
		if (extent.offset != position || run == 0 || run > count - index) {
			error = EIO;
			break;
		}

		/*
		 * A hole is zeroes.  A mapped run that the disk holds the newest
		 * copy of, on whole blocks of the disk and on lines of its own, is
		 * read straight; any other is read through the file.
		 */
		if (extent.kind == INODE_DATA_HOLE) {
			zero_pages(pages + index, run);
		} else if (extent.kind != INODE_DATA_MAPPED) {
			error = EIO;
			break;
		} else {
			dirty = 1;
			if ((extent.flags & INODE_DATA_SHARED_LINE) == 0 &&
			    extent.sector <= UINT64_MAX / DISK_SECTOR_SIZE &&
			    extent.sector * DISK_SECTOR_SIZE % disk->d_block_size == 0)
				dirty = buf_range_dirty(disk, extent.sector * DISK_SECTOR_SIZE / disk->d_block_size,
				    (uint32_t)((uint64_t)run * PAGE_SIZE / disk->d_block_size));
			if (dirty < 0) {
				error = -dirty;
				break;
			}
			if (dirty)
				error = read_fallback(fallback, argument, position, pages + index, run, remaining);
			else
				error = read_direct(ios, slots, disk, extent.sector, pages + index, run, limit, context);
			if (error != 0)
				break;
		}

		/* Stops after the last page; maps the next run otherwise. */
		index += run;
		if (index == count)
			break;
		error = inode->i_op->data_map(inode, base + (off_t)index * PAGE_SIZE,
		    (uint64_t)length - (uint64_t)index * PAGE_SIZE, INODE_DATA_READ, context, &extent);
		if (error != 0) {
			/* The mapping cannot be withdrawn halfway through. */
			if (error == EOPNOTSUPP)
				error = EIO;
			break;
		}
	}

	kern_free(ios);

	/* Reports why the pages could not be filled. */
	if (error != 0)
		return error;

	/* Zeroes what follows the end of file in the last page. */
	if (length % PAGE_SIZE != 0)
		kern_memset((uint8_t *)hal_pmem_to_kernel(pages[count - 1U]->pmem.paddr) + length % PAGE_SIZE, 0,
		    PAGE_SIZE - length % PAGE_SIZE);

	/* Succeeded: every page holds the file's bytes. */
	return 0;
}

/*
 * Writes a run of object pages back over the file's blocks, page by page
 * from base, the way the file system maps them (the inode's data_map): a
 * mapped run goes straight from the pages to the mount's disk, in requests
 * of up to one page-cache request each.  length is the file's bytes from
 * base (at most count pages, the last page short at the end of file); what
 * follows the end of file in the last page is zeroed first, so that bytes
 * a mapping wrote past the end never reach the file.
 *
 * A hole the pages cover is allocated first (data_map with
 * INODE_DATA_ALLOCATE) and written the same way, unless the file system
 * leaves that to its own write (EOPNOTSUPP).
 *
 * Before a mapped run is written, the buffer cache's lines over it are
 * written back and dropped: the pages hold the newest bytes (every write
 * of the file updates its resident pages), and no stale line may outlive
 * the write.  A run the file system has to write itself (a hole it does
 * not allocate here, a run that shares a cache line with another owner's
 * bytes, a run marked to go through the file, or one whose line a journal
 * still pins) is written
 * through fallback (the file's own write).  A run the file system orders
 * before a commit (INODE_DATA_ORDERED) has the end of its write reported
 * (the inode's data_written) before anything else is done: once it reached
 * the disk, when it failed, or (EBUSY: not written here) just before it
 * goes to the file's write.
 * The caller holds the inode's I/O lock and owns the pages, which do not
 * change until it returns.
 * EOPNOTSUPP, before any byte is written, means that the file's data does
 * not go through the page cache this way.
 */
int
vm_object_io_write_pages(
	struct inode *inode,
	off_t base,
	struct vm_object_page *const *pages,
	unsigned count,
	size_t length,
	const struct io_context *context,
	vm_object_write_fallback fallback,
	void *argument)
{
	struct inode_data_extent extent;
	struct inode_data_extent allocated;
	struct write_queue queue;
	struct disk *disk;
	uint64_t remaining;
	uint64_t block;
	uint32_t blocks;
	off_t position;
	unsigned limit;
	unsigned index;
	unsigned run;
	int direct;
	int error;
	int drained;

	/* Rejects a run that does not fit its pages or starts off a page. */
	if (inode == NULL || pages == NULL || count == 0 || length == 0 ||
	    base < 0 || (base & (off_t)(PAGE_SIZE - 1U)) != 0 ||
	    length > (size_t)count * PAGE_SIZE ||
	    length <= (size_t)(count - 1U) * PAGE_SIZE || fallback == NULL)
		return EINVAL;

	/* A file without the mapping, or a disk that takes no page, is written the old way. */
	if (inode->i_op == NULL || inode->i_op->data_map == NULL ||
	    inode->i_mount == NULL || inode->i_mount->m_disk == NULL)
		return EOPNOTSUPP;
	disk = inode->i_mount->m_disk;
	limit = vm_object_io_max_pages(disk);
	if (limit == 0)
		return EOPNOTSUPP;

	/* Maps the first run before writing anything, so that a refusal changes nothing. */
	error = inode->i_op->data_map(inode, base, length, INODE_DATA_WRITEBACK, context, &extent);
	if (error != 0)
		return error;

	/* Keeps what a mapping wrote past the end of file out of the file. */
	if (length % PAGE_SIZE != 0)
		kern_memset((uint8_t *)hal_pmem_to_kernel(pages[count - 1U]->pmem.paddr) + length % PAGE_SIZE, 0,
		    PAGE_SIZE - length % PAGE_SIZE);

	/* Holds the records of the requests kept in flight, off the stack (each is a few kilobytes). */
	error = write_queue_open(&queue, inode, count, limit);
	if (error != 0) {
		write_ordered_end(inode, &extent, error);
		return error;
	}

	/* Writes the pages run by run, in file order. */
	index = 0;
	for (;;) {
		position = base + (off_t)index * PAGE_SIZE;
		remaining = (uint64_t)length - (uint64_t)index * PAGE_SIZE;

		/* Refuses a run that is not where it was asked for, or covers no page. */
		run = extent_pages(&extent, remaining);
		if (extent.offset != position || run == 0 || run > count - index) {
			error = EIO;
			write_ordered_end(inode, &extent, error);
			break;
		}

		/*
		 * A hole under the pages (a shared mapping's stores into a sparse
		 * file) gets its blocks from the file system and is written like
		 * a mapped run; a file system that leaves the allocation to the
		 * file's own write keeps the hole for that write.
		 */
		if (extent.kind == INODE_DATA_HOLE) {
			/*
			 * The allocation may commit the journal, whose seal waits
			 * for the ordered runs in flight: they finish first.
			 */
			error = write_queue_drain(&queue);
			if (error != 0)
				break;
			error = inode->i_op->data_map(inode, position, extent.length, INODE_DATA_ALLOCATE, context,
			    &allocated);
			if (error == 0) {
				run = extent_pages(&allocated, remaining);
				if (allocated.offset != position || run == 0 || run > count - index ||
				    allocated.kind == INODE_DATA_HOLE) {
					error = EIO;
					break;
				}
				extent = allocated;
			} else if (error == EOPNOTSUPP) {
				error = 0;
			} else {
				/* The blocks could not be allocated. */
				break;
			}
		}

		/*
		 * A mapped run on whole blocks of the disk and on lines of its
		 * own, which the file system leaves to the cache, goes straight
		 * to the disk once the buffer cache's lines over it are gone.
		 */
		direct = 0;
		block = 0;
		blocks = 0;
		if ((extent.kind == INODE_DATA_MAPPED || extent.kind == INODE_DATA_NEW) &&
		    (extent.flags & (INODE_DATA_SHARED_LINE | INODE_DATA_THROUGH_FILE)) == 0 &&
		    extent.sector <= UINT64_MAX / DISK_SECTOR_SIZE &&
		    extent.sector * DISK_SECTOR_SIZE % disk->d_block_size == 0) {
			block = extent.sector * DISK_SECTOR_SIZE / disk->d_block_size;
			blocks = (uint32_t)((uint64_t)run * PAGE_SIZE / disk->d_block_size);
			error = buf_direct_prepare(disk, block, blocks, 1);
			if (error == 0) {
				direct = 1;
			} else if (error != EBUSY) {
				/* The cache could not write its lines over the run back. */
				write_ordered_end(inode, &extent, error);
				break;
			}
		}

		/*
		 * Starts the run's requests, its end reported when they finish,
		 * or reports that it goes on to the file's write and writes it
		 * there, once the requests in flight have finished (the file's
		 * write may commit the journal too).
		 */
		if (direct) {
			error = write_queue_run(&queue, disk, &extent, pages + index, run, limit, context);
		} else {
			error = write_queue_drain(&queue);
			if (error != 0) {
				write_ordered_end(inode, &extent, EBUSY);
				break;
			}
			write_ordered_end(inode, &extent, EBUSY);
			error = write_fallback(fallback, argument, position, pages + index, run, remaining);
		}
		if (error != 0)
			break;

		/* Stops after the last page; maps the next run otherwise. */
		index += run;
		if (index == count)
			break;
		error = inode->i_op->data_map(inode, base + (off_t)index * PAGE_SIZE,
		    (uint64_t)length - (uint64_t)index * PAGE_SIZE, INODE_DATA_WRITEBACK, context, &extent);
		if (error != 0) {
			/* The mapping cannot be withdrawn halfway through. */
			if (error == EOPNOTSUPP)
				error = EIO;
			break;
		}
	}

	/* Waits for the requests still in flight, keeping the first failure. */
	drained = write_queue_drain(&queue);
	if (error == 0)
		error = drained;
	write_queue_close(&queue);

	/* Reports why the pages could not be written. */
	if (error != 0)
		return error;

	/* Succeeded: the file holds every page's bytes. */
	return 0;
}

/*
 * Opens the queue of one write-back: as many request records as the pages
 * need, up to VM_OBJECT_IO_WRITE_DEPTH, or one when there is no memory for
 * more.
 */
static int
write_queue_open(
	struct write_queue *queue,
	struct inode *inode,
	unsigned count,
	unsigned limit)
{
	unsigned slots;

	/* Starts empty. */
	kern_memset(queue, 0, sizeof(*queue));
	queue->inode = inode;

	/* Takes the records, falling back to one. */
	slots = (count + limit - 1U) / limit;
	if (slots > VM_OBJECT_IO_WRITE_DEPTH)
		slots = VM_OBJECT_IO_WRITE_DEPTH;
	queue->ios = kern_malloc(sizeof(*queue->ios) * slots);
	if (queue->ios == NULL && slots > 1U) {
		slots = 1;
		queue->ios = kern_malloc(sizeof(*queue->ios));
	}
	if (queue->ios == NULL)
		return ENOMEM;
	queue->slots = slots;

	/* Succeeded. */
	return 0;
}

/* Gives the records back; nothing may be in flight. */
static void
write_queue_close(
	struct write_queue *queue)
{
	/* Traps on a request or a run left behind. */
	if (queue->busy != 0 || queue->run_count != 0)
		HAL_FATAL("page-cache write queue closed with requests in flight");

	kern_free(queue->ios);
	queue->ios = NULL;
}

/*
 * Starts the requests of one mapped run (at most limit pages each),
 * finishing the oldest request whenever every record is in flight.  The
 * run's end is reported once all of them have finished.  A request that
 * cannot start fails the run; the requests already started still finish.
 */
static int
write_queue_run(
	struct write_queue *queue,
	struct disk *disk,
	const struct inode_data_extent *extent,
	struct vm_object_page *const *pages,
	unsigned count,
	unsigned limit,
	const struct io_context *context)
{
	unsigned entry;
	unsigned slot;
	unsigned done;
	unsigned chunk;
	int error;

	/* Makes room for the run among the runs not yet reported. */
	while (queue->run_count == VM_OBJECT_IO_WRITE_DEPTH + 1U)
		write_queue_finish_one(queue);

	/* Records the run; it stays open while its requests are being started. */
	entry = (queue->run_head + queue->run_count) % (VM_OBJECT_IO_WRITE_DEPTH + 1U);
	queue->runs[entry] = *extent;
	queue->run_left[entry] = 0;
	queue->run_error[entry] = 0;
	queue->run_open[entry] = 1;
	queue->run_count++;

	/* Starts the run a request at a time, waiting for the oldest when every record is busy. */
	error = 0;
	done = 0;
	while (done < count) {
		if (queue->busy == queue->slots)
			write_queue_finish_one(queue);
		chunk = count - done;
		if (chunk > limit)
			chunk = limit;
		slot = (queue->head + queue->busy) % queue->slots;
		error = vm_object_io_start(&queue->ios[slot], disk, BIO_WRITE,
		    extent->sector + (uint64_t)done * (PAGE_SIZE / DISK_SECTOR_SIZE), pages + done, chunk, context);
		if (error != 0)
			break;
		queue->slot_run[slot] = entry;
		queue->run_left[entry]++;
		queue->busy++;
		done += chunk;
	}

	/* Closes the run, failed when a request could not start, and reports what has ended. */
	if (error != 0) {
		queue->run_error[entry] = error;
		if (queue->error == 0)
			queue->error = error;
	}
	queue->run_open[entry] = 0;
	write_queue_report(queue);

	/* Reports a request that could not start, or one that failed meanwhile. */
	if (error != 0)
		return error;
	if (queue->error != 0)
		return queue->error;

	/* Succeeded: the run's requests are in flight or done. */
	return 0;
}

/* Waits for the oldest request in flight and reports the runs that have ended. */
static void
write_queue_finish_one(
	struct write_queue *queue)
{
	unsigned entry;
	int error;

	/* Nothing in flight, nothing to wait for. */
	if (queue->busy == 0)
		return;

	/* Waits for it and charges its result to its run. */
	error = vm_object_io_finish(&queue->ios[queue->head]);
	entry = queue->slot_run[queue->head];
	queue->head = (queue->head + 1U) % queue->slots;
	queue->busy--;
	if (queue->run_left[entry] == 0)
		HAL_FATAL("page-cache write queue lost a request");
	queue->run_left[entry]--;
	if (error != 0) {
		if (queue->run_error[entry] == 0)
			queue->run_error[entry] = error;
		if (queue->error == 0)
			queue->error = error;
	}

	/* Reports the runs that have ended. */
	write_queue_report(queue);
}

/* Waits for every request in flight; reports the first failure of the queue. */
static int
write_queue_drain(
	struct write_queue *queue)
{
	/* Finishes the requests oldest first. */
	while (queue->busy != 0)
		write_queue_finish_one(queue);

	/* Reports the runs left, all of them ended now. */
	write_queue_report(queue);
	if (queue->run_count != 0)
		HAL_FATAL("page-cache write queue kept an open run");

	/* Reports the first failure. */
	if (queue->error != 0)
		return queue->error;

	/* Succeeded. */
	return 0;
}

/* Reports, in file order, the end of every run whose requests have all finished. */
static void
write_queue_report(
	struct write_queue *queue)
{
	unsigned entry;

	/* Ends the oldest run while nothing of it is in flight or still to start. */
	while (queue->run_count != 0) {
		entry = queue->run_head;
		if (queue->run_left[entry] != 0 || queue->run_open[entry])
			break;
		write_ordered_end(queue->inode, &queue->runs[entry], queue->run_error[entry]);
		queue->run_head = (queue->run_head + 1U) % (VM_OBJECT_IO_WRITE_DEPTH + 1U);
		queue->run_count--;
	}
}

/*
 * Reports the end of the write of a run the file system orders before a
 * commit (INODE_DATA_ORDERED) through the inode's data_written; any other
 * run has nothing to report.
 */
static void
write_ordered_end(
	struct inode *inode,
	const struct inode_data_extent *extent,
	int error)
{
	/* Only an ordered run's write is waited for. */
	if ((extent->flags & INODE_DATA_ORDERED) == 0)
		return;
	if (inode->i_op->data_written == NULL)
		return;

	/* Reports the write's end. */
	inode->i_op->data_written(inode, extent, error);
}

/*
 * The pages an extent fills: the whole pages it covers, and the page it
 * ends in when it ends at the end of file (the remaining bytes); 0 for a
 * run that ends inside a page before the end of file, which no file system
 * whose blocks are whole pages reports.
 */
static unsigned
extent_pages(
	const struct inode_data_extent *extent,
	uint64_t remaining)
{
	uint64_t length;

	/* A run past what was asked for, or an empty one, fills nothing. */
	length = extent->length;
	if (length == 0 || length > remaining)
		return 0;

	/* A run that ends at the end of file takes its last, short page. */
	if (length == remaining)
		return (unsigned)((length + PAGE_SIZE - 1U) / PAGE_SIZE);

	/* Any other run ends on a page. */
	if (length % PAGE_SIZE != 0)
		return 0;
	return (unsigned)(length / PAGE_SIZE);
}

/*
 * Reads pages straight from consecutive sectors: up to slots requests of
 * at most limit pages each are started before the first is waited for,
 * then all of them are finished, until the run is read.
 */
static int
read_direct(
	struct vm_object_io *ios,
	unsigned slots,
	struct disk *disk,
	uint64_t sector,
	struct vm_object_page *const *pages,
	unsigned count,
	unsigned limit,
	const struct io_context *context)
{
	unsigned done;
	unsigned chunk;
	unsigned started;
	unsigned slot;
	int finished;
	int error;

	/* Reads the run a batch of requests at a time. */
	error = 0;
	done = 0;
	while (done < count && error == 0) {
		/* Starts as many requests as there are slots and pages left. */
		for (started = 0; started < slots && done < count; started++) {
			chunk = count - done;
			if (chunk > limit)
				chunk = limit;
			error = vm_object_io_start(&ios[started], disk, BIO_READ,
			    sector + (uint64_t)done * (PAGE_SIZE / DISK_SECTOR_SIZE), pages + done, chunk, context);
			if (error != 0)
				break;
			done += chunk;
		}

		/* Waits for every request started, keeping the first failure. */
		for (slot = 0; slot < started; slot++) {
			finished = vm_object_io_finish(&ios[slot]);
			if (finished != 0 && error == 0)
				error = finished;
		}
	}

	/* Reports why the run could not be read. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reads pages through the file's own read, a page at a time. */
static int
read_fallback(
	vm_object_read_fallback fallback,
	void *argument,
	off_t offset,
	struct vm_object_page *const *pages,
	unsigned count,
	uint64_t remaining)
{
	unsigned index;
	size_t wanted;
	ssize_t got;
	void *data;

	/* Reads each page's bytes, the last one up to the end of file. */
	for (index = 0; index < count; index++) {
		wanted = PAGE_SIZE;
		if ((uint64_t)index * PAGE_SIZE + wanted > remaining)
			wanted = (size_t)(remaining - (uint64_t)index * PAGE_SIZE);
		data = hal_pmem_to_kernel(pages[index]->pmem.paddr);
		got = fallback(argument, offset + (off_t)index * PAGE_SIZE, data, wanted);
		if (got < 0)
			return (int)-got;
		if ((size_t)got != wanted)
			return EIO;
	}

	/* Succeeded. */
	return 0;
}

/* Writes pages through the file's own write, a page at a time. */
static int
write_fallback(
	vm_object_write_fallback fallback,
	void *argument,
	off_t offset,
	struct vm_object_page *const *pages,
	unsigned count,
	uint64_t remaining)
{
	unsigned index;
	size_t wanted;
	ssize_t put;
	const void *data;

	/* Writes each page's bytes, the last one up to the end of file. */
	for (index = 0; index < count; index++) {
		wanted = PAGE_SIZE;
		if ((uint64_t)index * PAGE_SIZE + wanted > remaining)
			wanted = (size_t)(remaining - (uint64_t)index * PAGE_SIZE);
		data = hal_pmem_to_kernel(pages[index]->pmem.paddr);
		put = fallback(argument, offset + (off_t)index * PAGE_SIZE, data, wanted);
		if (put < 0)
			return (int)-put;
		if ((size_t)put != wanted)
			return EIO;
	}

	/* Succeeded. */
	return 0;
}

/* Zeroes whole pages. */
static void
zero_pages(
	struct vm_object_page *const *pages,
	unsigned count)
{
	unsigned index;

	/* Clears each page. */
	for (index = 0; index < count; index++)
		kern_memset(hal_pmem_to_kernel(pages[index]->pmem.paddr), 0, PAGE_SIZE);
}
