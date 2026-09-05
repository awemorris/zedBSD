/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/input-device.h"
#include "kern/lock.h"

#include <errno.h>

#define INPUT_SUBSCRIBER_MAX 8U

static struct spinlock subscriber_lock;
static struct input_subscription *subscribers[INPUT_SUBSCRIBER_MAX];

void
input_subscriber_init(void)
{
	unsigned index;

	spin_init(&subscriber_lock, LOCK_RANK_DEVICE, "input subscribers");
	for (index = 0; index < INPUT_SUBSCRIBER_MAX; index++)
		subscribers[index] = NULL;
}

int
input_subscribe(struct input_subscription *subscription,
	input_subscriber_callback_t callback, void *context)
{
	unsigned long irq;
	unsigned index;

	if (subscription == NULL || callback == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&subscriber_lock);
	for (index = 0; index < INPUT_SUBSCRIBER_MAX; index++)
		if (subscribers[index] == subscription) {
			spin_unlock_irqrestore(&subscriber_lock, irq);
			return EBUSY;
		}
	for (index = 0; index < INPUT_SUBSCRIBER_MAX; index++)
		if (subscribers[index] == NULL)
			break;
	if (index == INPUT_SUBSCRIBER_MAX) {
		spin_unlock_irqrestore(&subscriber_lock, irq);
		return ENOSPC;
	}
	subscription->callback = callback;
	subscription->context = context;
	subscription->registered = 1;
	subscribers[index] = subscription;
	spin_unlock_irqrestore(&subscriber_lock, irq);
	return 0;
}

void
input_unsubscribe(struct input_subscription *subscription)
{
	unsigned long irq;
	unsigned index;

	if (subscription == NULL)
		return;
	irq = spin_lock_irqsave(&subscriber_lock);
	for (index = 0; index < INPUT_SUBSCRIBER_MAX; index++)
		if (subscribers[index] == subscription) {
			subscribers[index] = NULL;
			break;
		}
	subscription->registered = 0;
	subscription->callback = NULL;
	subscription->context = NULL;
	spin_unlock_irqrestore(&subscriber_lock, irq);
}

/*
 * The callback must be bounded and nonblocking.  Publication never holds an
 * input-device state lock.  Keeping this registry lock across the callback
 * makes input_unsubscribe() a join point for every already-admitted call.
 */
void
input_subscriber_publish(const struct input_report *report)
{
	unsigned long irq;
	unsigned index;

	if (report == NULL)
		return;
	irq = spin_lock_irqsave(&subscriber_lock);
	for (index = 0; index < INPUT_SUBSCRIBER_MAX; index++) {
		struct input_subscription *subscription = subscribers[index];
		if (subscription != NULL && subscription->registered)
			subscription->callback(subscription->context, report);
	}
	spin_unlock_irqrestore(&subscriber_lock, irq);
}
