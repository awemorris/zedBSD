/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_BIO_ASYNC_H
#define ZEDBSD_KERN_BIO_ASYNC_H

#include <kern/disk.h>

struct bio_async_request;
typedef void (*bio_async_callback)(struct bio_async_request *, void *);

/* Setup may allocate/start a worker; admission never waits for device I/O. */
int bio_async_enable(struct disk *disk);
int bio_async_disable(struct disk *disk);

/*
 * Returns one caller reference and a private payload snapshot (writes only).
 * A non-NULL claim is explicit authorization, independent of context provenance.
 * Context inode/claim owners are retained until final release. Four slots per
 * endpoint bound retained payloads. Requests must also fit the device transfer
 * limit; E2BIG leaves splitting to the caller. Failure transfers no ownership.
 */
int bio_async_prepare(struct disk *disk, enum bio_op op, uint64_t block,
    uint32_t count, const void *write_data, const struct backing_claim *claim,
    const struct io_context *context, bio_async_callback callback, void *argument,
    struct bio_async_request **result);

/*
 * Success transfers an additional reference to the queue; callback may run
 * before submit returns. Rejection leaves a prepared caller-owned request and
 * never calls back. The callback may release the caller reference. Callback
 * argument lifetime is the caller's responsibility through callback return.
 * Callbacks run in the driver completion context and must not block. Final
 * ordinary handle release and endpoint operations require process context.
 */
int bio_async_submit(struct bio_async_request *request);
int bio_async_cancel(struct bio_async_request *request);
int bio_async_wait(struct bio_async_request *request);
/* Data is read-only and valid only while the caller retains its reference. */
int bio_async_result(struct bio_async_request *request, const void **data,
    size_t *transferred);
void bio_async_ref(struct bio_async_request *request);
void bio_async_release(struct bio_async_request *request);

#endif
