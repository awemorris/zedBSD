/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * NVMe register, queue, and Identify arithmetic
 */

#ifndef KERN_DRIVERS_PCI_NVME_PROTOCOL_H
#define KERN_DRIVERS_PCI_NVME_PROTOCOL_H

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#define DRV_NVME_PCI_CLASS		0x010802U

#define DRV_NVME_REG_CAP		0x0000U
#define DRV_NVME_REG_VS			0x0008U
#define DRV_NVME_REG_INTMS		0x000cU
#define DRV_NVME_REG_INTMC		0x0010U
#define DRV_NVME_REG_CC			0x0014U
#define DRV_NVME_REG_CSTS		0x001cU
#define DRV_NVME_REG_NSSR		0x0020U
#define DRV_NVME_REG_AQA		0x0024U
#define DRV_NVME_REG_ASQ		0x0028U
#define DRV_NVME_REG_ACQ		0x0030U
#define DRV_NVME_REG_DOORBELL_BASE	0x1000U

#define DRV_NVME_CC_ENABLE		0x00000001U
#define DRV_NVME_CC_SHN_MASK		0x0000c000U
#define DRV_NVME_CSTS_READY		0x00000001U
#define DRV_NVME_CSTS_FATAL		0x00000002U

#define DRV_NVME_ADMIN_DELETE_IO_SQ	0x00U
#define DRV_NVME_ADMIN_CREATE_IO_SQ	0x01U
#define DRV_NVME_ADMIN_DELETE_IO_CQ	0x04U
#define DRV_NVME_ADMIN_CREATE_IO_CQ	0x05U
#define DRV_NVME_ADMIN_IDENTIFY		0x06U
#define DRV_NVME_ADMIN_SET_FEATURES	0x09U

#define DRV_NVME_FEATURE_NUMBER_OF_QUEUES	0x07U

/*
 * The Host Memory Buffer feature and its descriptor list entry.
 * Values referenced: NVM Express Base Specification 1.4c, Set Features
 * Feature Identifier 0Dh (Host Memory Buffer: CDW11 EHM bit 0 and MR bit 1,
 * CDW12 HSIZE, CDW13 HMDLLA, CDW14 HMDLUA, CDW15 HMDLEC) and the Host
 * Memory Buffer Descriptor Entry (BADD bytes 7:0, BSIZE bytes 11:8, both in
 * units of the memory page size CC.MPS).
 */
#define DRV_NVME_FEATURE_HOST_MEMORY_BUFFER	0x0DU
#define DRV_NVME_HMB_ENABLE			(1U << 0)
#define DRV_NVME_HMB_MEMORY_RETURN		(1U << 1)
#define DRV_NVME_HMB_DESCRIPTOR_SIZE		16U

#define DRV_NVME_NVM_FLUSH		0x00U
#define DRV_NVME_NVM_WRITE		0x01U
#define DRV_NVME_NVM_READ		0x02U

#define DRV_NVME_STATUS_GENERIC		0x00U
#define DRV_NVME_STATUS_COMMAND_SPECIFIC	0x01U
#define DRV_NVME_STATUS_MEDIA		0x02U
#define DRV_NVME_STATUS_PATH		0x03U

#define DRV_NVME_IDENTIFY_NAMESPACE	0x00U
#define DRV_NVME_IDENTIFY_CONTROLLER	0x01U
#define DRV_NVME_IDENTIFY_ACTIVE_LIST	0x02U

#define DRV_NVME_ADMIN_SQE_SIZE		64U
#define DRV_NVME_CQE_SIZE		16U
#define DRV_NVME_IDENTIFY_SIZE		4096U

struct drv_nvme_command {
	uint32_t cdw0;
	uint32_t namespace_id;
	uint32_t reserved2[2];
	uint64_t metadata;
	uint64_t prp1;
	uint64_t prp2;
	uint32_t cdw10;
	uint32_t cdw11;
	uint32_t cdw12;
	uint32_t cdw13;
	uint32_t cdw14;
	uint32_t cdw15;
} __attribute__((packed));

struct drv_nvme_completion {
	uint32_t result;
	uint32_t reserved;
	uint16_t submission_head;
	uint16_t submission_id;
	uint16_t command_id;
	uint16_t status;
} __attribute__((packed));

typedef char drv_nvme_command_size_must_be_64[
	(sizeof(struct drv_nvme_command) == DRV_NVME_ADMIN_SQE_SIZE) ? 1 : -1];
typedef char drv_nvme_completion_size_must_be_16[
	(sizeof(struct drv_nvme_completion) == DRV_NVME_CQE_SIZE) ? 1 : -1];

struct drv_nvme_capability_snapshot {
	size_t mapping_size;
	uint64_t capability;
	uint32_t version;
	uint32_t page_size;
	unsigned requested_queue_entries;
	unsigned maximum_queue_id;
};

enum drv_nvme_capability_reason {
	DRV_NVME_CAP_MAPPING_SHORT = 1U << 0,
	DRV_NVME_CAP_ALL_ZERO = 1U << 1,
	DRV_NVME_CAP_ALL_ONE = 1U << 2,
	DRV_NVME_CAP_VERSION = 1U << 3,
	DRV_NVME_CAP_COMMAND_SET = 1U << 4,
	DRV_NVME_CAP_PAGE_SIZE = 1U << 5,
	DRV_NVME_CAP_QUEUE_DEPTH = 1U << 6,
	DRV_NVME_CAP_DOORBELL_EXTENT = 1U << 7
};

enum drv_nvme_ready_state {
	DRV_NVME_READY_WAIT,
	DRV_NVME_READY_MATCH,
	DRV_NVME_READY_FATAL,
	DRV_NVME_READY_UNREACHABLE
};

struct drv_nvme_completion_cursor {
	uint32_t head;
	uint32_t depth;
	uint8_t phase;
};

enum drv_nvme_identify_controller_reason {
	DRV_NVME_ID_CTRL_BUFFER_SHORT = 1U << 0,
	DRV_NVME_ID_CTRL_SQ_ENTRY_SIZE = 1U << 1,
	DRV_NVME_ID_CTRL_CQ_ENTRY_SIZE = 1U << 2,
	DRV_NVME_ID_CTRL_NO_NAMESPACES = 1U << 3
};

/*
 * What Identify Controller tells of the controller: the namespace count, MDTS,
 * and the Host Memory Buffer it would take (HMPRE, HMMIN and HMMINDS in
 * 4 KiB units, HMMAXD entries; zero is no preference, no minimum, no limit).
 */
struct drv_nvme_controller_profile {
	uint32_t namespace_count;
	uint8_t maximum_transfer_shift;
	uint32_t hmb_preferred;
	uint32_t hmb_minimum;
	uint32_t hmb_minimum_descriptor;
	uint16_t hmb_maximum_descriptors;
};

