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
#define JNI_CLASS	"io/noctvm/mikuterm/MikutermBridge"

/* The class as a global reference, and the methods M calls (found in JNI_OnLoad, as a native thread's own search does not see the program's classes). */
static jclass jni_class;
static jmethodID jni_secret_wrap;
static jmethodID jni_secret_unwrap;
static jmethodID jni_service_update;

static void jni_native_secret(JNIEnv *env, jclass cls, jint request, jbyteArray data);
static void jni_native_network(JNIEnv *env, jclass cls, jboolean up, jlong handle);
static void jni_native_stop_all(JNIEnv *env, jclass cls);
static int jni_bytes_call(jmethodID method, uint32_t request, const uint8_t *bytes, size_t size);

/* The native methods, with the types JNI knows them by. */
static const JNINativeMethod jni_natives[] = {
	{ "nativeSecret", "(I[B)V", (void *)jni_native_secret },
	{ "nativeNetwork", "(ZJ)V", (void *)jni_native_network },
	{ "nativeStopAll", "()V", (void *)jni_native_stop_all }
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

	/* The methods M calls. */
	jni_secret_wrap = (*env)->GetStaticMethodID(env, jni_class, "secretWrap", "(I[B)V");
	jni_secret_unwrap = (*env)->GetStaticMethodID(env, jni_class, "secretUnwrap", "(I[B)V");
	jni_service_update = (*env)->GetStaticMethodID(env, jni_class, "serviceUpdate", "(I)V");
	if (jni_secret_wrap == NULL || jni_secret_unwrap == NULL || jni_service_update == NULL)
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
