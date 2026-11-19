/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MC68040 exception dispatch: interrupts to the board, trap #0 to the
 * kernel's system calls, and faults to the kernel's fault handlers.
 */

#include <hal/hal.h>
#include "asm.h"
#include "board.h"
#include "defs.h"
#include "int.h"
#include "space.h"
#include "task.h"

/*
 * The access-error frame (format 7), as offsets past its format word: the
 * special status word, the three write-back status words, the fault
 * address, and each write-back's address and data.
 *
 * Values referenced: M68040 User's Manual (MC68040UM/AD), section 8.4.6
 * (the format $7 frame, the special status word, the write-back status
 * format and data alignment, and the order the handler completes the
 * write-backs in: 1, 2, then 3).
 */
#define FRAME7_SPECIAL_STATUS		4
#define FRAME7_WB3_STATUS		6
#define FRAME7_WB2_STATUS		8
#define FRAME7_WB1_STATUS		10
#define FRAME7_FAULT_ADDRESS		12
#define FRAME7_WB3_ADDRESS		16
#define FRAME7_WB3_DATA			20
#define FRAME7_WB2_ADDRESS		24
#define FRAME7_WB2_DATA			28
#define FRAME7_WB1_ADDRESS		32
#define FRAME7_WB1_DATA			36

/* The frame format of an access error. */
#define FRAME_FORMAT_ACCESS		7U

/* The special status word's fault flag, read flag and transfer-modifier field. */
#define SSW_ATC				0x0400
#define SSW_READ			0x0100
#define SSW_TM_MASK			0x0007
#define SSW_TM_CODE			0x0002

/*
 * A write-back status word: valid, the size (long, byte, word or line),
 * the transfer type (normal or MOVE16) and the transfer modifier, which is
 * the function code the write goes to.
 */
#define WB_VALID			0x80
#define WB_SIZE_SHIFT			5
#define WB_SIZE_MASK			0x3
#define WB_SIZE_LONG			0
#define WB_SIZE_BYTE			1
#define WB_SIZE_WORD			2
#define WB_TT_SHIFT			3
#define WB_TT_MASK			0x3
#define WB_TT_NORMAL			0
#define WB_TM_MASK			0x7
#define WB_TM_SUPERVISOR		0x4

/*
 * The classification of one exception for the kernel.
 */
struct int_fault {
	enum hal_trap_cause cause;
	enum hal_trap_mode mode;
	uintptr_t address;
};

static unsigned frame_vector(const struct m68040_trap_frame *frame);
static const uint8_t *frame_extra(const struct m68040_trap_frame *frame);
static uint32_t frame_long(const struct m68040_trap_frame *frame, unsigned offset);
static uint16_t frame_word(const struct m68040_trap_frame *frame, unsigned offset);
static void classify(const struct m68040_trap_frame *frame, struct int_fault *fault);
static void take_interrupt(struct m68040_trap_frame *frame, unsigned level, int from_user);
static void take_syscall(struct m68040_trap_frame *frame);
static void take_user_fault(struct m68040_trap_frame *frame);
static void take_kernel_fault(struct m68040_trap_frame *frame);
static void describe_frame(const struct m68040_trap_frame *frame);
static void complete_writebacks(const struct m68040_trap_frame *frame);
static void complete_writeback(uint16_t status, uint32_t address, uint32_t data, int memory_aligned);

/*
 * Points the processor's exceptions at the HAL.
 */
void
m68040_int_init(
	void)
{
	/* Every vector enters at the same place. */
	m68040_set_vbr(m68040_vector_table);
	hal_puts("M68040 TRAP READY\n");
}

/*
 * Takes one exception, called by the entry with the frame it built.
 */
void
m68040_trap(
	struct m68040_trap_frame *frame)
{
	unsigned vector;
	int from_user;

	/* The saved status register says where the processor came from. */
	from_user = 0;
	if ((frame->sr & M68040_SR_SUPERVISOR) == 0)
		from_user = 1;