enum drv_nvme_active_namespace_reason {
	DRV_NVME_ACTIVE_BUFFER_SHORT = 1U << 0,
	DRV_NVME_ACTIVE_NONE = 1U << 1,
	DRV_NVME_ACTIVE_MULTIPLE = 1U << 2,
	DRV_NVME_ACTIVE_RANGE = 1U << 3
};

enum drv_nvme_identify_namespace_reason {
	DRV_NVME_ID_NS_BUFFER_SHORT = 1U << 0,
	DRV_NVME_ID_NS_SIZE = 1U << 1,
	DRV_NVME_ID_NS_CAPACITY = 1U << 2,
	DRV_NVME_ID_NS_USAGE = 1U << 3,
	DRV_NVME_ID_NS_FORMAT = 1U << 4,
	DRV_NVME_ID_NS_METADATA = 1U << 5,
	DRV_NVME_ID_NS_BLOCK_SIZE = 1U << 6
};

struct drv_nvme_namespace_profile {
	uint64_t block_count;
	uint64_t capacity_blocks;
	uint64_t used_blocks;
	uint8_t formatted_lba_index;
	uint8_t block_size_shift;
};

static inline uint16_t
drv_nvme_load_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8);
}

static inline uint32_t
drv_nvme_load_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static inline uint64_t
drv_nvme_load_le64(const uint8_t *bytes)
{
	return (uint64_t)drv_nvme_load_le32(bytes) |
	    ((uint64_t)drv_nvme_load_le32(bytes + 4U) << 32);
}

static inline int
drv_nvme_power_of_two(uint32_t value)
{
	return value != 0U && (value & (value - 1U)) == 0U;
}

static inline uint32_t
drv_nvme_cap_page_min(uint64_t capability)
{
	return UINT32_C(1) << (12U + (unsigned)((capability >> 48) & 15U));
}

static inline uint32_t
drv_nvme_cap_page_max(uint64_t capability)
{
	return UINT32_C(1) << (12U + (unsigned)((capability >> 52) & 15U));
}

static inline unsigned
drv_nvme_cap_queue_entries(uint64_t capability)
{
	return (unsigned)(capability & UINT64_C(0xffff)) + 1U;
}

static inline unsigned
drv_nvme_cap_timeout_ms(uint64_t capability)
{
	unsigned units = (unsigned)((capability >> 24) & 0xffU);

	/* NVMe defines a zero CAP.TO as a minimum wait of 500 milliseconds. */
	return (units == 0U ? 1U : units) * 500U;
}

static inline size_t
drv_nvme_doorbell_stride(uint64_t capability)
{
	return (size_t)4U << (unsigned)((capability >> 32) & 15U);
}

static inline int
drv_nvme_doorbell_offset(uint64_t capability, unsigned queue_id,
	int completion, size_t mapping_size, size_t *offset_out)
{
	size_t index, offset, stride;

	if (offset_out == NULL || queue_id > UINT16_MAX ||
	    (completion != 0 && completion != 1))
		return 0;
	stride = drv_nvme_doorbell_stride(capability);
	index = (size_t)queue_id * 2U + (unsigned)completion;
	if (index > (SIZE_MAX - DRV_NVME_REG_DOORBELL_BASE) / stride)
		return 0;
	offset = DRV_NVME_REG_DOORBELL_BASE + index * stride;
	if (offset > mapping_size || sizeof(uint32_t) > mapping_size - offset)
		return 0;
	*offset_out = offset;
	return 1;
}

static inline unsigned
drv_nvme_selected_queue_depth(uint64_t capability, unsigned requested)
{
	unsigned maximum = drv_nvme_cap_queue_entries(capability);

	return requested < maximum ? requested : maximum;
}

static inline uint32_t
drv_nvme_capability_validate(
	const struct drv_nvme_capability_snapshot *snapshot)
{
	uint32_t reasons = 0;
	uint32_t minimum_page, maximum_page;
	size_t ignored;

	if (snapshot == NULL)
		return DRV_NVME_CAP_MAPPING_SHORT;
	if (snapshot->mapping_size < DRV_NVME_REG_DOORBELL_BASE)
		reasons |= DRV_NVME_CAP_MAPPING_SHORT;
	if (snapshot->capability == 0U && snapshot->version == 0U)
		reasons |= DRV_NVME_CAP_ALL_ZERO;
	if (snapshot->capability == UINT64_MAX &&
	    snapshot->version == UINT32_MAX)
		reasons |= DRV_NVME_CAP_ALL_ONE;
	if ((snapshot->version >> 16) == 0U ||
	    snapshot->version == UINT32_MAX)
		reasons |= DRV_NVME_CAP_VERSION;
	/* CAP.CSS bit 0 (CAP bit 37) advertises the NVM command set. */
	if ((snapshot->capability & (UINT64_C(1) << 37)) == 0U)
		reasons |= DRV_NVME_CAP_COMMAND_SET;
	minimum_page = drv_nvme_cap_page_min(snapshot->capability);
	maximum_page = drv_nvme_cap_page_max(snapshot->capability);
	if (!drv_nvme_power_of_two(snapshot->page_size) ||
	    snapshot->page_size < minimum_page ||
	    snapshot->page_size > maximum_page)
		reasons |= DRV_NVME_CAP_PAGE_SIZE;
	if (snapshot->requested_queue_entries < 2U ||
	    drv_nvme_selected_queue_depth(snapshot->capability,
	    snapshot->requested_queue_entries) < 2U)
		reasons |= DRV_NVME_CAP_QUEUE_DEPTH;
	if (!drv_nvme_doorbell_offset(snapshot->capability,
	    snapshot->maximum_queue_id, 1, snapshot->mapping_size, &ignored))
		reasons |= DRV_NVME_CAP_DOORBELL_EXTENT;
	return reasons;
}

static inline const char *
drv_nvme_capability_reason_name(uint32_t reasons)
{
	if (reasons & DRV_NVME_CAP_MAPPING_SHORT)
		return "mapping-short";
	if (reasons & DRV_NVME_CAP_ALL_ZERO)
		return "all-zero";
	if (reasons & DRV_NVME_CAP_ALL_ONE)
		return "all-one";
	if (reasons & DRV_NVME_CAP_VERSION)
		return "version";
	if (reasons & DRV_NVME_CAP_COMMAND_SET)
		return "command-set";
	if (reasons & DRV_NVME_CAP_PAGE_SIZE)
		return "page-size";
	if (reasons & DRV_NVME_CAP_QUEUE_DEPTH)
		return "queue-depth";
	if (reasons & DRV_NVME_CAP_DOORBELL_EXTENT)
		return "doorbell-extent";
	return "ok";
}

