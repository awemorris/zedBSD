/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

/* The same immutable plan drives publication and short-event accounting. */
static int
xhci_sg_plan(
	struct xhci_request *request,
	size_t length)
{
	struct drv_dma_segment segment;
	size_t remaining, chunk;
	unsigned index, count;

	request->normal_count = 0;
	count = drv_dma_vector_count(request->vector);
	if (count == 0 || count > DRV_DMA_VECTOR_MAX_SEGMENTS ||
	    length > DRV_DMA_VECTOR_MAX_SIZE)
		return EINVAL;
	if (length == 0) {
		request->normal_count = 1;
		request->normal[0].address = 0;
		request->normal[0].length = 0;
		return 0;
	}
	remaining = length;
	for (index = 0; index < count && remaining != 0; index++) {
		if (drv_dma_vector_segment(request->vector, index, &segment) != 0 ||
		    segment.length == 0 || segment.length - 1U > UINT64_MAX - segment.address)
			return EINVAL;
		if (segment.length > remaining)
			segment.length = remaining;
		while (segment.length != 0) {
			chunk = 0x10000U - (size_t)(segment.address & 0xffffU);
			if (chunk > segment.length)
				chunk = segment.length;
			if (request->normal_count == DRV_DMA_VECTOR_MAX_SEGMENTS)
				return E2BIG;
			request->normal[request->normal_count].address = segment.address;
			request->normal[request->normal_count++].length = chunk;
			segment.address += chunk;
			segment.length -= chunk;
			remaining -= chunk;
		}
	}
	return remaining == 0 ? 0 : EINVAL;
}

static int
xhci_sg_short(
	const struct xhci_request *request,
	unsigned offset,
	size_t residual,
	size_t *actual)
{
	size_t prefix;
	unsigned index;

	if (actual == NULL || offset >= request->normal_count ||
	    request->normal[offset].length == 0 ||
	    residual > request->normal[offset].length)
		return 0;
	prefix = 0;
	for (index = 0; index < offset; index++)
		prefix += request->normal[index].length;
	*actual = prefix + request->normal[offset].length - residual;
	return 1;
}

static void
xhci_sg_enqueue(
	struct xhci_ring *ring,
	const struct xhci_request *request,
	int input,
	size_t packet_size,
	int zero_packet)
{
	size_t cumulative;
	uint32_t control;
	unsigned index, td_size;
	int final;

	cumulative = 0;
	for (index = 0; index < request->normal_count; index++) {
		cumulative += request->normal[index].length;
		final = index + 1U == request->normal_count && !zero_packet;
		control = XHCI_TRB_TYPE(1) | (final ? XHCI_TRB_IOC : XHCI_TRB_CHAIN);
		if (input)
			control |= DRV_XHCI_TRB_ISP;
		td_size = drv_xhci_normal_td_size(request->length, cumulative, packet_size, final);
		if (zero_packet && td_size < 31U)
			td_size++;
		(void)ring_push(ring, request->normal[index].address,
		    (uint32_t)request->normal[index].length | (td_size << 17), control);
		io_stats_record(IO_XHCI_DATA_TRB, request->normal[index].length);
	}
	if (zero_packet) {
		(void)ring_push(ring, 0, 0, XHCI_TRB_TYPE(1) | XHCI_TRB_IOC);
		io_stats_record(IO_XHCI_DATA_TRB, 0);
	}
}
