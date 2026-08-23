#ifndef ZEDBSD_HAL_ARM64_INT_H
#define ZEDBSD_HAL_ARM64_INT_H

#include <hal/types.h>

struct arm64_exception_frame {
	uint64_t x[31];
	uint64_t elr;
	uint64_t spsr;
	uint64_t esr;
	uint64_t far;
	uint64_t user_sp;
};

void arm64_int_init(void);
void arm64_sync_handler(struct arm64_exception_frame *frame, uint64_t vector);
void arm64_irq_handler(struct arm64_exception_frame *frame, int from_user);

#endif