static inline uint32_t
drv_nvme_controller_configuration(uint32_t page_size, int enable)
{
	unsigned shift = 0;
	uint32_t value;

	if (!drv_nvme_power_of_two(page_size) || page_size < 4096U)
		return 0U;
	while ((UINT32_C(1) << shift) != page_size && shift < 31U)
		shift++;
	if (shift < 12U || shift > 27U)
		return 0U;
	value = (uint32_t)(shift - 12U) << 7;
	value |= 6U << 16; /* 64-byte submission entries. */
	value |= 4U << 20; /* 16-byte completion entries. */
	if (enable)
		value |= DRV_NVME_CC_ENABLE;
	return value;
}

static inline uint32_t
drv_nvme_admin_queue_attributes(unsigned depth)
{
	/* AQA.ASQS and AQA.ACQS are 12-bit zero-based fields. */
	return depth >= 2U && depth <= 4096U ?
	    (uint32_t)(depth - 1U) | ((uint32_t)(depth - 1U) << 16) : 0U;
}

static inline int
drv_nvme_queue_bytes(unsigned depth, size_t entry_size, size_t *bytes_out)
{
	if (bytes_out == NULL || depth < 2U || entry_size == 0U ||
	    depth > SIZE_MAX / entry_size)
		return 0;
	*bytes_out = (size_t)depth * entry_size;
	return 1;
}

static inline enum drv_nvme_ready_state
drv_nvme_controller_ready_state(uint32_t status, int expected_ready)
{
	if (status == UINT32_MAX)
		return DRV_NVME_READY_UNREACHABLE;
	if ((status & DRV_NVME_CSTS_FATAL) != 0U)
		return DRV_NVME_READY_FATAL;
	return ((status & DRV_NVME_CSTS_READY) != 0U) ==
	    (expected_ready != 0) ? DRV_NVME_READY_MATCH : DRV_NVME_READY_WAIT;
}

/*
 * CSTS.CFS describes why a live controller needs resetting; it is not proof
 * that the reset failed.  A disable/reset waiter must therefore continue past
 * CFS while RDY is set and accept RDY clear even if CFS has not cleared yet.
 * An all-ones MMIO read remains unreachable and can never prove quiescence.
 */
static inline enum drv_nvme_ready_state
drv_nvme_controller_disable_state(uint32_t status)
{
	if (status == UINT32_MAX)
		return DRV_NVME_READY_UNREACHABLE;
	return (status & DRV_NVME_CSTS_READY) == 0U ?
	    DRV_NVME_READY_MATCH : DRV_NVME_READY_WAIT;
}

static inline void
drv_nvme_command_clear(struct drv_nvme_command *command)
{
	uint8_t *bytes = (uint8_t *)command;
	size_t index;

	if (command == NULL)
		return;
	for (index = 0; index < sizeof(*command); index++)
		bytes[index] = 0U;
}

static inline int
drv_nvme_identify_command(struct drv_nvme_command *command,
	uint16_t command_id, uint32_t namespace_id, uint8_t selector,
	uint64_t buffer_address)
{
	/* The initial profile uses one page-sized Identify buffer and PRP1 only. */
	if (command == NULL || buffer_address == 0U ||
	    (buffer_address & UINT64_C(0xfff)) != 0U ||
	    (selector != DRV_NVME_IDENTIFY_NAMESPACE &&
	    selector != DRV_NVME_IDENTIFY_CONTROLLER &&
	    selector != DRV_NVME_IDENTIFY_ACTIVE_LIST) ||
	    (selector == DRV_NVME_IDENTIFY_NAMESPACE) != (namespace_id != 0U))
		return 0;
	drv_nvme_command_clear(command);
	command->cdw0 = DRV_NVME_ADMIN_IDENTIFY | ((uint32_t)command_id << 16);
	command->namespace_id = namespace_id;
	command->prp1 = buffer_address;
	command->cdw10 = selector;
	return 1;
}

static inline int
drv_nvme_set_queue_count_command(struct drv_nvme_command *command,
	uint16_t command_id, unsigned submission_count,
	unsigned completion_count)
{
	if (command == NULL || submission_count == 0U ||
	    submission_count > 65536U || completion_count == 0U ||
	    completion_count > 65536U)
		return 0;
	drv_nvme_command_clear(command);
	command->cdw0 = DRV_NVME_ADMIN_SET_FEATURES |
	    ((uint32_t)command_id << 16);
	command->cdw10 = DRV_NVME_FEATURE_NUMBER_OF_QUEUES;
	command->cdw11 = (uint32_t)(submission_count - 1U) |
	    ((uint32_t)(completion_count - 1U) << 16);
	return 1;
}

/*
 * Builds the Set Features command that enables (with memory_return when the
 * buffer the controller had before is given back unchanged) or disables the
 * Host Memory Buffer of pages memory pages, described by count entries at
 * list (16-byte aligned).
 */
static inline int
drv_nvme_set_hmb_command(struct drv_nvme_command *command, uint16_t command_id,
	int enable, int memory_return, uint32_t pages, uint64_t list, uint32_t count)
{
	if (command == NULL || (list & 15U) != 0U ||
	    (enable && (pages == 0U || count == 0U || list == 0U)))
		return 0;
	drv_nvme_command_clear(command);
	command->cdw0 = DRV_NVME_ADMIN_SET_FEATURES | ((uint32_t)command_id << 16);
	command->cdw10 = DRV_NVME_FEATURE_HOST_MEMORY_BUFFER;
	command->cdw11 = (enable ? DRV_NVME_HMB_ENABLE : 0U) |
	    (enable && memory_return ? DRV_NVME_HMB_MEMORY_RETURN : 0U);
	command->cdw12 = enable ? pages : 0U;
	command->cdw13 = enable ? (uint32_t)list : 0U;
	command->cdw14 = enable ? (uint32_t)(list >> 32) : 0U;
	command->cdw15 = enable ? count : 0U;
	return 1;
}

/* Writes one Host Memory Buffer descriptor: a page-aligned address and its size in memory pages. */
static inline void
drv_nvme_hmb_descriptor(uint8_t *entry, uint64_t address, uint32_t pages)
{
	unsigned index;

	for (index = 0; index < 8U; index++)
		entry[index] = (uint8_t)(address >> (8U * index));
	for (index = 0; index < 4U; index++)
		entry[8U + index] = (uint8_t)(pages >> (8U * index));
	for (index = 12U; index < DRV_NVME_HMB_DESCRIPTOR_SIZE; index++)
		entry[index] = 0;
}