	/* An interrupt goes to the board by its level. */
	vector = frame_vector(frame);
	if (vector >= M68040_VECTOR_AUTOVECTOR_1 && vector <= M68040_VECTOR_AUTOVECTOR_7) {
		take_interrupt(frame, vector - M68040_VECTOR_AUTOVECTOR_1 + 1U, from_user);
		return;
	}

	/* A spurious interrupt has nothing to serve. */
	if (vector == M68040_VECTOR_SPURIOUS)
		return;

	/* trap #0 from user is a system call. */
	if (from_user && vector == M68040_VECTOR_TRAP_0) {
		take_syscall(frame);
		return;
	}

	/* Any other exception is a fault of whichever side took it. */
	if (from_user) {
		take_user_fault(frame);
		return;
	}

	/* Anything else is the kernel's own fault. */
	take_kernel_fault(frame);
}

/* Reports the vector number of a frame. */
static unsigned
frame_vector(
	const struct m68040_trap_frame *frame)
{
	/* The low twelve bits are the vector offset, four bytes a vector. */
	return (unsigned)(frame->format & 0x0fffU) >> 2;
}

/* Reports where the processor's frame goes on past its format word. */
static const uint8_t *
frame_extra(
	const struct m68040_trap_frame *frame)
{
	/* Right after the frame the entry knows. */
	return (const uint8_t *)frame + sizeof(*frame);
}

/* Reads a big-endian 32-bit word of the processor's frame past its format word. */
static uint32_t
frame_long(
	const struct m68040_trap_frame *frame,
	unsigned offset)
{
	const uint8_t *bytes;

	/* The frame is only two-byte aligned past its first fields. */
	bytes = frame_extra(frame) + offset;
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | bytes[3];
}

/* Reads a 16-bit word of the processor's frame past its format word. */
static uint16_t
frame_word(
	const struct m68040_trap_frame *frame,
	unsigned offset)
{
	const uint8_t *bytes;

	/* Big-endian, as every word of the frame. */
	bytes = frame_extra(frame) + offset;
	return (uint16_t)(((unsigned)bytes[0] << 8) | bytes[1]);
}

/* Turns an exception into the kernel's cause, access mode and address. */
static void
classify(
	const struct m68040_trap_frame *frame,
	struct int_fault *fault)
{
	unsigned vector;
	uint16_t status;

	/* Starts from a fault with no mode and no address. */
	fault->cause = HAL_TRAP_CAUSE_OTHER;
	fault->mode = HAL_TRAP_MODE_NONE;
	fault->address = 0;

	/* Chooses by the vector. */
	vector = frame_vector(frame);
	switch (vector) {
	case M68040_VECTOR_ACCESS:
		/* The format-7 frame names the address and the kind of access. */
		fault->cause = HAL_TRAP_CAUSE_PAGE_FAULT;
		fault->address = frame_long(frame, FRAME7_FAULT_ADDRESS);
		status = frame_word(frame, FRAME7_SPECIAL_STATUS);
		if ((status & SSW_TM_MASK) == SSW_TM_CODE) {
			fault->mode = HAL_TRAP_MODE_EXEC;
		} else if ((status & SSW_READ) != 0) {
			fault->mode = HAL_TRAP_MODE_READ;
		} else {
			fault->mode = HAL_TRAP_MODE_WRITE;
		}

		break;
	case M68040_VECTOR_ADDRESS:
		fault->cause = HAL_TRAP_CAUSE_ALIGNMENT;
		break;
	case M68040_VECTOR_ILLEGAL:
	case M68040_VECTOR_PRIVILEGE:
	case M68040_VECTOR_LINE_A:
	case M68040_VECTOR_LINE_F:
		fault->cause = HAL_TRAP_CAUSE_ILLEGAL_INSN;
		break;
	case M68040_VECTOR_TRACE:
		fault->cause = HAL_TRAP_CAUSE_BREAKPOINT;
		break;
	default:
		break;
	}
}

/*
 * Serves an interrupt.  One that came from user code is served with the
 * task's user frame recorded and goes back through the kernel's return
 * path.
 */
