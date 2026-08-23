/* Deterministic host test for libc's SIGEV_THREAD handle/lifecycle rules. */
#include <stdint.h>
#include <string.h>

#include "userland/base/libc/timer.c"

static int test_errno;
static struct sigaction mock_actions[NSIG];
static uint32_t next_kernel_timer = 0x80000101U;
static unsigned worker_creates;
static unsigned callback_creates;
static unsigned callback_count;
static int callback_sum;
static size_t callback_guard;
static int callback_detach;

int *__libc_errno_location(void) { return &test_errno; }
void __signal_restorer(void) { }

intptr_t
__syscall6(uint32_t number, uintptr_t a, uintptr_t b, uintptr_t c,
	uintptr_t d, uintptr_t e, uintptr_t f)
{
	(void)d;
	(void)e;
	(void)f;
	switch (number) {
	case ZEDBSD_SYS_sigaction: {
		int signo = (int)a;
		if (signo <= 0 || signo >= NSIG)
			return -EINVAL;
		if (c != 0)
			*(struct sigaction *)c = mock_actions[signo];
		if (b != 0)
			mock_actions[signo] = *(const struct sigaction *)b;
		return 0;
	}
	case ZEDBSD_SYS_sigprocmask:
	case ZEDBSD_SYS_usync:
		return 0;
	case ZEDBSD_SYS_timer_create:
		*(timer_t *)c = (timer_t)next_kernel_timer;
		next_kernel_timer += 0x100U;
		return 0;
	case ZEDBSD_SYS_timer_delete:
	case ZEDBSD_SYS_timer_settime:
	case ZEDBSD_SYS_timer_gettime:
	case ZEDBSD_SYS_timer_getoverrun:
		return 0;
	default:
		return -ENOSYS;
	}
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{ (void)mutex; return 0; }
int pthread_mutex_unlock(pthread_mutex_t *mutex)
{ (void)mutex; return 0; }
int pthread_attr_init(pthread_attr_t *attributes)
{
	if (attributes == NULL)
		return EINVAL;
	memset(attributes, 0, sizeof(*attributes));
	attributes->stacksize = PTHREAD_STACK_MIN;
	attributes->guardsize = 4096U;
	return 0;
}
int pthread_attr_destroy(pthread_attr_t *attributes)
{ return attributes == NULL ? EINVAL : 0; }
int pthread_attr_setdetachstate(pthread_attr_t *attributes, int state)
{
	if (attributes == NULL)
		return EINVAL;
	attributes->detachstate = state;
	return 0;
}
int
pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
	void *(*start)(void *), void *argument)
{
	if (thread != NULL)
		*thread = (pthread_t)(worker_creates + callback_creates + 1U);
	if (start == timer_worker) {
		worker_creates++;
		return 0;
	}
	callback_creates++;
	callback_guard = attributes->guardsize;
	callback_detach = attributes->detachstate;
	(void)start(argument);
	return 0;
}
int sigemptyset(sigset_t *set)
{ if (set == NULL) return -1; *set = 0; return 0; }
int sigaddset(sigset_t *set, int signo)
{
	if (set == NULL || signo <= 0 || signo >= NSIG)
		return -1;
	*set |= (sigset_t)1ULL << ((unsigned)signo - 1U);
	return 0;
}
int sched_yield(void) { return 0; }

static void
test_callback(union sigval value)
{
	callback_count++;
	callback_sum += value.sival_int;
}

static int
same_action(const struct sigaction *left, const struct sigaction *right)
{
	return memcmp(left, right, sizeof(*left)) == 0;
}

#define CHECK(expression) do { if (!(expression)) return __LINE__; } while (0)