/*
 * Plans the Host Memory Buffer to give a controller: up to the smaller of
 * its preference and limit bytes, in chunks of a power of two up to
 * chunk_limit bytes, no smaller than its minimum descriptor size nor
 * floor bytes, and no more than count_limit of them (or HMMAXD).  Reports
 * the total to aim at (zero: give none), the first chunk size, the
 * smallest chunk size, the most chunks and the least total the controller
 * accepts.
 */
static inline int
drv_nvme_hmb_plan(const struct drv_nvme_controller_profile *profile,
	uint64_t limit, uint64_t chunk_limit, uint64_t floor, unsigned count_limit,
	uint64_t *target, uint64_t *chunk, uint64_t *smallest, unsigned *count,
	uint64_t *minimum)
{
	uint64_t wanted, size, least;
	unsigned most;

	if (profile == NULL || target == NULL || chunk == NULL ||
	    smallest == NULL || count == NULL || minimum == NULL)
		return 0;
	*target = 0;
	*chunk = 0;
	*smallest = 0;
	*count = 0;
	*minimum = 0;
	if (profile->hmb_preferred == 0U || chunk_limit == 0U || count_limit == 0U)
		return 1;
	wanted = (uint64_t)profile->hmb_preferred * 4096U;
	if (wanted > limit)
		wanted = limit;
	least = (uint64_t)profile->hmb_minimum * 4096U;
	if (least > wanted || wanted == 0U)
		return 1;
	size = 1;
	while (size * 2U <= wanted && size * 2U <= chunk_limit)
		size *= 2U;
	floor = floor > (uint64_t)profile->hmb_minimum_descriptor * 4096U ?
	    floor : (uint64_t)profile->hmb_minimum_descriptor * 4096U;
	if (floor > size)
		return 1;
	if (least == 0U)
		least = floor;
	most = count_limit;
	if (profile->hmb_maximum_descriptors != 0U &&
	    profile->hmb_maximum_descriptors < most)
		most = profile->hmb_maximum_descriptors;
	*target = wanted;
	*chunk = size;
	*smallest = floor;
	*count = most;
	*minimum = least;
	return 1;
}

/*
 * The power states a controller describes and its Autonomous Power State
 * Transition feature.
 *
 * Values referenced: NVM Express Base Specification 1.4c, Identify
 * Controller: NPSS byte 263 (the number of power states less one), APSTA
 * byte 265 bit 0, the Power State Descriptors from byte 2048, 32 bytes
 * each (MP bytes 1:0, MXPS byte 3 bit 0: units of 0.0001 W instead of
 * 0.01 W, NOPS byte 3 bit 1, ENLAT bytes 7:4 and EXLAT bytes 11:8 in
 * microseconds, zero when not reported); Set Features Feature Identifier
 * 0Ch (CDW11 APSTE bit 0, the table in the data buffer: 32 entries of 64
 * bits, one per power state, ITPS bits 7:3 and ITPT bits 31:8 in
 * milliseconds, an ITPT of zero making no transition).
 */
#define DRV_NVME_FEATURE_AUTONOMOUS_POWER	0x0CU
#define DRV_NVME_APST_ENABLE			(1U << 0)
#define DRV_NVME_POWER_STATES_MAX		32U
#define DRV_NVME_POWER_STATE_DESCRIPTORS	2048U
#define DRV_NVME_POWER_STATE_DESCRIPTOR_SIZE	32U
#define DRV_NVME_APST_ENTRY_SIZE		8U
#define DRV_NVME_APST_TABLE_SIZE		(DRV_NVME_POWER_STATES_MAX * DRV_NVME_APST_ENTRY_SIZE)
#define DRV_NVME_APST_ITPT_MAX			0x00ffffffU

/*
 * The latencies the transitions may cost: a state whose entry and exit
 * latencies add up to more than the limit is never a target, and one
 * that is out within the quick latency is entered after the short idle
 * time, a slower one after the long idle time.
 * Values referenced: Linux 6.19 drivers/nvme/host/core.c (the defaults of
 * the module parameters default_ps_max_latency_us 100000,
 * apst_primary_latency_tol_us 15000, apst_primary_timeout_ms 100 and
 * apst_secondary_timeout_ms 2000, as read from
 * /sys/module/nvme_core/parameters on Debian with Linux 6.19.13); values
 * only.  They also give the table Debian programs on a Kioxia BG5.
 * Procedure referenced: the same file (the shorter idle time for a target
 * within the quick latency, the longer one for a slower target).
 */
#define DRV_NVME_APST_LATENCY_LIMIT_US		100000U
#define DRV_NVME_APST_QUICK_LATENCY_US		15000U
#define DRV_NVME_APST_QUICK_IDLE_MS		100U
#define DRV_NVME_APST_SLOW_IDLE_MS		2000U

/* One power state: its most power in microwatts, its latencies, and whether it runs commands. */
struct drv_nvme_power_state {
	uint32_t max_power_uw;
	uint32_t entry_latency_us;
	uint32_t exit_latency_us;
	uint8_t non_operational;
};

/* The power states Identify Controller describes, and whether the controller makes autonomous transitions. */
struct drv_nvme_power_profile {
	unsigned count;
	uint8_t apst_supported;
	struct drv_nvme_power_state states[DRV_NVME_POWER_STATES_MAX];
};

/*
 * What a table of autonomous transitions does: for each power state, the
 * state it goes to and after how long idle (its own number and zero when
 * it stays), and how many states make a transition.
 */
struct drv_nvme_apst_summary {
	unsigned transitions;
	uint8_t target[DRV_NVME_POWER_STATES_MAX];
	uint32_t idle_ms[DRV_NVME_POWER_STATES_MAX];
};

/* Reads the power states and APSTA from an Identify Controller page; reports whether the page was whole. */
static inline int
drv_nvme_power_parse(
	const uint8_t *identify,
	size_t size,
	struct drv_nvme_power_profile *profile)
{
	const uint8_t *descriptor;
	unsigned index;
	uint32_t power;

	/* Refuses a page too short to hold the descriptors. */
	if (identify == NULL || profile == NULL)
		return 0;
	if (size < DRV_NVME_POWER_STATE_DESCRIPTORS + DRV_NVME_POWER_STATES_MAX * DRV_NVME_POWER_STATE_DESCRIPTOR_SIZE)
		return 0;

	/* Reads the count of the states, and whether the controller moves among them by itself. */
	profile->count = (unsigned)identify[263U] + 1U;
	profile->apst_supported = (uint8_t)(identify[265U] & 1U);

