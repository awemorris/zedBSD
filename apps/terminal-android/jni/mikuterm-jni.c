/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mikuterm's side of the Java calls: libkeiland's JNI_OnLoad calls
 * keiland_android_app_jni_load once the library is loaded, for the program to
 * register its own native methods and look up its own classes.  The native
 * methods only queue what they are given for M (keiland_post_app) and return;
 * the calls into Java are made on M and return at once too.
 */

#include "apps/libkeiland/android/keiland-android.h"

#include "mikuterm-android.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The Java class that holds the methods, in the form FindClass takes. */
#define JNI_CLASS		"io/noctvm/mikuterm/MikutermBridge"
#define JNI_STORAGE_CLASS	"io/noctvm/mikuterm/SftpStorage"

/* The class as a global reference, and the methods M calls (found in JNI_OnLoad, as a native thread's own search does not see the program's classes). */
static jclass jni_class;
static jclass jni_storage_class;
static jmethodID jni_secret_wrap;
static jmethodID jni_secret_unwrap;
static jmethodID jni_service_update;
static jmethodID jni_get_app_context;
static jmethodID jni_sftp_pick_upload;
static jmethodID jni_sftp_pick_download_dir;

static void jni_native_secret(JNIEnv *env, jclass cls, jint request, jbyteArray data);
static void jni_native_network(JNIEnv *env, jclass cls, jboolean up, jlong handle);
static void jni_native_stop_all(JNIEnv *env, jclass cls);
static void jni_native_sftp_upload_picked(JNIEnv *env, jclass cls, jint view_number, jobjectArray paths);
static void jni_native_sftp_download_dir_picked(JNIEnv *env, jclass cls, jint view_number, jstring tree_uri, jstring virtual_dir);
static int jni_bytes_call(jmethodID method, uint32_t request, const uint8_t *bytes, size_t size);

/* The native methods, with the types JNI knows them by. */
static const JNINativeMethod jni_natives[] = {
	{ "nativeSecret", "(I[B)V", (void *)jni_native_secret },
	{ "nativeNetwork", "(ZJ)V", (void *)jni_native_network },
	{ "nativeStopAll", "()V", (void *)jni_native_stop_all },
	{ "nativeSftpUploadPicked", "(I[Ljava/lang/String;)V", (void *)jni_native_sftp_upload_picked },
	{ "nativeSftpDownloadDirPicked", "(ILjava/lang/String;Ljava/lang/String;)V", (void *)jni_native_sftp_download_dir_picked }
};

/*
 * Prepares the program's Java classes for native calls: registers the native
 * methods of MikutermBridge and finds the methods M calls.
 */
jint
keiland_android_app_jni_load(
	JNIEnv *env)
{
	jclass local;
	jint status;

	/* The class, kept. */
	local = (*env)->FindClass(env, JNI_CLASS);
	if (local == NULL)
		return JNI_ERR;
	jni_class = (*env)->NewGlobalRef(env, local);
	(*env)->DeleteLocalRef(env, local);
	if (jni_class == NULL)
		return JNI_ERR;

	/* The native methods reach this file. */
	status = (*env)->RegisterNatives(env, jni_class, jni_natives, sizeof(jni_natives) / sizeof(jni_natives[0]));
	if (status != 0)
		return JNI_ERR;

	/* The storage class, kept for worker thread JNI calls. */
	local = (*env)->FindClass(env, JNI_STORAGE_CLASS);
	if (local == NULL)
		return JNI_ERR;
	jni_storage_class = (*env)->NewGlobalRef(env, local);
	(*env)->DeleteLocalRef(env, local);
	if (jni_storage_class == NULL)
		return JNI_ERR;

	/* The methods M calls. */
	jni_secret_wrap = (*env)->GetStaticMethodID(env, jni_class, "secretWrap", "(I[B)V");
	jni_secret_unwrap = (*env)->GetStaticMethodID(env, jni_class, "secretUnwrap", "(I[B)V");
	jni_service_update = (*env)->GetStaticMethodID(env, jni_class, "serviceUpdate", "(I)V");
	jni_get_app_context = (*env)->GetStaticMethodID(env, jni_class, "getAppContext", "()Landroid/content/Context;");
	jni_sftp_pick_upload = (*env)->GetStaticMethodID(env, jni_class, "sftpPickUpload", "(I)V");
	jni_sftp_pick_download_dir = (*env)->GetStaticMethodID(env, jni_class, "sftpPickDownloadDir", "(I)V");
	if (jni_secret_wrap == NULL || jni_secret_unwrap == NULL || jni_service_update == NULL ||
	    jni_get_app_context == NULL || jni_sftp_pick_upload == NULL || jni_sftp_pick_download_dir == NULL)
		return JNI_ERR;

	/* Succeeded. */
	return JNI_OK;
}

/* Asks Java to wrap a passphrase with the Keystore's key.  Returns 0, ENOMEM or EIO. */
int
mikuterm_java_secret_wrap(
	uint32_t request,
	const uint8_t *bytes,
	size_t size)
{
	int error;

	/* The same call as opening. */
	error = jni_bytes_call(jni_secret_wrap, request, bytes, size);
	if (error != 0)
		return error;

	/* Succeeded: the request was sent. */
	return 0;
}