static void
take_interrupt(
	struct m68040_trap_frame *frame,
	unsigned level,
	int from_user)
{
	/* The frame is the task's user context while the interrupt is served. */
	if (from_user)
		m68040_task_enter_user_frame(frame);

	/* The board finds the sources behind the level. */
	m68040_board_interrupt(level);

	/* A user context goes back through the kernel's return path. */
	if (from_user) {
		kernel_user_return_handler();
		m68040_task_leave_user_frame();
	}
}

/*
 * Serves a system call: the number in d0, the arguments in d1 to d5 and
 * a0, the result back in d0.  trap leaves the program counter past the
 * instruction.
 */
static void
take_syscall(
	struct m68040_trap_frame *frame)
{
	uintptr_t arguments[HAL_SYSCALL_ARGS];
	intptr_t result;
	unsigned index;

	/* Copies the arguments before the kernel may change the frame. */
	for (index = 0; index < 5U; index++)
		arguments[index] = (uintptr_t)frame->d[index + 1U];
	arguments[5] = (uintptr_t)frame->a[0];

	/* The kernel serves the call with the frame as the user context. */
	m68040_task_enter_user_frame(frame);
	result = kernel_syscall_handler(frame->d[0], arguments);
	frame->d[0] = (uint32_t)result;
	kernel_user_return_handler();
	m68040_task_leave_user_frame();
}

/*
 * Hands a fault of user code to the kernel, which either fixes it or
 * arranges the task's end.
 */
static void
take_user_fault(
	struct m68040_trap_frame *frame)
{
	struct int_fault fault;
	int handled;

	/* The kernel sees the cause and the task's frame. */
	classify(frame, &fault);
	m68040_task_enter_user_frame(frame);
	handled = kernel_user_fault_handler(fault.cause,
					    fault.mode,
					    (uintptr_t)frame->pc,
					    fault.address,
					    (uintptr_t)frame_vector(frame),
					    (uintptr_t)frame->format);

	/* A fault the kernel could not take stops the system. */
	if (handled != HAL_TRAP_RET_SUCCESS) {
		m68040_task_leave_user_frame();
		describe_frame(frame);
		HAL_FATAL("m68040 user fault handler failed");
	}

	/* The writes the fault held back go to memory now that their pages may be there. */
	complete_writebacks(frame);

	/* The kernel's return path, then the frame is no longer the task's context. */
	kernel_user_return_handler();
	m68040_task_leave_user_frame();
}

/*
 * Hands a fault of the kernel to the kernel's handler, which may fix it;
 * any other is fatal.
 */
static void
take_kernel_fault(
	struct m68040_trap_frame *frame)
{
	struct int_fault fault;
	int handled;

	/* The kernel's handler sees the cause and the faulting instruction. */
	classify(frame, &fault);
	handled = kernel_sys_fault_handler(fault.cause,
					   fault.mode,
					   (uintptr_t)frame->pc,
					   fault.address,
					   (uintptr_t)frame_vector(frame),
					   (uintptr_t)frame->format);

	/* Handled: the held-back writes complete, and the return goes on. */
	if (handled == HAL_TRAP_RET_SUCCESS) {
		complete_writebacks(frame);
		return;
	}

	/* Anything else stops the system with the frame on the console. */
	describe_frame(frame);
	HAL_FATAL("unhandled m68040 kernel exception");
}

/* Writes a frame's registers to the console. */
static void
describe_frame(
	const struct m68040_trap_frame *frame)
{
	unsigned index;

	/* The processor's fields first. */
	hal_printf("m68040 trap vector=%u sr=%x pc=%x format=%x usp=%x\n",
		   frame_vector(frame),
		   (uint32_t)frame->sr,
		   frame->pc,
		   (uint32_t)frame->format,
		   frame->usp);

	/* Then the registers, four to a line. */
	for (index = 0; index < 8U; index += 4U) {
		hal_printf("d%u=%x d%u=%x d%u=%x d%u=%x\n",
			   index, frame->d[index], index + 1U, frame->d[index + 1U],
			   index + 2U, frame->d[index + 2U], index + 3U, frame->d[index + 3U]);
	}

	/* Then the address registers. */
	hal_printf("a0=%x a1=%x a2=%x a3=%x\n", frame->a[0], frame->a[1], frame->a[2], frame->a[3]);
	hal_printf("a4=%x a5=%x a6=%x\n", frame->a[4], frame->a[5], frame->a[6]);
}