	/* Reads each described state; the rest stay zero. */
	for (index = 0; index < DRV_NVME_POWER_STATES_MAX; index++) {
		descriptor = identify + DRV_NVME_POWER_STATE_DESCRIPTORS + index * DRV_NVME_POWER_STATE_DESCRIPTOR_SIZE;
		if (index >= profile->count) {
			profile->states[index].max_power_uw = 0;
			profile->states[index].entry_latency_us = 0;
			profile->states[index].exit_latency_us = 0;
			profile->states[index].non_operational = 0;
			continue;
		}

		/* The most power is in units of 0.01 W, or of 0.0001 W when MXPS is set. */
		power = drv_nvme_load_le16(descriptor);
		if ((descriptor[3] & 1U) != 0U)
			power *= 100U;
		else
			power *= 10000U;
		profile->states[index].max_power_uw = power;
		profile->states[index].non_operational = (uint8_t)((descriptor[3] >> 1) & 1U);
		profile->states[index].entry_latency_us = drv_nvme_load_le32(descriptor + 4U);
		profile->states[index].exit_latency_us = drv_nvme_load_le32(descriptor + 8U);
	}

	/* Succeeded: the profile describes the controller. */
	return 1;
}

/*
 * Finds the state an idle controller in state from goes to: the first
 * state numbered after it that is non-operational, has both latencies
 * reported, costs no more than latency_limit_us to enter and leave, and
 * draws no more power than from.  Reports whether there is one.
 */
static inline int
drv_nvme_apst_target(
	const struct drv_nvme_power_profile *profile,
	unsigned from,
	uint32_t latency_limit_us,
	unsigned *target)
{
	const struct drv_nvme_power_state *state;
	uint64_t latency;
	unsigned index;

	/* Looks at the states after from in order, so that the shallowest one that qualifies is chosen. */
	for (index = from + 1U; index < profile->count && index < DRV_NVME_POWER_STATES_MAX; index++) {
		state = &profile->states[index];

		/* An operational state is not a place to idle in. */
		if (!state->non_operational)
			continue;

		/* A state that does not say what it costs is not chosen blind. */
		if (state->entry_latency_us == 0U || state->exit_latency_us == 0U)
			continue;

		/* A state too slow to come back from is left alone. */
		latency = (uint64_t)state->entry_latency_us + state->exit_latency_us;
		if (latency > latency_limit_us)
			continue;

		/* A state that may draw more than the one it is entered from saves nothing. */
		if (state->max_power_uw > profile->states[from].max_power_uw)
			continue;

		*target = index;

		/* Succeeded: the state found. */
		return 1;
	}

	/* No state qualifies: the controller stays in from. */
	return 0;
}

/*
 * Builds the table of autonomous transitions in table (size bytes, at
 * least DRV_NVME_APST_TABLE_SIZE) and describes it in summary: each state
 * goes to its drv_nvme_apst_target() after DRV_NVME_APST_QUICK_IDLE_MS
 * idle when the target is out within DRV_NVME_APST_QUICK_LATENCY_US, or
 * after DRV_NVME_APST_SLOW_IDLE_MS when it is slower.  A table with no
 * transition is all zero.
 */
static inline int
drv_nvme_apst_build(
	const struct drv_nvme_power_profile *profile,
	uint32_t latency_limit_us,
	uint8_t *table,
	size_t size,
	struct drv_nvme_apst_summary *summary)
{
	const struct drv_nvme_power_state *state;
	uint64_t latency;
	uint32_t idle_ms;
	uint32_t entry;
	unsigned index;
	unsigned target;
	unsigned byte;
	int found;

	/* Refuses a table too small for the controller's states. */
	if (profile == NULL || table == NULL || summary == NULL)
		return EINVAL;
	if (size < DRV_NVME_APST_TABLE_SIZE || profile->count == 0U || profile->count > DRV_NVME_POWER_STATES_MAX)
		return EINVAL;

	/* Starts from a table in which no state moves. */
	for (byte = 0; byte < DRV_NVME_APST_TABLE_SIZE; byte++)
		table[byte] = 0;
	summary->transitions = 0;

	/* Gives each state its target and idle time. */
	for (index = 0; index < DRV_NVME_POWER_STATES_MAX; index++) {
		summary->target[index] = (uint8_t)index;
		summary->idle_ms[index] = 0;
		if (index >= profile->count)
			continue;

		/* A state with nowhere to go keeps its zero entry. */
		found = drv_nvme_apst_target(profile, index, latency_limit_us, &target);
		if (!found)
			continue;

		/* A target that is quick to leave is entered sooner. */
		state = &profile->states[target];
		latency = (uint64_t)state->entry_latency_us + state->exit_latency_us;
		idle_ms = DRV_NVME_APST_SLOW_IDLE_MS;
		if (latency <= DRV_NVME_APST_QUICK_LATENCY_US)
			idle_ms = DRV_NVME_APST_QUICK_IDLE_MS;

		/* Writes the entry: ITPS in bits 7:3, ITPT in bits 31:8, the upper word reserved. */
		entry = ((uint32_t)target << 3) | (idle_ms << 8);
		table[index * DRV_NVME_APST_ENTRY_SIZE + 0U] = (uint8_t)entry;
		table[index * DRV_NVME_APST_ENTRY_SIZE + 1U] = (uint8_t)(entry >> 8);
		table[index * DRV_NVME_APST_ENTRY_SIZE + 2U] = (uint8_t)(entry >> 16);
		table[index * DRV_NVME_APST_ENTRY_SIZE + 3U] = (uint8_t)(entry >> 24);
		summary->target[index] = (uint8_t)target;
		summary->idle_ms[index] = idle_ms;
		summary->transitions++;
	}

	/* Succeeded: the table is ready to send. */
	return 0;
}

/*
 * Builds the Set Features that enables (or disables) the autonomous power
 * state transitions, with the table at table_address: one data buffer of
 * DRV_NVME_APST_TABLE_SIZE bytes, dword aligned, in one memory page.
 */
static inline int
drv_nvme_apst_command(
	struct drv_nvme_command *command,
	uint16_t command_id,
	int enable,
	uint64_t table_address)
{
	uint64_t offset;

	/* Refuses a table the command's one PRP entry cannot describe. */
	if (command == NULL || table_address == 0U || (table_address & 3U) != 0U)
		return 0;
	offset = table_address & UINT64_C(0xfff);
	if (offset + DRV_NVME_APST_TABLE_SIZE > UINT64_C(0x1000))
		return 0;

	/* Builds the command. */
	drv_nvme_command_clear(command);
	command->cdw0 = DRV_NVME_ADMIN_SET_FEATURES | ((uint32_t)command_id << 16);
	command->prp1 = table_address;
	command->cdw10 = DRV_NVME_FEATURE_AUTONOMOUS_POWER;
	command->cdw11 = 0;
	if (enable)
		command->cdw11 = DRV_NVME_APST_ENABLE;

	/* Succeeded: the command is ready to send. */
	return 1;
}

