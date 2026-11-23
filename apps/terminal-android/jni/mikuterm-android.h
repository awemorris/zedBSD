/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mikuterm's body on Android, which the program's native thread (M) runs,
 * and what its files tell one another: the posts the Java side and the
 * connections' threads make for M, and the calls M makes into Java.
 */

#ifndef MIKUTERM_ANDROID_H
#define MIKUTERM_ANDROID_H

#include <stddef.h>
#include <stdint.h>

/*
 * The codes of the posts made with keiland_post_app, which M's hook sees
 * (mikuterm_android_post).  The two numbers below 0x6d6b0003 are not used.
 */
#define MIKUTERM_POST_SECRET		0x6d6b0003U
#define MIKUTERM_POST_NETWORK		0x6d6b0004U
#define MIKUTERM_POST_STOP_ALL		0x6d6b0005U
#define MIKUTERM_POST_KEEP_SECRET	0x6d6b0006U
#define MIKUTERM_POST_FORGET_SECRET	0x6d6b0007U

/*
 * The data of MIKUTERM_POST_SECRET and MIKUTERM_POST_NETWORK, all allocated
 * by the poster and freed by the hook: the request's number (0 for the
 * network), whether it worked, the network's handle, and the bytes of a
 * secret's answer.
 */
struct mikuterm_post_result {
	uint32_t request;
	int ok;
	int64_t value;
	size_t size;
	uint8_t *bytes;
};

/* The data of MIKUTERM_POST_KEEP_SECRET and MIKUTERM_POST_FORGET_SECRET: the key file's path and the passphrase (NULL to forget), both allocated by the poster. */
struct mikuterm_post_secret {
	char *path;
	char *passphrase;
};

/*
 * Sets the program up (its data, its settings, its fonts) and runs the terminal
 * until its last tab or its window closes.
 */
void mikuterm_android_main(void);

/* M's hook for the posts of the codes above.  Returns 1 when it took the post (the data is its to free), 0 for another code. */
int mikuterm_android_post(uint32_t code, void *data);

/*
 * The calls into Java (jni/mikuterm-jni.c), made on M; each returns at once, and
 * the answer to a secret comes as MIKUTERM_POST_SECRET.  They return 0, ENOMEM or EIO.
 */
int mikuterm_java_secret_wrap(uint32_t request, const uint8_t *bytes, size_t size);
int mikuterm_java_secret_unwrap(uint32_t request, const uint8_t *bytes, size_t size);
int mikuterm_java_service_update(unsigned connections);

/*
 * The passphrases kept on the disk (credential-android.c), on M: reads what
 * was kept and asks Java to open it; takes the answers of Java's wrapping
 * and opening; and keeps or forgets one on the disk as a connection's thread
 * asked (the posts above).
 */
void mikuterm_credential_start(void);
void mikuterm_credential_answer(struct mikuterm_post_result *result);
void mikuterm_credential_keep(struct mikuterm_post_secret *post);
void mikuterm_credential_forget(struct mikuterm_post_secret *post);

#endif /* MIKUTERM_ANDROID_H */
