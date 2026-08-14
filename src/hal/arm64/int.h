#ifndef ZEDBSD_HAL_ARM64_INT_H
#define ZEDBSD_HAL_ARM64_INT_H

#include <hal/types.h>

struct arm64_exception_frame {
	uint64 x[31];
	uint64 elr;
	uint64 spsr;
	uint64 esr;
	uint64 far;
	uint64 user_sp;
};

void arm64_int_init(void);
void arm64_sync_handler(struct arm64_exception_frame *frame, uint64 vector);
void arm64_irq_handler(struct arm64_exception_frame *frame);

#endif