static inline int
drv_nvme_queue_count_result(uint32_t result, unsigned *submission_count,
	unsigned *completion_count)
{
	unsigned submission_zero_based, completion_zero_based;

	if (submission_count == NULL || completion_count == NULL)
		return 0;
	submission_zero_based = (unsigned)(result & UINT32_C(0xffff));
	completion_zero_based = (unsigned)(result >> 16);
	*submission_count = submission_zero_based + 1U;
	*completion_count = completion_zero_based + 1U;
	return 1;
}

static inline int
drv_nvme_create_io_cq_command(struct drv_nvme_command *command,
	uint16_t command_id, unsigned queue_id, unsigned depth,
	uint64_t buffer_address, unsigned interrupt_vector,
	int interrupt_enabled)
{
	/* This initial PCI profile selects a 4-KiB CC.MPS and contiguous queues. */
	if (command == NULL || queue_id == 0U || queue_id > UINT16_MAX ||
	    depth < 2U || depth > 65536U || buffer_address == 0U ||
	    (buffer_address & UINT64_C(0xfff)) != 0U ||
	    interrupt_vector > UINT16_MAX ||
	    (interrupt_enabled != 0 && interrupt_enabled != 1))
		return 0;
	drv_nvme_command_clear(command);
	command->cdw0 = DRV_NVME_ADMIN_CREATE_IO_CQ |
	    ((uint32_t)command_id << 16);
	command->prp1 = buffer_address;
	command->cdw10 = (uint32_t)queue_id |
	    ((uint32_t)(depth - 1U) << 16);
	command->cdw11 = 1U | ((uint32_t)interrupt_enabled << 1) |
	    ((uint32_t)interrupt_vector << 16);
	return 1;
}

static inline int
drv_nvme_create_io_sq_command(struct drv_nvme_command *command,
	uint16_t command_id, unsigned queue_id, unsigned completion_queue_id,
	unsigned depth, uint64_t buffer_address)
{
	/* QPRIO is zero (urgent) and PC is one for coherent contiguous memory. */
	if (command == NULL || queue_id == 0U || queue_id > UINT16_MAX ||
	    completion_queue_id == 0U || completion_queue_id > UINT16_MAX ||
	    depth < 2U || depth > 65536U || buffer_address == 0U ||
	    (buffer_address & UINT64_C(0xfff)) != 0U)
		return 0;
	drv_nvme_command_clear(command);
	command->cdw0 = DRV_NVME_ADMIN_CREATE_IO_SQ |
	    ((uint32_t)command_id << 16);
	command->prp1 = buffer_address;
	command->cdw10 = (uint32_t)queue_id |
	    ((uint32_t)(depth - 1U) << 16);
	command->cdw11 = 1U | ((uint32_t)completion_queue_id << 16);
	return 1;
}

static inline int
drv_nvme_io_range_valid(uint64_t first_block, uint32_t block_count,
	uint64_t namespace_blocks)
{
	return block_count != 0U && first_block < namespace_blocks &&
	    (uint64_t)block_count <= namespace_blocks - first_block;
}

static inline int
drv_nvme_single_prp_transfer_valid(uint64_t buffer_address,
	uint32_t block_size, uint32_t block_count, uint32_t page_size)
{
	uint64_t bytes, offset;

	if (buffer_address == 0U || (buffer_address & 3U) != 0U ||
	    block_size == 0U || block_count == 0U ||
	    !drv_nvme_power_of_two(page_size))
		return 0;
	bytes = (uint64_t)block_size * block_count;
	offset = buffer_address & (uint64_t)(page_size - 1U);
	return bytes <= (uint64_t)page_size - offset &&
	    buffer_address <= UINT64_MAX - (bytes - 1U);
}

static inline uint32_t
drv_nvme_io_chunk_blocks(uint64_t remaining_blocks, uint32_t block_size,
	size_t bounce_bytes, size_t controller_bytes)
{
	size_t limit, blocks;

	if (remaining_blocks == 0U || block_size == 0U ||
	    bounce_bytes < block_size || controller_bytes < block_size)
		return 0U;
	limit = bounce_bytes < controller_bytes ? bounce_bytes :
	    controller_bytes;
	blocks = limit / block_size;
	if (blocks > 65536U)
		blocks = 65536U;
	if ((uint64_t)blocks > remaining_blocks)
		blocks = (size_t)remaining_blocks;
	return (uint32_t)blocks;
}

static inline int
drv_nvme_io_command(struct drv_nvme_command *command, uint16_t command_id,
	uint8_t opcode, uint32_t namespace_id, uint64_t first_block,
	uint32_t block_count, uint64_t buffer_address)
{
	if (command == NULL || namespace_id == 0U ||
	    namespace_id == UINT32_MAX)
		return 0;
	if (opcode == DRV_NVME_NVM_FLUSH) {
		if (first_block != 0U || block_count != 0U ||
		    buffer_address != 0U)
			return 0;
	} else if (opcode == DRV_NVME_NVM_READ ||
	    opcode == DRV_NVME_NVM_WRITE) {
		if (block_count == 0U || block_count > 65536U ||
		    buffer_address == 0U)
			return 0;
	} else {
		return 0;
	}
	drv_nvme_command_clear(command);
	command->cdw0 = opcode | ((uint32_t)command_id << 16);
	command->namespace_id = namespace_id;
	if (opcode != DRV_NVME_NVM_FLUSH) {
		command->prp1 = buffer_address;
		command->cdw10 = (uint32_t)first_block;
		command->cdw11 = (uint32_t)(first_block >> 32);
		command->cdw12 = block_count - 1U;
	}
	return 1;
}

static inline int
drv_nvme_completion_available(const struct drv_nvme_completion *completion,
	unsigned expected_phase)
{
	return completion != NULL && (completion->status & 1U) ==
	    (expected_phase & 1U);
}

static inline int
drv_nvme_completion_matches(const struct drv_nvme_completion *completion,
	unsigned expected_phase, uint16_t expected_submission_id,
	uint16_t expected_submission_head, uint16_t expected_command_id)
{
	return drv_nvme_completion_available(completion, expected_phase) &&
	    completion->submission_id == expected_submission_id &&
	    completion->submission_head == expected_submission_head &&
	    completion->command_id == expected_command_id;
}

static inline unsigned
drv_nvme_completion_status_code(const struct drv_nvme_completion *completion)
{
	return completion == NULL ? UINT32_MAX :
	    (unsigned)((completion->status >> 1) & 0xffU);
}

static inline unsigned
drv_nvme_completion_status_type(const struct drv_nvme_completion *completion)
{
	return completion == NULL ? UINT32_MAX :
	    (unsigned)((completion->status >> 9) & 7U);
}

