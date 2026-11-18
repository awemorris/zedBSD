/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The answers the compositor owes the seat (libkeiland-backend's
 * kl_backend_seat_device_let_go): an input callback that found the input
 * thread still reading the descriptor (or about to stop) is answered once
 * the input thread has acknowledged the detach that stops it, in the order
 * the callbacks came.  The detaches' acknowledgements are numbered by the
 * input thread's detach sequence (input-thread.c), which only grows.
 *
 * The record also says since when an acknowledgement is awaited: an input
 * thread that does not let go in time leaves a descriptor it may still read,
 * which the compositor cannot give back safely.
 *
 * The main thread alone uses it.
 */

#ifndef KWL_SEAT_ANSWER_H
#define KWL_SEAT_ANSWER_H

#include <stdint.h>

/* The most answers owed at once: a pause, a resume and a removal of every input. */
#define KWL_SEAT_ANSWERS_MAX	96U

/* One answer owed: the descriptor to let go once the detach numbered sequence is acknowledged. */
struct kwl_seat_answer {
	uint64_t sequence;
	int descriptor;
};

/*
 * The answers owed, oldest first (count of them); the last detach asked
 * and the last acknowledged as the main thread last looked; since when an
 * acknowledgement is awaited, in milliseconds (0 while none is).
 */
struct kwl_seat_answers {
	struct kwl_seat_answer list[KWL_SEAT_ANSWERS_MAX];
	unsigned count;
	uint64_t asked;
	uint64_t acked;
	uint64_t waiting_since_ms;
};

void kwl_seat_answers_init(struct kwl_seat_answers *answers);
void kwl_seat_answers_asked(struct kwl_seat_answers *answers, uint64_t sequence, uint64_t now_ms);
void kwl_seat_answers_heard(struct kwl_seat_answers *answers, uint64_t acked, uint64_t now_ms);
int kwl_seat_answers_pending(const struct kwl_seat_answers *answers, uint64_t sequence);
int kwl_seat_answers_add(struct kwl_seat_answers *answers, uint64_t sequence, int descriptor);
int kwl_seat_answers_next(struct kwl_seat_answers *answers, int *descriptor);
int kwl_seat_answers_late(const struct kwl_seat_answers *answers, uint64_t now_ms, uint64_t limit_ms, uint64_t *waited_ms);

#endif