/*
 * Completes the writes an access error held back.  The processor leaves
 * a faulted write and up to two later ones to the handler, which must
 * write them once the fault is corrected; the return does not repeat
 * them.  Only a translation fault is corrected by the kernel, and each
 * write is made only when its destination is now writable, so that a
 * fault the kernel answered with a signal does not fault again here.
 */
static void
complete_writebacks(
	const struct m68040_trap_frame *frame)
{
	uint16_t status;
	uint16_t wb_status;
	uint32_t wb_address;
	uint32_t wb_data;

	/* Only an access error's frame holds write-backs. */
	if (frame_vector(frame) != M68040_VECTOR_ACCESS)
		return;
	if (((unsigned)frame->format >> 12) != FRAME_FORMAT_ACCESS)
		return;

	/* A physical bus error is not corrected by the kernel. */
	status = frame_word(frame, FRAME7_SPECIAL_STATUS);
	if ((status & SSW_ATC) == 0)
		return;

	/* The first write-back, which is the faulted write when it is valid. */
	wb_status = frame_word(frame, FRAME7_WB1_STATUS);
	wb_address = frame_long(frame, FRAME7_WB1_ADDRESS);
	wb_data = frame_long(frame, FRAME7_WB1_DATA);
	complete_writeback(wb_status, wb_address, wb_data, 1);

	/* The second, held before translation. */
	wb_status = frame_word(frame, FRAME7_WB2_STATUS);
	wb_address = frame_long(frame, FRAME7_WB2_ADDRESS);
	wb_data = frame_long(frame, FRAME7_WB2_DATA);
	complete_writeback(wb_status, wb_address, wb_data, 0);

	/* The third, held in the integer unit. */
	wb_status = frame_word(frame, FRAME7_WB3_STATUS);
	wb_address = frame_long(frame, FRAME7_WB3_ADDRESS);
	wb_data = frame_long(frame, FRAME7_WB3_DATA);
	complete_writeback(wb_status, wb_address, wb_data, 0);
}

/*
 * Completes one held-back write.  The first write-back's data is aligned
 * as on the memory bus; the others' is aligned in the register, the byte
 * or the word in the low bits.
 */
static void
complete_writeback(
	uint16_t status,
	uint32_t address,
	uint32_t data,
	int memory_aligned)
{
	uint32_t size;
	uint32_t value;
	uint32_t function_code;
	unsigned rotate;
	bool supervisor;
	bool writable;

	/* A slot with no pending write has nothing to do. */
	if ((status & WB_VALID) == 0)
		return;

	/* A MOVE16 or a whole line is restarted by the instruction, not written here. */
	if (((status >> WB_TT_SHIFT) & WB_TT_MASK) != WB_TT_NORMAL)
		return;

	/* The size in bytes. */
	switch ((status >> WB_SIZE_SHIFT) & WB_SIZE_MASK) {
	case WB_SIZE_LONG:
		size = 4U;
		break;
	case WB_SIZE_BYTE:
		size = 1U;
		break;
	case WB_SIZE_WORD:
		size = 2U;
		break;
	default:
		return;
	}

	/*
	 * Turns bus-aligned data into register-aligned data: the byte of the
	 * address's lane goes to the top, then the value moves to the low bits.
	 */
	value = data;
	if (memory_aligned) {
		rotate = (address & 3U) * 8U;
		if (rotate != 0)
			value = (data << rotate) | (data >> (32U - rotate));
		value >>= 32U - size * 8U;
	}

	/* Writes only where the write cannot fault again. */
	function_code = status & WB_TM_MASK;
	supervisor = false;
	if ((function_code & WB_TM_SUPERVISOR) != 0)
		supervisor = true;
	writable = m68040_space_writable(address, size, supervisor);
	if (!writable) {
		hal_printf("m68040 write-back to %x dropped\n", address);
		return;
	}

	/* Writes the value in the space the processor would have written it in. */
	m68040_write_as(address, value, size, function_code);
}
