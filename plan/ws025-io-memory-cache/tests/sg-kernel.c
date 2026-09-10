/* Link-only native probe: actual DMA owners backed by discontiguous RAM. */
#include <hal/hal.h>
#include <drivers/dma.h>
#include <drivers/usb.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) HAL_FATAL("SG probe: " #x); } while (0)
static unsigned allocating[HAL_CPU_MAX];
static unsigned high_index, low_index;
int __real_drv_dma_vector_create(struct drv_dma_device *, size_t, struct drv_dma_vector **);
int __real_hal_pmem_alloc_range(hal_physaddr_t, size_t, size_t, uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, struct hal_pmem *);
int __real_drv_usb_hcd_register(struct drv_usb_hcd *, struct drv_usb_bus **);

/* Reproduce the extra core/HCD copy with identical backing and TRB shapes. */
int
__wrap_drv_usb_hcd_register(struct drv_usb_hcd *hcd, struct drv_usb_bus **bus)
{
#ifdef WS025_SG_HIGH_QEMU
	static struct drv_dma_device *high_device;
	struct drv_dma_constraints constraints;

	/* QEMU AC64 fixture only; keep the owner alive as long as the HCD. */
	if (strcmp(hcd->name, "xHCI") == 0) {
		REQUIRE(high_device == NULL);
		memset(&constraints, 0, sizeof(constraints));
		constraints.address_bits = 64;
		constraints.max_segment_size = drv_dma_device_max_segment_size(hcd->dma);
		constraints.coherent = drv_dma_device_is_coherent(hcd->dma);
		REQUIRE(drv_dma_device_create(&constraints, &high_device) == 0);
		hcd->dma = high_device;
		hal_puts("SG QEMU TEST DMA OWNER bits=64 lifetime=HCD\n");
	}
#endif
#ifdef WS025_SG_COPY_BASELINE
	static struct drv_usb_hcd_ops saved[8];
	static unsigned count;
	if (hcd->capabilities & DRV_USB_HCD_CAP_SHARED_STAGING) {
		REQUIRE(count < 8);
		saved[count] = *hcd->ops;
		saved[count].urb_reserve_buffer = NULL;
		hcd->ops = &saved[count++];
		hcd->capabilities &= ~DRV_USB_HCD_CAP_SHARED_STAGING;
	}
#endif
	return __real_drv_usb_hcd_register(hcd, bus);
}

int
__wrap_hal_pmem_alloc_range(hal_physaddr_t request_paddr, size_t request_size, size_t request_alignment, uint32_t request_type, uint32_t request_attr,
    uint64_t minimum, uint64_t maximum, uint64_t boundary, struct hal_pmem *memory)
{
	uint64_t candidate;
	unsigned index;
	unsigned attempt;
	int error;

	if (!allocating[hal_cpu_current()] || request_size != 4096)
		return __real_hal_pmem_alloc_range(request_paddr, request_size,
		    request_alignment, request_type, request_attr, minimum, maximum,
		    boundary, memory);

	/* Exact test PFNs may already belong to ordinary coherent buffers. */
	for (attempt = 0; attempt < 1024; attempt++) {
		if (maximum > UINT32_MAX) {
			index = __atomic_fetch_add(&high_index, 1U, __ATOMIC_RELAXED);
			candidate = UINT64_C(0x110000000) + (uint64_t)index * 65536U;
		} else {
			index = __atomic_fetch_add(&low_index, 1U, __ATOMIC_RELAXED);
			candidate = UINT64_C(0x20000000) + (uint64_t)index * 65536U;
		}
		if (maximum < 4095U || candidate < minimum || candidate > maximum - 4095U)
			return HAL_ERR_NOMEM;
		error = __real_hal_pmem_alloc_range(request_paddr, request_size,
		    request_alignment, request_type, request_attr, candidate,
		    candidate + 4095U, boundary, memory);
		if (error != HAL_ERR_NOMEM)
			return error;
		hal_printf("SG TEST PFN UNAVAILABLE address=%llx attempt=%u\n",
		    (unsigned long long)candidate, attempt);
	}
	return HAL_ERR_NOMEM;
}

int
__wrap_drv_dma_vector_create(struct drv_dma_device *device, size_t size,
    struct drv_dma_vector **result)
{
	struct drv_dma_segment segment;
	uint64_t previous, first;
	unsigned cpu, index, count;
	int error;

	cpu = hal_cpu_current();
	REQUIRE(!allocating[cpu]);
	allocating[cpu] = 1;
	error = __real_drv_dma_vector_create(device, size, result);
	REQUIRE(hal_cpu_current() == cpu);
	allocating[cpu] = 0;
	if (error != 0 || size != 65536)
		return error;
	count = drv_dma_vector_count(*result);
	hal_printf("SG VECTOR OBSERVED count=%u bits=%u\n", count, drv_dma_device_address_bits(device));
	REQUIRE(count == 16);
	previous = first = 0;
	for (index = 0; index < count; index++) {
		REQUIRE(drv_dma_vector_segment(*result, index, &segment) == 0);
		REQUIRE(segment.length == 4096 && segment.address != previous + 4096);
		if (drv_dma_device_address_bits(device) > 32)
			REQUIRE(segment.address > UINT32_MAX);
		if (index == 0)
			first = segment.address;
		previous = segment.address;
	}
	hal_printf("SG DMA PASS size=%u segments=%u bits=%u first=%llx last=%llx\n",
	    (unsigned)size, count, drv_dma_device_address_bits(device),
	    (unsigned long long)first, (unsigned long long)previous);
	return 0;
}
