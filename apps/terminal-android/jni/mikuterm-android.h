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
#define MIKUTERM_POST_SFTP_UPLOAD	0x6d6b0008U
#define MIKUTERM_POST_SFTP_DOWNLOAD_DIR	0x6d6b0009U

#include <jni.h>

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

/* The data of MIKUTERM_POST_SFTP_UPLOAD: target view number and array of virtual local paths. */
struct mikuterm_post_sftp_upload {
	uint32_t view_number;
	size_t count;
	char **paths;
};

/* The data of MIKUTERM_POST_SFTP_DOWNLOAD_DIR: target view number, persisted tree URI, and virtual dir. */
struct mikuterm_post_sftp_dir {
	uint32_t view_number;
	char *tree_uri;
	char *virtual_dir;
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
int mikuterm_java_sftp_pick_upload(uint32_t view_number);
int mikuterm_java_sftp_pick_download_dir(uint32_t view_number);

/* JNI attachment and SftpStorage class helpers for worker threads (mikuterm-jni.c). */
int mikuterm_jni_attach(JNIEnv **env_out, int *attached_out);
void mikuterm_jni_detach(int attached);
jclass mikuterm_jni_sftp_storage_class(void);
jobject mikuterm_jni_app_context(JNIEnv *env);

/* Configures the persisted SAF download tree URI on Android (sftp-local-android.c). */
void sftp_local_android_set_download_uri(const char *uri);

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
