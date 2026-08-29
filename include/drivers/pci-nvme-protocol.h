/*
 * PCI NVMe register, queue, and Identify arithmetic
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_DRIVERS_PCI_NVME_PROTOCOL_H
#define ZEDBSD_DRIVERS_PCI_NVME_PROTOCOL_H

#include <errno.h>
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

struct drv_nvme_controller_profile {
	uint32_t namespace_count;
	uint8_t maximum_transfer_shift;
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
		if (block_shift != 9U)
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

#endif