static inline int
drv_nvme_completion_success(const struct drv_nvme_completion *completion)
{
	return completion != NULL &&
	    drv_nvme_completion_status_code(completion) == 0U &&
	    drv_nvme_completion_status_type(completion) == 0U;
}

static inline int
drv_nvme_completion_error(const struct drv_nvme_completion *completion)
{
	unsigned code, type;

	if (completion == NULL)
		return EINVAL;
	type = drv_nvme_completion_status_type(completion);
	code = drv_nvme_completion_status_code(completion);
	if (type == DRV_NVME_STATUS_GENERIC) {
		switch (code) {
		case 0x00U:
			return 0;
		case 0x01U:
			return EOPNOTSUPP;
		case 0x02U:
		case 0x0aU:
		case 0x0cU:
			return EINVAL;
		case 0x03U:
		case 0x1dU:
		case 0x82U:
		case 0x83U:
		case 0x84U:
			return EBUSY;
		case 0x07U:
		case 0x08U:
			return ECANCELED;
		case 0x0bU:
			return ENXIO;
		case 0x14U:
		case 0x23U:
			return EACCES;
		case 0x20U:
			return EROFS;
		case 0x21U:
		case 0x22U:
			return EAGAIN;
		case 0x80U:
			return EOVERFLOW;
		case 0x81U:
			return ENOSPC;
		default:
			return EIO;
		}
	}
	if (type == DRV_NVME_STATUS_COMMAND_SPECIFIC) {
		if (code <= 0x02U)
			return EINVAL;
		if (code == 0x82U)
			return EROFS;
		if (code == 0x83U)
			return EOVERFLOW;
	}
	return EIO;
}

static inline int
drv_nvme_completion_cursor_init(struct drv_nvme_completion_cursor *cursor,
	unsigned depth)
{
	if (cursor == NULL || depth < 2U || depth > 65536U)
		return 0;
	cursor->head = 0;
	cursor->depth = depth;
	cursor->phase = 1U;
	return 1;
}

static inline void
drv_nvme_completion_cursor_advance(struct drv_nvme_completion_cursor *cursor)
{
	if (cursor == NULL || cursor->depth < 2U)
		return;
	cursor->head++;
	if (cursor->head == cursor->depth) {
		cursor->head = 0;
		cursor->phase ^= 1U;
	}
}

static inline unsigned
drv_nvme_queue_index_advance(unsigned index, unsigned depth)
{
	return depth >= 2U && index < depth ?
	    (index + 1U == depth ? 0U : index + 1U) : UINT32_MAX;
}

static inline uint32_t
drv_nvme_identify_controller_validate(const uint8_t *identify, size_t size,
	struct drv_nvme_controller_profile *profile)
{
	uint32_t reasons = 0;
	uint8_t sqes, cqes;
	uint32_t namespaces;

	if (identify == NULL || size < DRV_NVME_IDENTIFY_SIZE) {
		if (profile != NULL) {
			profile->namespace_count = 0;
			profile->maximum_transfer_shift = 0;
			profile->hmb_preferred = 0;
			profile->hmb_minimum = 0;
			profile->hmb_minimum_descriptor = 0;
			profile->hmb_maximum_descriptors = 0;
		}
		return DRV_NVME_ID_CTRL_BUFFER_SHORT;
	}
	sqes = identify[512U];
	cqes = identify[513U];
	namespaces = drv_nvme_load_le32(identify + 516U);
	if ((sqes & 15U) > 6U || (sqes >> 4) < 6U)
		reasons |= DRV_NVME_ID_CTRL_SQ_ENTRY_SIZE;
	if ((cqes & 15U) > 4U || (cqes >> 4) < 4U)
		reasons |= DRV_NVME_ID_CTRL_CQ_ENTRY_SIZE;
	if (namespaces == 0U)
		reasons |= DRV_NVME_ID_CTRL_NO_NAMESPACES;
	if (profile != NULL) {
		profile->namespace_count = namespaces;
		profile->maximum_transfer_shift = identify[77U];
		/*
		 * Values referenced: NVM Express Base Specification 1.4c,
		 * Identify Controller: HMPRE bytes 275:272, HMMIN 279:276,
		 * HMMINDS 335:332, HMMAXD 337:336.
		 */
		profile->hmb_preferred = drv_nvme_load_le32(identify + 272U);
		profile->hmb_minimum = drv_nvme_load_le32(identify + 276U);
		profile->hmb_minimum_descriptor = drv_nvme_load_le32(identify + 332U);
		profile->hmb_maximum_descriptors = drv_nvme_load_le16(identify + 336U);
	}
	return reasons;
}

static inline uint32_t
drv_nvme_active_namespace_validate(const uint8_t *identify, size_t size,
	uint32_t reported_namespace_count, uint32_t *namespace_id)
{
	uint32_t selected = 0;
	uint32_t reasons = 0;
	unsigned active_count = 0;
	unsigned index;

	/* Identify Controller.NN is a namespace count, not an upper bound on a
	 * namespace identifier.  NSIDs may therefore be sparse.  The initial
	 * driver profile accepts exactly one active entry and validates the NSID
	 * against the independently defined 1..0xfffffffe range. */
	(void)reported_namespace_count;
	if (namespace_id != NULL)
		*namespace_id = 0;
	if (identify == NULL || size < DRV_NVME_IDENTIFY_SIZE)
		return DRV_NVME_ACTIVE_BUFFER_SHORT;
	for (index = 0; index < DRV_NVME_IDENTIFY_SIZE / sizeof(uint32_t);
	    index++) {
		uint32_t candidate = drv_nvme_load_le32(identify + index * 4U);

		if (candidate == 0U)
			continue;
		active_count++;
		if (active_count == 1U)
			selected = candidate;
		else
			reasons |= DRV_NVME_ACTIVE_MULTIPLE;
		if (candidate == UINT32_MAX)
			reasons |= DRV_NVME_ACTIVE_RANGE;
	}
	if (active_count == 0U)
		reasons |= DRV_NVME_ACTIVE_NONE;
	if (namespace_id != NULL && reasons == 0U)
		*namespace_id = selected;
	return reasons;
}

static inline uint32_t
drv_nvme_identify_namespace_validate(const uint8_t *identify, size_t size,
	struct drv_nvme_namespace_profile *profile)
{
	uint64_t blocks, capacity, used;
	uint16_t metadata_size = 0;
	uint8_t formatted = 0, block_shift = 0;
	uint32_t reasons = 0;