int
main(void)
{
	struct sigaction public_action, private_action;
	struct sigevent event;
	struct libc_timer_slot *record;
	pthread_attr_t attributes;
	siginfo_t information;
	timer_t first, second, replacement, kernel_timer;
	uint32_t stale_id, stale_count;
	unsigned stale_slot;

	CHECK(sizeof(sigset_t) == 8U);
	CHECK(SIGRTMAX - SIGRTMIN + 1 >= 8);
	CHECK(__ZEDBSD_SIGEV_THREAD_SIGNAL > SIGRTMAX);
	memset(&public_action, 0x35, sizeof(public_action));
	memset(&private_action, 0x57, sizeof(private_action));
	mock_actions[SIGRTMAX] = public_action;
	mock_actions[LIBC_TIMER_WAKE_SIGNAL] = private_action;

	memset(&event, 0, sizeof(event));
	event.sigev_notify = SIGEV_NONE;
	CHECK(timer_create(CLOCK_MONOTONIC, &event, &kernel_timer) == 0);
	CHECK(!timer_is_libc_id(kernel_timer));
	CHECK(timer_gettime(kernel_timer, &(struct itimerspec){0}) == 0);

	CHECK(pthread_attr_init(&attributes) == 0);
	attributes.guardsize = 8192U;
	event.sigev_notify = SIGEV_THREAD;
	event.sigev_notify_function = test_callback;
	event.sigev_notify_attributes = &attributes;
	event.sigev_value.sival_int = 11;
	CHECK(timer_create(CLOCK_MONOTONIC, &event, &first) == 0);
	CHECK(timer_is_libc_id(first));
	CHECK(((uint32_t)first & 0xffU) == 0);
	CHECK(worker_creates == 1U);
	CHECK(same_action(&mock_actions[SIGRTMAX], &public_action));
	CHECK(!same_action(&mock_actions[LIBC_TIMER_WAKE_SIGNAL],
	    &private_action));
	attributes.guardsize = 16384U;

	event.sigev_value.sival_int = 17;
	event.sigev_notify_attributes = NULL;
	CHECK(timer_create(CLOCK_MONOTONIC, &event, &second) == 0);
	CHECK(first != second && worker_creates == 1U);

	memset(&information, 0, sizeof(information));
	information.si_code = SI_TIMER;
	information.si_value.__sival_pad = (uint32_t)first;
	timer_signal_handler(LIBC_TIMER_WAKE_SIGNAL, &information, NULL);
	CHECK(timer_id_slot((uint32_t)first, &stale_slot) == 0);
	timer_dispatch_slot(stale_slot);
	CHECK(callback_count == 1U && callback_sum == 11);
	CHECK(callback_guard == 8192U);
	CHECK(callback_detach == PTHREAD_CREATE_DETACHED);

	/* Freeze work after the worker's exchange, then reuse the slot.  The old
	 * generation must never be interpreted as a notification for replacement. */
	timer_signal_handler(LIBC_TIMER_WAKE_SIGNAL, &information, NULL);
	record = &timer_slots[stale_slot];
	stale_id = __atomic_load_n(&record->public_id, __ATOMIC_ACQUIRE);
	stale_count = __atomic_exchange_n(&record->pending, 0, __ATOMIC_ACQ_REL);
	CHECK(stale_count == 1U);
	CHECK(timer_delete(first) == 0);
	event.sigev_value.sival_int = 23;
	CHECK(timer_create(CLOCK_MONOTONIC, &event, &replacement) == 0);
	CHECK(replacement != first);
	timer_dispatch_pending(stale_slot, stale_id, stale_count);
	CHECK(callback_count == 1U && callback_sum == 11);

	CHECK(timer_delete(second) == 0);
	CHECK(timer_delete(replacement) == 0);
	CHECK(same_action(&mock_actions[SIGRTMAX], &public_action));

	/* A fork child owns no kernel timers or worker.  It restores the private
	 * disposition, keeps public SIGRTMAX untouched, and rejects copied IDs. */
	__timer_sigev_thread_fork_child();
	CHECK(same_action(&mock_actions[LIBC_TIMER_WAKE_SIGNAL],
	    &private_action));
	CHECK(same_action(&mock_actions[SIGRTMAX], &public_action));
	errno = 0;
	CHECK(timer_gettime(replacement, &(struct itimerspec){0}) == -1);
	CHECK(errno == EINVAL);
	return 0;
}
