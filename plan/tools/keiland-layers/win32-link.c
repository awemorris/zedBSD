/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Windows layers' link check (build-win32.sh): a program that calls
 * every function of keiland/keiland_thread.h and keiland/keiland_socket.h
 * once, so that the link fails when one is not defined.  It is built, never
 * run.
 */

#include <keiland/keiland_socket.h>
#include <keiland/keiland_thread.h>

#include <stddef.h>
#include <stdint.h>

/* A static mutex and once, as a program writes them. */
static struct kl_mutex link_lock = KL_MUTEX_INIT;
static struct kl_once link_once = KL_ONCE_INIT;

static void link_run(void *argument);
static void link_once_run(void);
static void link_destroy(void *value);

/*
 * Calls each function once.
 */
int
main(void)
{
	struct kl_thread_options options = { "link", link_run, NULL, KL_THREAD_PRIORITY_NORMAL, 0U };
	struct kl_socket_wait wait = { NULL, NULL, KL_SOCKET_READABLE, 0U };
	struct kl_address addresses[2];
	struct kl_socket_waker *waker;
	struct kl_socket *accepted;
	struct kl_socket *socket;
	struct kl_thread_key key;
	struct kl_thread *thread;
	struct kl_mutex mutex;
	struct kl_cond cond;
	char text[KL_ADDRESS_TEXT];
	unsigned char bytes[4];
	size_t count;
	intptr_t native;
	int sum;

	/* The threads. */
	sum = kl_thread_start(&options, &thread);
	sum += kl_thread_is_current(thread);
	sum += kl_thread_join(thread);
	sum += kl_thread_detach(thread);
	kl_thread_sleep(1U);
	kl_thread_yield();
	sum += (int)kl_thread_cpu_count();

	/* The mutexes, conditions, once and keys. */
	sum += kl_mutex_init(&mutex);
	kl_mutex_lock(&link_lock);
	sum += kl_mutex_try(&mutex);
	kl_mutex_unlock(&link_lock);
	sum += kl_cond_init(&cond);
	kl_cond_wait(&cond, &mutex);
	sum += kl_cond_wait_for(&cond, &mutex, 1U);
	kl_cond_signal(&cond);
	kl_cond_broadcast(&cond);
	kl_cond_destroy(&cond);
	kl_mutex_destroy(&mutex);
	kl_once(&link_once, link_once_run);
	sum += kl_thread_key_create(&key, link_destroy);
	sum += kl_thread_key_set(&key, &sum);
	sum += kl_thread_key_get(&key) != NULL;
	kl_thread_key_delete(&key);

	/* The addresses. */
	sum += kl_address_parse("192.0.2.1", 1U, &addresses[0]);
	sum += kl_address_format(&addresses[0], text, sizeof(text));
	kl_address_any(KL_ADDRESS_IPV4, 1U, &addresses[0]);
	kl_address_loopback(KL_ADDRESS_IPV6, 1U, &addresses[1]);
	sum += kl_socket_resolve("localhost", 1U, KL_ADDRESS_ANY, KL_SOCKET_STREAM, addresses, 2U, &count);

	/* The sockets. */
	sum += kl_socket_open(KL_ADDRESS_IPV4, KL_SOCKET_STREAM, 0U, &socket);
	sum += kl_socket_set_nonblocking(socket, 1);
	sum += kl_socket_connect(socket, &addresses[0]);
	sum += kl_socket_connect_result(socket);
	sum += kl_socket_bind(socket, &addresses[0]);
	sum += kl_socket_listen(socket, 0U);
	sum += kl_socket_accept(socket, 0U, &accepted, &addresses[1]);
	sum += kl_socket_send(socket, bytes, sizeof(bytes), &count);
	sum += kl_socket_receive(socket, bytes, sizeof(bytes), &count);
	sum += kl_socket_send_to(socket, bytes, sizeof(bytes), &addresses[0], &count);
	sum += kl_socket_receive_from(socket, bytes, sizeof(bytes), &addresses[1], &count);
	sum += kl_socket_shutdown(socket, KL_SOCKET_SHUT_BOTH);
	sum += kl_socket_local_address(socket, &addresses[0]);
	sum += kl_socket_peer_address(socket, &addresses[1]);
	sum += kl_socket_set_option(socket, KL_SOCKET_NO_DELAY, 1);
	native = kl_socket_native(socket);
	sum += native != 0;
	kl_socket_close(socket);

	/* The wait and the wakers. */
	sum += kl_socket_waker_open(&waker);
	kl_socket_waker_ring(waker);
	wait.waker = waker;
	sum += kl_socket_poll(&wait, 1U, 0, &count);
	kl_socket_waker_close(waker);

	/* Failed when the calls' results do not sum to zero (the program is only built, never run). */
	if (sum != 0)
		return 1;

	/* Succeeded: linked. */
	return 0;
}

/* What the thread runs: nothing. */
static void
link_run(
	void *argument)
{
	/* Nothing to do. */
	(void)argument;
}

/* What the once runs: nothing. */
static void
link_once_run(void)
{
	/* Nothing to do. */
}

/* A key's destroy: nothing. */
static void
link_destroy(
	void *value)
{
	/* Nothing to free. */
	(void)value;
}