	if (identify == NULL || size < DRV_NVME_IDENTIFY_SIZE) {
		if (profile != NULL) {
			profile->block_count = 0;
			profile->capacity_blocks = 0;
			profile->used_blocks = 0;
			profile->formatted_lba_index = 0;
			profile->block_size_shift = 0;
		}
		return DRV_NVME_ID_NS_BUFFER_SHORT;
	}
	blocks = drv_nvme_load_le64(identify);
	capacity = drv_nvme_load_le64(identify + 8U);
	used = drv_nvme_load_le64(identify + 16U);
	formatted = identify[26U] & 15U;
	if (blocks == 0U)
		reasons |= DRV_NVME_ID_NS_SIZE;
	if (capacity == 0U || capacity > blocks)
		reasons |= DRV_NVME_ID_NS_CAPACITY;
	if (used > capacity)
		reasons |= DRV_NVME_ID_NS_USAGE;
	if (formatted > identify[25U] || (identify[26U] & 0xe0U) != 0U)
		reasons |= DRV_NVME_ID_NS_FORMAT;
	else {
		const uint8_t *format = identify + 128U + (size_t)formatted * 4U;

		metadata_size = drv_nvme_load_le16(format);
		block_shift = format[2U];
		if (metadata_size != 0U || (identify[26U] & 0x10U) != 0U)
			reasons |= DRV_NVME_ID_NS_METADATA;
		/*
		 * Logical blocks of 512 bytes to a page (4096): the sizes the
		 * disk layer and the filesystems above it read.
		 */
		if (block_shift < 9U || block_shift > 12U)
			reasons |= DRV_NVME_ID_NS_BLOCK_SIZE;
	}
	if (profile != NULL) {
		profile->block_count = blocks;
		profile->capacity_blocks = capacity;
		profile->used_blocks = used;
		profile->formatted_lba_index = formatted;
		profile->block_size_shift = block_shift;
	}
	return reasons;
}

static inline size_t
drv_nvme_max_transfer_bytes(uint8_t maximum_transfer_shift,
	size_t minimum_page_size, size_t policy_limit)
{
	if (minimum_page_size == 0U || policy_limit == 0U ||
	    maximum_transfer_shift == 0U)
		return policy_limit;
	if (maximum_transfer_shift >= sizeof(size_t) * 8U ||
	    minimum_page_size > policy_limit >> maximum_transfer_shift)
		return policy_limit;
	return minimum_page_size << maximum_transfer_shift;
}

/*
 * Fills the PRP entries of one read or write whose data lies in whole pages.
 *
 * pages holds the bus address of each page in transfer order, every one
 * aligned to page_size.  One page goes in PRP1 alone, two in PRP1 and PRP2,
 * and more in PRP1 and a PRP list: PRP2 is then the list's bus address and
 * the list holds the second and later pages (NVMe Base Specification 2.0,
 * 4.1.1).  The list never chains, so it holds at most list_capacity entries,
 * and list_capacity is at most one page of entries.  The command's other
 * fields are the caller's.  Reports 1 when the entries were filled and 0
 * for inputs it refuses.
 */
static __inline int
drv_nvme_prp_fill(
	struct drv_nvme_command *command,
	const uint64_t *pages,
	uint32_t page_count,
	uint32_t page_size,
	uint64_t *list,
	uint64_t list_bus_address,
	uint32_t list_capacity)
{
	uint64_t page_mask;
	uint32_t index;

	/* Refuses a missing command, no pages, or a page size the rules cannot use. */
	if (command == NULL || pages == NULL || page_count == 0U)
		return 0;
	if (!drv_nvme_power_of_two(page_size))
		return 0;
	page_mask = (uint64_t)page_size - 1U;

	/* Refuses a page that is absent or not aligned to a page. */
	for (index = 0; index < page_count; index++) {
		if (pages[index] == 0U)
			return 0;
		if ((pages[index] & page_mask) != 0U)
			return 0;
	}

	/* The first page is PRP1 whatever follows it. */
	command->prp1 = pages[0];
	command->prp2 = 0U;

	/* One page needs nothing more. */
	if (page_count == 1U)
		return 1;

	/* Two pages put the second straight in PRP2. */
	if (page_count == 2U) {
		command->prp2 = pages[1];
		return 1;
	}

	/* More pages need a page-aligned list that holds all but the first without a chain. */
	if (list == NULL || list_bus_address == 0U)
		return 0;
	if ((list_bus_address & page_mask) != 0U)
		return 0;
	if (list_capacity > page_size / 8U)
		return 0;
	if (page_count - 1U > list_capacity)
		return 0;

	/* Writes the second and later pages into the list, in order. */
	for (index = 1; index < page_count; index++)
		list[index - 1U] = pages[index];

	/* Succeeded: PRP2 names the list. */
	command->prp2 = list_bus_address;
	return 1;
}

/*
 * An I/O command identifier names its slot: the slot's number plus one in
 * the low DRV_NVME_IO_CID_SLOT_BITS bits, and the slot's generation (how
 * many commands it has carried, wrapping) above them.  It is never zero, and
 * two commands outstanding at once never share one, because their slots
 * differ.  A queue has at most DRV_NVME_IO_CID_SLOTS slots.
 */
#define DRV_NVME_IO_CID_SLOT_BITS	6U
#define DRV_NVME_IO_CID_SLOTS		((1U << DRV_NVME_IO_CID_SLOT_BITS) - 1U)

/* Builds the identifier of a slot's command from the slot's number and generation. */
static inline uint16_t
drv_nvme_io_command_id(unsigned slot, unsigned generation)
{
	/* Reports the generation above the slot number plus one. */
	return (uint16_t)(((generation << DRV_NVME_IO_CID_SLOT_BITS) |
	    (slot + 1U)) & 0xffffU);
}

/*
 * Advances a slot's generation and names its next command, skipping FFFFh,
 * which the error information log uses for an error of no command.
 */
static inline uint16_t
drv_nvme_io_next_command_id(unsigned slot, unsigned *generation)
{
	uint16_t command_id;

	/* Names the command by the next generation of the slot. */
	(*generation)++;
	command_id = drv_nvme_io_command_id(slot, *generation);

	/* Steps over the one identifier the device reserves. */
	if (command_id == 0xffffU) {
		(*generation)++;
		command_id = drv_nvme_io_command_id(slot, *generation);
	}

	/* Reports the identifier of the slot's next command. */
	return command_id;
}

/* Finds the slot a command identifier names; 0 when it names none of slot_count. */
static inline int
drv_nvme_io_command_slot(uint16_t command_id, unsigned slot_count, unsigned *slot)
{
	unsigned low;

	/* Refuses an identifier whose slot number is not one of the queue's slots. */
	low = command_id & DRV_NVME_IO_CID_SLOTS;
	if (low == 0U || low > slot_count || slot_count > DRV_NVME_IO_CID_SLOTS)
		return 0;

	/* Succeeded: the identifier names slot number low minus one. */
	*slot = low - 1U;
	return 1;
}

#endif
