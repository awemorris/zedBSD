/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel as the Cadence GEM driver, the Pi 5's Ethernet glue and the
 * PHY drivers see it on the host.
 *
 * The build copies each driver with its include lines removed and compiles
 * the copy with this header forced in front of it, so every copy sees the
 * same few kernel types and functions.  cadence-gem-host-test.c defines
 * the functions: fake registers, a fake device tree, fake DMA memory, and
 * a trace of everything the drivers do that the hardware or the rest of
 * the kernel could observe.
 */

#ifndef TESTS_CADENCE_GEM_HOST_TEST_H
#define TESTS_CADENCE_GEM_HOST_TEST_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/ethernet/bcm54213pe.h>
#include <drivers/ethernet/cadence-gem.h>
#include <drivers/ethernet/mii-phy.h>
#include <drivers/platform/rpi5/rp1-ethernet.h>

/*
 * A number of milliseconds as scheduler ticks, rounded up.  The fake clock
 * counts one tick a millisecond.
 */
static inline uint64_t
kern_ms_to_ticks(
	uint64_t milliseconds)
{
	/* Reports the ticks of the fake clock. */
	return milliseconds;
}

/* The lock rank the drivers take their device lock at. */
enum lock_rank {
	LOCK_RANK_DEVICE = 125
};

/* The thread priority the periodic service runs at. */
#define SCHED_PRIORITY_DEFAULT		8

/* The interface's name length and hardware address length. */
#define NET_DEVICE_NAME_MAX		16U
#define NET_DEVICE_HWADDR_MAX		8U

/* The interface flags the driver sets. */
#define NET_DEVICE_BROADCAST		0x00000004U
#define NET_DEVICE_MULTICAST		0x00000008U

/* The headroom the driver asks of a received packet. */
#define PACKET_BUF_DEFAULT_HEADROOM	64U

/* The node offset that names no node. */
#define DRV_FDT_NO_NODE			0U

/* A spin lock; the test runs on one thread and only records its use. */
struct spinlock {
	int held;
};

/* A device tree; the fake answers by node number and property name. */
struct drv_fdt {
	int unused;
};

/* A kernel thread the driver creates; the test never runs it. */
struct thread {
	void (*entry)(void *argument);
	void *argument;
};

/*
 * A DMA device: the address width it reaches and where its next fake
 * allocation is placed.
 */
struct drv_dma_device {
	unsigned address_bits;
	int coherent;
	uint64_t next_address;
};

/* A coherent DMA allocation, as the kernel returns it. */
struct drv_dma_buffer {
	void *address;
	uint64_t device_address;
	size_t size;
	uintptr_t private_data[2];
};

/* What a driver says about the DMA its device does. */
struct drv_dma_constraints {
	unsigned address_bits;
	size_t max_segment_size;
	uint64_t segment_boundary;
	int coherent;
};

/* One network packet: its storage and the frame within it. */
struct packet_buf {
	uint8_t *storage;
	uint8_t *data;
	size_t capacity;
	size_t length;
};

struct net_device;

/* The callbacks a network driver publishes. */
struct net_device_ops {
	int (*open)(struct net_device *device);
	void (*close)(struct net_device *device);
	int (*transmit)(struct net_device *device, struct packet_buf *packet);
	unsigned (*poll_receive)(struct net_device *device, unsigned budget);
	int (*ioctl)(struct net_device *device, unsigned long request, void *argument);
	void (*release)(void *driver_data);
	int (*set_mtu)(struct net_device *device, unsigned mtu);
};

/* The fields of a network interface that the driver fills in and counts. */
struct net_device {
	char name[NET_DEVICE_NAME_MAX];
	unsigned flags;
	unsigned mtu;
	uint32_t link_mbps;
	uint8_t hwaddr[NET_DEVICE_HWADDR_MAX];
	uint8_t hwaddr_len;
	const struct net_device_ops *ops;
	void *driver_data;
	uint64_t rx_errors;
	uint64_t rx_dropped;
	uint64_t tx_errors;
};

/* The RP1 interrupt handler's type. */
typedef void (*drv_rp1_irq_handler_t)(void *argument);

void spin_init(struct spinlock *lock, enum lock_rank rank, const char *name);
unsigned long spin_lock_irqsave(struct spinlock *lock);
void spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled);

uint32_t kern_mmio_read32(const volatile void *address);
void kern_mmio_write32(volatile void *address, uint32_t value);
void kern_io_barrier(void);
void kern_io_read_barrier(void);
void kern_io_write_barrier(void);

void kern_logf(const char *format, ...) __attribute__((format(printf, 1, 2)));
void *kern_memcpy(void *destination, const void *source, size_t length);
void *kern_memset(void *destination, int byte, size_t length);
int kern_strcmp(const char *left, const char *right);
size_t kern_strlen(const char *string);

uint64_t clock_ticks(void);
bool kern_rtc_read_counter(uint64_t *counter, uint64_t *frequency_hz);
void kern_usleep_range(unsigned min_us, unsigned max_us);
uint64_t sched_ticks(void);
void sched_sleep(uint64_t timeout_tick);
int kthread_create(void (*entry)(void *), void *arg, int priority, struct thread **result);
void thread_start(struct thread *thread);

int drv_dma_device_create(const struct drv_dma_constraints *constraints, struct drv_dma_device **result);
int drv_dma_device_create_window(const struct drv_dma_constraints *constraints, uint64_t bus_offset, uint64_t physical_limit, struct drv_dma_device **result);
int drv_dma_alloc_coherent(struct drv_dma_device *device, size_t size, size_t alignment, struct drv_dma_buffer *buffer);

struct packet_buf *packet_buf_alloc(size_t headroom);
void packet_buf_free(struct packet_buf *packet);
void *packet_buf_append(struct packet_buf *packet, size_t length);

struct net_device *net_device_alloc(void);
int net_device_create(struct net_device *device);
void net_device_ref(struct net_device *device);
int net_device_set_carrier(struct net_device *device, int carrier);
void net_device_receive(struct net_device *device, struct packet_buf *packet);
void net_device_schedule_poll(struct net_device *device);

int drv_fdt_find_phandle(const struct drv_fdt *fdt, uint32_t phandle, uint32_t *node);
int drv_fdt_property(const struct drv_fdt *fdt, uint32_t node, const char *name, const uint8_t **value, uint32_t *length);
bool drv_fdt_node_enabled(const struct drv_fdt *fdt, uint32_t node);
uint32_t drv_fdt_node_cells(const struct drv_fdt *fdt, uint32_t node, const char *name, uint32_t fallback);
uint64_t drv_fdt_cells_value(const uint8_t *value, uint32_t cell_index, uint32_t cell_count);

const struct drv_fdt *drv_rp1_fdt(void);
int drv_rp1_find_child(const char *compatible, uint32_t after, uint32_t *node);
int drv_rp1_map(uint32_t node, unsigned index, volatile uint8_t **mapped, uint64_t *size);
int drv_rp1_dma_window(uint64_t *bus_offset, uint64_t *physical_limit, int *coherent);
int drv_rp1_gpio_init(void);
int drv_rp1_gpio_set_output(unsigned pin, int level);
int drv_rp1_irq_init(void);
int drv_rp1_irq_of(uint32_t node, unsigned index, unsigned *vector, unsigned *trigger);
int drv_rp1_irq_establish(unsigned vector, unsigned trigger, drv_rp1_irq_handler_t handler, void *argument);

#endif