/* Asks Java to open a wrapped passphrase.  Returns 0, ENOMEM or EIO. */
int
mikuterm_java_secret_unwrap(
	uint32_t request,
	const uint8_t *bytes,
	size_t size)
{
	int error;

	/* The same call as wrapping. */
	error = jni_bytes_call(jni_secret_unwrap, request, bytes, size);
	if (error != 0)
		return error;

	/* Succeeded: the request was sent. */
	return 0;
}

/* Tells Java how many connections there are.  Returns 0 or EIO. */
int
mikuterm_java_service_update(
	unsigned connections)
{
	JNIEnv *env;
	jboolean raised;

	/* Only M may call. */
	env = keiland_android_env();
	if (env == NULL)
		return EIO;

	/* The call, which returns at once. */
	(*env)->CallStaticVoidMethod(env, jni_class, jni_service_update, (jint)connections);
	raised = (*env)->ExceptionCheck(env);
	if (raised) {
		(*env)->ExceptionClear(env);
		return EIO;
	}

	/* Succeeded: the service state was sent. */
	return 0;
}

/* Queues the answer to a secret: the bytes are copied. */
static void
jni_native_secret(
	JNIEnv *env,
	jclass cls,
	jint request,
	jbyteArray data)
{
	struct mikuterm_post_result *result;
	jsize length;
	int error;

	(void)cls;

	/* The result, with the bytes of a success. */
	result = calloc(1U, sizeof(*result));
	if (result == NULL)
		return;
	result->request = (uint32_t)request;
	if (data != NULL) {
		length = (*env)->GetArrayLength(env, data);
		result->bytes = malloc((size_t)length + 1U);
		if (result->bytes == NULL) {
			free(result);
			return;
		}

		(*env)->GetByteArrayRegion(env, data, 0, length, (jbyte *)result->bytes);
		result->size = (size_t)length;
		result->ok = 1;
	}

	/* Queued for M, which frees it. */
	error = keiland_post_app(MIKUTERM_POST_SECRET, result);
	if (error != 0) {
		if (result->bytes != NULL) {
			memset(result->bytes, 0, result->size);
			free(result->bytes);
		}

		free(result);
	}
}

/* Queues a change of the default network. */
static void
jni_native_network(
	JNIEnv *env,
	jclass cls,
	jboolean up,
	jlong handle)
{
	struct mikuterm_post_result *result;
	int error;

	(void)env;
	(void)cls;

	/* The result. */
	result = calloc(1U, sizeof(*result));
	if (result == NULL)
		return;
	result->ok = (up != 0);
	result->value = (int64_t)handle;

	/* Queued for M. */
	error = keiland_post_app(MIKUTERM_POST_NETWORK, result);
	if (error != 0)
		free(result);
}

/* Queues the user's ask to close every connection. */
static void
jni_native_stop_all(
	JNIEnv *env,
	jclass cls)
{
	(void)env;
	(void)cls;

	/* Nothing to carry. */
	(void)keiland_post_app(MIKUTERM_POST_STOP_ALL, NULL);
}

/* Calls a method of the class that takes a number and a byte array (the bytes are copied for Java). */
static int
jni_bytes_call(
	jmethodID method,
	uint32_t request,
	const uint8_t *bytes,
	size_t size)
{
	JNIEnv *env;
	jbyteArray array;
	jboolean raised;
	int error;

	/* Only M may call. */
	env = keiland_android_env();
	if (env == NULL)
		return EIO;

	/* The array. */
	array = (*env)->NewByteArray(env, (jsize)size);
	if (array == NULL) {
		(*env)->ExceptionClear(env);
		return ENOMEM;
	}

	(*env)->SetByteArrayRegion(env, array, 0, (jsize)size, (const jbyte *)bytes);

	/* The call, which returns at once; Java wipes what it was given when it is done. */
	error = 0;
	(*env)->CallStaticVoidMethod(env, jni_class, method, (jint)request, array);
	raised = (*env)->ExceptionCheck(env);
	if (raised) {
		(*env)->ExceptionClear(env);
		error = EIO;
	}

	(*env)->DeleteLocalRef(env, array);
	if (error != 0)
		return error;

	/* Succeeded: the request was sent. */
	return 0;
}

/* Asks Java to open the SAF upload document picker for view_number. */
int
mikuterm_java_sftp_pick_upload(
	uint32_t view_number)
{
	JNIEnv *env;
	jboolean raised;

	env = keiland_android_env();
	if (env == NULL)
		return EIO;

	(*env)->CallStaticVoidMethod(env, jni_class, jni_sftp_pick_upload, (jint)view_number);
	raised = (*env)->ExceptionCheck(env);
	if (raised) {
		(*env)->ExceptionClear(env);
		return EIO;
	}
	return 0;
}

/* Asks Java to open the SAF folder picker for changing the SFTP download folder. */
int
mikuterm_java_sftp_pick_download_dir(
	uint32_t view_number)
{
	JNIEnv *env;
	jboolean raised;

	env = keiland_android_env();
	if (env == NULL)
		return EIO;

	(*env)->CallStaticVoidMethod(env, jni_class, jni_sftp_pick_download_dir, (jint)view_number);
	raised = (*env)->ExceptionCheck(env);
	if (raised) {
		(*env)->ExceptionClear(env);
		return EIO;
	}
	return 0;
}

/* Attaches the current thread to the JVM if needed and returns its JNIEnv. */
int
mikuterm_jni_attach(
	JNIEnv **env_out,
	int *attached_out)
{
	JavaVM *vm;
	JNIEnv *env;
	jint status;

	*env_out = NULL;
	*attached_out = 0;
	vm = keiland_android_vm();
	if (vm == NULL)
		return EIO;

	status = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
	if (status == JNI_OK) {
		*env_out = env;
		return 0;
	}
	if (status != JNI_EDETACHED)
		return EIO;

	status = (*vm)->AttachCurrentThread(vm, &env, NULL);
	if (status != JNI_OK || env == NULL)
		return EIO;

	*env_out = env;
	*attached_out = 1;
	return 0;
}

/* Detaches the current thread from the JVM if mikuterm_jni_attach attached it. */
void
mikuterm_jni_detach(
	int attached)
{
	JavaVM *vm;

	if (!attached)
		return;
	vm = keiland_android_vm();
	if (vm != NULL)
		(*vm)->DetachCurrentThread(vm);
}

/* Returns the cached global reference to io/noctvm/mikuterm/SftpStorage. */
jclass
mikuterm_jni_sftp_storage_class(void)
{
	return jni_storage_class;
}

/* Returns a local reference to the application Context. */
jobject
mikuterm_jni_app_context(
	JNIEnv *env)
{
	jobject ctx;

	if (env == NULL || jni_class == NULL || jni_get_app_context == NULL)
		return NULL;
	ctx = (*env)->CallStaticObjectMethod(env, jni_class, jni_get_app_context);
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionClear(env);
		return NULL;
	}
	return ctx;
}

/* Queues selected SAF upload virtual paths for M. */
static void
jni_native_sftp_upload_picked(
	JNIEnv *env,
	jclass cls,
	jint view_number,
	jobjectArray paths)
{
	struct mikuterm_post_sftp_upload *post;
	jsize count;
	jsize i;
	jstring jstr;
	const char *utf;
	int error;

	(void)cls;
	if (paths == NULL)
		return;
	count = (*env)->GetArrayLength(env, paths);
	if (count <= 0)
		return;

	post = calloc(1U, sizeof(*post));
	if (post == NULL)
		return;
	post->paths = calloc((size_t)count, sizeof(char *));
	if (post->paths == NULL) {
		free(post);
		return;
	}
	post->view_number = (uint32_t)view_number;
	for (i = 0; i < count; i++) {
		jstr = (jstring)(*env)->GetObjectArrayElement(env, paths, i);
		if (jstr == NULL)
			continue;
		utf = (*env)->GetStringUTFChars(env, jstr, NULL);
		if (utf != NULL) {
			post->paths[post->count] = strdup(utf);
			(*env)->ReleaseStringUTFChars(env, jstr, utf);
			if (post->paths[post->count] != NULL)
				post->count++;
		}
		(*env)->DeleteLocalRef(env, jstr);
	}

	if (post->count == 0U) {
		free(post->paths);
		free(post);
		return;
	}

	error = keiland_post_app(MIKUTERM_POST_SFTP_UPLOAD, post);
	if (error != 0) {
		for (i = 0; (size_t)i < post->count; i++)
			free(post->paths[i]);
		free(post->paths);
		free(post);
	}
}

/* Queues the persisted SAF download tree URI for M. */
static void
jni_native_sftp_download_dir_picked(
	JNIEnv *env,
	jclass cls,
	jint view_number,
	jstring tree_uri,
	jstring virtual_dir)
{
	struct mikuterm_post_sftp_dir *post;
	const char *utf;
	int error;

	(void)cls;
	post = calloc(1U, sizeof(*post));
	if (post == NULL)
		return;
	post->view_number = (uint32_t)view_number;

	if (tree_uri != NULL) {
		utf = (*env)->GetStringUTFChars(env, tree_uri, NULL);
		if (utf != NULL) {
			post->tree_uri = strdup(utf);
			(*env)->ReleaseStringUTFChars(env, tree_uri, utf);
		}
		if (post->tree_uri == NULL) {
			free(post);
			return;
		}
	}
	if (virtual_dir != NULL) {
		utf = (*env)->GetStringUTFChars(env, virtual_dir, NULL);
		if (utf != NULL) {
			post->virtual_dir = strdup(utf);
			(*env)->ReleaseStringUTFChars(env, virtual_dir, utf);
		}
	}

	error = keiland_post_app(MIKUTERM_POST_SFTP_DOWNLOAD_DIR, post);
	if (error != 0) {
		free(post->tree_uri);
		free(post->virtual_dir);
		free(post);
	}
}
