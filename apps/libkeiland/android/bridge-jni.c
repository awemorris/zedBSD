/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JNI side of the bridge between Java and the native thread: the native
 * methods of KeilandBridge, which queue what Java tells M, and the functions M
 * calls to ask Java for something.
 *
 * Strings cross as UTF-16 (GetStringChars, NewString) and are converted with
 * keiland_utf16_to_utf8 and keiland_utf8_to_utf16; the modified UTF-8 of JNI
 * is never used.  Every call into Java is followed by a check for an exception,
 * which is cleared.
 */

#include "apps/libkeiland/android/keiland-android.h"

#include "apps/libkeiland/android/bridge.h"
#include "apps/libkeiland/android/bridge-jni.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The name of the Java class that holds the methods, in the form FindClass takes. */
#define BRIDGE_JNI_CLASS	"io/noctvm/keiland/KeilandBridge"

/* The virtual machine, given to JNI_OnLoad. */
static JavaVM *bridge_jni_vm;

/*
 * The KeilandBridge class and String class, as global references, and the
 * methods M calls.  They are found in JNI_OnLoad because a native thread's own
 * search does not see the program's classes.
 */
static jclass bridge_jni_class;
static jclass bridge_jni_string_class;
static jmethodID bridge_jni_show_keyboard;
static jmethodID bridge_jni_set_input_mode;
static jmethodID bridge_jni_set_cursor_anchor;
static jmethodID bridge_jni_set_title;
static jmethodID bridge_jni_set_fullscreen;
static jmethodID bridge_jni_set_keyboard_suppressed;
static jmethodID bridge_jni_clipboard_set;
static jmethodID bridge_jni_popup_menu;
static jmethodID bridge_jni_open_url;
static jmethodID bridge_jni_quit;

static void bridge_jni_native_commit(JNIEnv *env, jclass cls, jstring text);
static void bridge_jni_native_compose(JNIEnv *env, jclass cls, jstring text, jint cursor);
static void bridge_jni_native_delete(JNIEnv *env, jclass cls, jint before, jint after);
static void bridge_jni_native_insets(JNIEnv *env, jclass cls, jint left, jint top, jint right, jint bottom, jint keyboard);
static void bridge_jni_native_config(JNIEnv *env, jclass cls, jboolean dark, jint accent, jfloat font_scale, jstring locale);
static void bridge_jni_native_focus(JNIEnv *env, jclass cls, jboolean focused);
static void bridge_jni_native_clipboard(JNIEnv *env, jclass cls, jstring text);
static void bridge_jni_native_menu(JNIEnv *env, jclass cls, jint request, jint item);
static void bridge_jni_native_key(JNIEnv *env, jclass cls, jint keycode, jint meta, jboolean down);
static void bridge_jni_native_debug_touch(JNIEnv *env, jclass cls, jint kind, jint a, jint b, jfloat ratio);
static int bridge_jni_push_string(JNIEnv *env, struct keiland_bridge_event *event, jstring text);
static int bridge_jni_method(JNIEnv *env, jmethodID *method, const char *name, const char *signature);
static jstring bridge_jni_new_string(JNIEnv *env, const char *text, size_t length);
static int bridge_jni_call_failed(JNIEnv *env);

/* The native methods of KeilandBridge, with the types JNI knows them by. */
static const JNINativeMethod bridge_jni_natives[] = {
	{ "nativeCommit", "(Ljava/lang/String;)V", (void *)bridge_jni_native_commit },
	{ "nativeCompose", "(Ljava/lang/String;I)V", (void *)bridge_jni_native_compose },
	{ "nativeDelete", "(II)V", (void *)bridge_jni_native_delete },
	{ "nativeInsets", "(IIIII)V", (void *)bridge_jni_native_insets },
	{ "nativeConfig", "(ZIFLjava/lang/String;)V", (void *)bridge_jni_native_config },
	{ "nativeFocus", "(Z)V", (void *)bridge_jni_native_focus },
	{ "nativeClipboard", "(Ljava/lang/String;)V", (void *)bridge_jni_native_clipboard },
	{ "nativeMenu", "(II)V", (void *)bridge_jni_native_menu },
	{ "nativeKey", "(IIZ)V", (void *)bridge_jni_native_key },
	{ "nativeDebugTouch", "(IIIF)V", (void *)bridge_jni_native_debug_touch }
};

/*
 * Called when the library is loaded: registers the native methods, finds the
 * methods M calls, and lets the program do the same for its own classes.
 */
JNIEXPORT jint
JNI_OnLoad(
	JavaVM *vm,
	void *reserved)
{
	JNIEnv *env;
	jclass local;
	jint status;
	int error;

	(void)reserved;

	/* Gets the environment of this thread. */
	bridge_jni_vm = vm;
	status = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
	if (status != JNI_OK)
		return JNI_ERR;

	/* Finds the class of the bridge, and keeps it. */
	local = (*env)->FindClass(env, BRIDGE_JNI_CLASS);
	if (local == NULL)
		return JNI_ERR;
	bridge_jni_class = (*env)->NewGlobalRef(env, local);
	(*env)->DeleteLocalRef(env, local);
	if (bridge_jni_class == NULL)
		return JNI_ERR;

	/* Finds the String class, which the menu's arrays are made of, and keeps it. */
	local = (*env)->FindClass(env, "java/lang/String");
	if (local == NULL)
		return JNI_ERR;
	bridge_jni_string_class = (*env)->NewGlobalRef(env, local);
	(*env)->DeleteLocalRef(env, local);
	if (bridge_jni_string_class == NULL)
		return JNI_ERR;

	/* Makes the native methods of the bridge reach the functions of this file. */
	status = (*env)->RegisterNatives(env, bridge_jni_class, bridge_jni_natives, sizeof(bridge_jni_natives) / sizeof(bridge_jni_natives[0]));
	if (status != 0)
		return JNI_ERR;

	/* Finds the methods that M calls. */
	error = bridge_jni_method(env, &bridge_jni_show_keyboard, "showKeyboard", "(Z)V");
	if (error != 0)
		return JNI_ERR;
	error = bridge_jni_method(env, &bridge_jni_set_input_mode, "setInputMode", "(I)V");
	if (error != 0)
		return JNI_ERR;
	error = bridge_jni_method(env, &bridge_jni_set_cursor_anchor, "setCursorAnchor", "(FFFF)V");
	if (error != 0)
		return JNI_ERR;
	error = bridge_jni_method(env, &bridge_jni_set_title, "setTitle", "(Ljava/lang/String;)V");
	if (error != 0)
		return JNI_ERR;
	error = bridge_jni_method(env, &bridge_jni_set_fullscreen, "setFullscreen", "(Z)V");
	if (error != 0)
		return JNI_ERR;
	error = bridge_jni_method(env, &bridge_jni_set_keyboard_suppressed, "setKeyboardSuppressed", "(Z)V");
	if (error != 0)
		return JNI_ERR;
	error = bridge_jni_method(env, &bridge_jni_clipboard_set, "clipboardSet", "(Ljava/lang/String;)V");
	if (error != 0)
		return JNI_ERR;
	error = bridge_jni_method(env, &bridge_jni_popup_menu, "popupMenu", "(III[Ljava/lang/String;[I[I)V");
	if (error != 0)
		return JNI_ERR;
	error = bridge_jni_method(env, &bridge_jni_open_url, "openUrl", "(Ljava/lang/String;)V");
	if (error != 0)
		return JNI_ERR;
	error = bridge_jni_method(env, &bridge_jni_quit, "quit", "()V");
	if (error != 0)
		return JNI_ERR;

	/* Lets the program prepare its own classes. */
	status = keiland_android_app_jni_load(env);
	if (status != JNI_OK)
		return JNI_ERR;

	/* Succeeded: the version of JNI this library needs. */
	return JNI_VERSION_1_6;
}

/*
 * Returns the virtual machine.
 */
JavaVM *
keiland_android_vm(void)
{
	/* The machine JNI_OnLoad was given. */
	return bridge_jni_vm;
}

/*
 * Shows or hides the soft keyboard.
 */
int
keiland_java_show_keyboard(
	int show)
{
	JNIEnv *env;
	int error;

	/* Needs M's environment. */
	env = keiland_android_env();
	if (env == NULL)
		return EIO;

	/* Asks Java, which does it on the main thread. */
	(*env)->CallStaticVoidMethod(env, bridge_jni_class, bridge_jni_show_keyboard, (jboolean)(show != 0));
	error = bridge_jni_call_failed(env);
	if (error != 0)
		return error;

	/* Succeeded: the request was sent. */
	return 0;
}

/*
 * Chooses the kind of input.
 */
int
keiland_java_set_input_mode(
	int mode)
{
	JNIEnv *env;
	int error;

	/* Needs M's environment. */
	env = keiland_android_env();
	if (env == NULL)
		return EIO;

	/* Asks Java, which does it on the main thread. */
	(*env)->CallStaticVoidMethod(env, bridge_jni_class, bridge_jni_set_input_mode, (jint)mode);
	error = bridge_jni_call_failed(env);
	if (error != 0)
		return error;

	/* Succeeded: the request was sent. */
	return 0;
}

/*
 * Tells the input method where the composing text is.
 */
int
keiland_java_set_cursor_anchor(
	float x,
	float y,
	float width,
	float height)
{
	JNIEnv *env;
	int error;

	/* Needs M's environment. */
	env = keiland_android_env();
	if (env == NULL)
		return EIO;

	/* Asks Java, which does it on the main thread. */
	(*env)->CallStaticVoidMethod(env, bridge_jni_class, bridge_jni_set_cursor_anchor, (jfloat)x, (jfloat)y, (jfloat)width, (jfloat)height);
	error = bridge_jni_call_failed(env);
	if (error != 0)
		return error;

	/* Succeeded: the request was sent. */
	return 0;
}

/*
 * Sets the task's title.
 */
int
keiland_java_set_title(
	const char *title)
{
	JNIEnv *env;
	jstring string;
	int error;

	/* Needs M's environment. */
	env = keiland_android_env();
	if (env == NULL)
		return EIO;

	/* Makes the Java string of the title. */
	string = bridge_jni_new_string(env, title, title == NULL ? 0 : strlen(title));
	if (string == NULL)
		return ENOMEM;

	/* Asks Java, which does it on the main thread. */
	(*env)->CallStaticVoidMethod(env, bridge_jni_class, bridge_jni_set_title, string);
	error = bridge_jni_call_failed(env);
	(*env)->DeleteLocalRef(env, string);

	/* Reports whether Java raised an exception. */
	if (error != 0)
		return error;

	/* Succeeded: the title is on its way. */
	return 0;
}

/*
 * Enables or disables immersive fullscreen mode.
 */
int
keiland_java_set_fullscreen(
	int fullscreen)
{
	JNIEnv *env;
	int error;

	/* Needs M's environment. */
	env = keiland_android_env();
	if (env == NULL)
		return EIO;

	/* Asks Java, which does it on the main thread. */
	(*env)->CallStaticVoidMethod(env, bridge_jni_class, bridge_jni_set_fullscreen, (jboolean)(fullscreen != 0));
	error = bridge_jni_call_failed(env);
	if (error != 0)
		return error;

	/* Succeeded: the request was sent. */
	return 0;
}

/*
 * Suppresses or restores soft keyboard activation.
 */
int
keiland_java_set_keyboard_suppressed(
	int suppressed)
{
	JNIEnv *env;
	int error;

	/* Needs M's environment. */
	env = keiland_android_env();
	if (env == NULL)
		return EIO;

	/* Asks Java, which does it on the main thread. */
	(*env)->CallStaticVoidMethod(env, bridge_jni_class, bridge_jni_set_keyboard_suppressed, (jboolean)(suppressed != 0));
	error = bridge_jni_call_failed(env);
	if (error != 0)
		return error;

	/* Succeeded: the request was sent. */
	return 0;
}

/*
 * Puts text on the clipboard, or empties it.
 */
int
keiland_java_clipboard_set(
	const char *text,
	size_t length)
{
	JNIEnv *env;
	jstring string;
	int error;

	/* Needs M's environment. */
	env = keiland_android_env();
	if (env == NULL)
		return EIO;

	/* Makes the Java string, which is null when there is nothing to put. */
	string = NULL;
	if (text != NULL && length > 0) {
		string = bridge_jni_new_string(env, text, length);
		if (string == NULL)
			return ENOMEM;
	}

	/* Asks Java, which does it on the main thread. */
	(*env)->CallStaticVoidMethod(env, bridge_jni_class, bridge_jni_clipboard_set, string);
	error = bridge_jni_call_failed(env);
	if (string != NULL)
		(*env)->DeleteLocalRef(env, string);

	/* Reports whether Java raised an exception. */
	if (error != 0)
		return error;

	/* Succeeded: the clipboard is on its way. */
	return 0;
}

/*
 * Shows a menu at a place.
 */
int
keiland_java_popup_menu(
	uint32_t request,
	int x,
	int y,
	const char *const *labels,
	const int32_t *parents,
	const int32_t *flags,
	size_t count)
{
	JNIEnv *env;
	jobjectArray label_array;
	jintArray parent_array;
	jintArray flag_array;
	jstring string;
	size_t i;
	int error;

	/* Needs M's environment. */
	env = keiland_android_env();
	if (env == NULL)
		return EIO;

	/* Makes the three arrays, one item to each place. */
	label_array = (*env)->NewObjectArray(env, (jsize)count, bridge_jni_string_class, NULL);
	parent_array = (*env)->NewIntArray(env, (jsize)count);
	flag_array = (*env)->NewIntArray(env, (jsize)count);
	if (label_array == NULL || parent_array == NULL || flag_array == NULL) {
		bridge_jni_call_failed(env);

		return ENOMEM;
	}

	/* Fills the labels. */
	error = 0;
	for (i = 0;
	     i < count;
	     i++) {
		string = bridge_jni_new_string(env, labels[i], strlen(labels[i]));
		if (string == NULL) {
			error = ENOMEM;
			break;
		}

		(*env)->SetObjectArrayElement(env, label_array, (jsize)i, string);
		(*env)->DeleteLocalRef(env, string);
	}

	/* Fills the parents and the flags, and asks Java to show the menu. */
	if (error == 0) {
		(*env)->SetIntArrayRegion(env, parent_array, 0, (jsize)count, (const jint *)parents);
		(*env)->SetIntArrayRegion(env, flag_array, 0, (jsize)count, (const jint *)flags);
		(*env)->CallStaticVoidMethod(env, bridge_jni_class, bridge_jni_popup_menu, (jint)request, (jint)x, (jint)y, label_array, parent_array, flag_array);
		error = bridge_jni_call_failed(env);
	}

	/* Lets go of the arrays. */
	(*env)->DeleteLocalRef(env, label_array);
	(*env)->DeleteLocalRef(env, parent_array);
	(*env)->DeleteLocalRef(env, flag_array);

	/* Reports why the menu could not be asked for. */
	if (error != 0)
		return error;

	/* Succeeded: the menu is on its way. */
	return 0;
}

/*
 * Opens a web address in the browser.
 */
int
keiland_java_open_url(
	const char *url)
{
	JNIEnv *env;
	jstring string;
	int error;

	/* Needs M's environment. */
	env = keiland_android_env();
	if (env == NULL)
		return EIO;

	/* Makes the Java string of the address. */
	string = bridge_jni_new_string(env, url, url == NULL ? 0 : strlen(url));
	if (string == NULL)
		return ENOMEM;

	/* Asks Java, which does it on the main thread. */
	(*env)->CallStaticVoidMethod(env, bridge_jni_class, bridge_jni_open_url, string);
	error = bridge_jni_call_failed(env);
	(*env)->DeleteLocalRef(env, string);

	/* Reports whether Java raised an exception. */
	if (error != 0)
		return error;

	/* Succeeded: the address is on its way. */
	return 0;
}

/*
 * Closes the Activity and removes its task.
 */
int
keiland_java_quit(void)
{
	JNIEnv *env;
	int error;

	/* Needs M's environment. */
	env = keiland_android_env();
	if (env == NULL)
		return EIO;

	/* Asks Java, which does it on the main thread. */
	(*env)->CallStaticVoidMethod(env, bridge_jni_class, bridge_jni_quit);
	error = bridge_jni_call_failed(env);
	if (error != 0)
		return error;

	/* Succeeded: the request was sent. */
	return 0;
}

/*
 * Closes the current Activity and removes its task from the recent apps list.
 */
void
keiland_android_quit(void)
{
	/* Asks Java to finish the Activity. */
	(void)keiland_java_quit();
}

/*
 * Queues the text that the input method committed.
 */
static void
bridge_jni_native_commit(
	JNIEnv *env,
	jclass cls,
	jstring text)
{
	struct keiland_bridge_event event;
	int error;

	(void)cls;

	/* Queues the event with its text. */
	memset(&event, 0, sizeof(event));
	event.kind = KEILAND_BRIDGE_COMMIT;
	error = bridge_jni_push_string(env, &event, text);
	if (error != 0) {
		/* The event was dropped and counted. */
	}
}

/*
 * Queues the text being composed and the cursor in it.
 */
static void
bridge_jni_native_compose(
	JNIEnv *env,
	jclass cls,
	jstring text,
	jint cursor)
{
	struct keiland_bridge_event event;
	int error;

	(void)cls;

	/* Queues the event with its text. */
	memset(&event, 0, sizeof(event));
	event.kind = KEILAND_BRIDGE_COMPOSE;
	event.a = cursor;
	error = bridge_jni_push_string(env, &event, text);
	if (error != 0) {
		/* The event was dropped and counted. */
	}
}

/*
 * Queues a request to delete characters around the cursor.
 */
static void
bridge_jni_native_delete(
	JNIEnv *env,
	jclass cls,
	jint before,
	jint after)
{
	struct keiland_bridge_event event;
	int error;

	(void)env;
	(void)cls;

	/* Queues the event. */
	memset(&event, 0, sizeof(event));
	event.kind = KEILAND_BRIDGE_DELETE;
	event.a = before;
	event.b = after;
	error = keiland_bridge_push(&event, NULL, 0);
	if (error != 0) {
		/* The event was dropped and counted. */
	}
}

/*
 * Queues the window's insets and the keyboard's height.
 */
static void
bridge_jni_native_insets(
	JNIEnv *env,
	jclass cls,
	jint left,
	jint top,
	jint right,
	jint bottom,
	jint keyboard)
{
	struct keiland_bridge_event event;
	int error;

	(void)env;
	(void)cls;

	/* Queues the event. */
	memset(&event, 0, sizeof(event));
	event.kind = KEILAND_BRIDGE_INSETS;
	event.a = left;
	event.b = top;
	event.c = right;
	event.d = bottom;
	event.e = keyboard;
	error = keiland_bridge_push(&event, NULL, 0);
	if (error != 0) {
		/* The event was dropped and counted. */
	}
}

/*
 * Queues the look of the system.
 */
static void
bridge_jni_native_config(
	JNIEnv *env,
	jclass cls,
	jboolean dark,
	jint accent,
	jfloat font_scale,
	jstring locale)
{
	struct keiland_bridge_event event;
	int error;

	(void)cls;

	/* Queues the event with the locale as its text. */
	memset(&event, 0, sizeof(event));
	event.kind = KEILAND_BRIDGE_CONFIG;
	event.a = dark ? 1 : 0;
	event.b = accent;
	event.f = font_scale;
	error = bridge_jni_push_string(env, &event, locale);
	if (error != 0) {
		/* The event was dropped and counted. */
	}
}

/*
 * Queues a change of the window's input focus.
 */
static void
bridge_jni_native_focus(
	JNIEnv *env,
	jclass cls,
	jboolean focused)
{
	struct keiland_bridge_event event;
	int error;

	(void)env;
	(void)cls;

	/* Queues the event. */
	memset(&event, 0, sizeof(event));
	event.kind = KEILAND_BRIDGE_FOCUS;
	event.a = focused ? 1 : 0;
	error = keiland_bridge_push(&event, NULL, 0);
	if (error != 0) {
		/* The event was dropped and counted. */
	}
}

/*
 * Queues the clipboard's text.
 */
static void
bridge_jni_native_clipboard(
	JNIEnv *env,
	jclass cls,
	jstring text)
{
	struct keiland_bridge_event event;
	int error;

	(void)cls;

	/* Queues the event; a null text stays NULL, which means the clipboard is empty. */
	memset(&event, 0, sizeof(event));
	event.kind = KEILAND_BRIDGE_CLIPBOARD;
	error = bridge_jni_push_string(env, &event, text);
	if (error != 0) {
		/* The event was dropped and counted. */
	}
}

/*
 * Queues the answer to a menu.
 */
static void
bridge_jni_native_menu(
	JNIEnv *env,
	jclass cls,
	jint request,
	jint item)
{
	struct keiland_bridge_event event;
	int error;

	(void)env;
	(void)cls;

	/* Queues the event. */
	memset(&event, 0, sizeof(event));
	event.kind = KEILAND_BRIDGE_MENU;
	event.a = request;
	event.b = item;
	error = keiland_bridge_push(&event, NULL, 0);
	if (error != 0) {
		/* The event was dropped and counted. */
	}
}

/*
 * Converts a Java string to UTF-8 and queues the event with it.  A null string
 * queues the event without text.  Returns what keiland_bridge_push returns, or
 * ENOMEM.
 */
static int
bridge_jni_push_string(
	JNIEnv *env,
	struct keiland_bridge_event *event,
	jstring text)
{
	const jchar *units;
	jsize count;
	size_t needed;
	char *utf8;
	int error;

	/* A null string is an event without text. */
	if (text == NULL) {
		error = keiland_bridge_push(event, NULL, 0);
		if (error != 0)
			return error;

		/* Succeeded: queued without text. */
		return 0;
	}

	/* Reads the string's UTF-16 units. */
	count = (*env)->GetStringLength(env, text);
	units = (*env)->GetStringChars(env, text, NULL);
	if (units == NULL)
		return ENOMEM;

	/* Converts them into a buffer of the exact size. */
	needed = keiland_utf16_to_utf8((const uint16_t *)units, (size_t)count, NULL, 0);
	utf8 = malloc(needed + 1);
	if (utf8 == NULL) {
		(*env)->ReleaseStringChars(env, text, units);

		return ENOMEM;
	}

	keiland_utf16_to_utf8((const uint16_t *)units, (size_t)count, utf8, needed + 1);
	(*env)->ReleaseStringChars(env, text, units);

	/* Queues the event, which keeps its own copy of the text. */
	error = keiland_bridge_push(event, utf8, needed);
	free(utf8);

	/* Reports why the event was not queued. */
	if (error != 0)
		return error;

	/* Succeeded: queued with its text. */
	return 0;
}

/*
 * Finds a static method of the bridge and keeps its id.  Returns 0, or EIO
 * when there is no such method (the exception is cleared).
 */
static int
bridge_jni_method(
	JNIEnv *env,
	jmethodID *method,
	const char *name,
	const char *signature)
{
	/* Looks the method up by its name and its type. */
	*method = (*env)->GetStaticMethodID(env, bridge_jni_class, name, signature);
	if (*method == NULL) {
		(*env)->ExceptionClear(env);

		return EIO;
	}

	/* Succeeded: the method's id is kept. */
	return 0;
}

/*
 * Makes a Java string from UTF-8, or returns NULL when it cannot be made (the
 * exception is cleared).
 */
static jstring
bridge_jni_new_string(
	JNIEnv *env,
	const char *text,
	size_t length)
{
	jstring string;
	uint16_t *units;
	size_t count;

	/* The number of units the text needs, and a buffer for them. */
	count = keiland_utf8_to_utf16(text, length, NULL, 0);
	units = malloc((count + 1) * sizeof(*units));
	if (units == NULL)
		return NULL;

	/* Converts, and makes the string from the units. */
	keiland_utf8_to_utf16(text, length, units, count);
	string = (*env)->NewString(env, (const jchar *)units, (jsize)count);
	free(units);
	if (string == NULL) {
		bridge_jni_call_failed(env);

		return NULL;
	}

	/* Succeeded: the string is a local reference. */
	return string;
}

/*
 * Checks for an exception after a call into Java, and clears it.  Returns 0 when
 * there was none, or EIO.
 */
static int
bridge_jni_call_failed(
	JNIEnv *env)
{
	jboolean raised;

	/* Asks whether Java raised an exception. */
	raised = (*env)->ExceptionCheck(env);
	if (raised) {
		(*env)->ExceptionClear(env);

		return EIO;
	}

	/* Succeeded: no exception. */
	return 0;
}

/*
 * Queues a key the key bar sent: its Android key code, the meta state and
 * whether it is pressed.
 */
static void
bridge_jni_native_key(
	JNIEnv *env,
	jclass cls,
	jint keycode,
	jint meta,
	jboolean down)
{
	struct keiland_bridge_event event;
	int error;

	(void)env;
	(void)cls;

	/* Queues the event. */
	memset(&event, 0, sizeof(event));
	event.kind = KEILAND_BRIDGE_KEY;
	event.a = keycode;
	event.b = meta;
	event.c = down ? 1 : 0;
	error = keiland_bridge_push(&event, NULL, 0);
	if (error != 0) {
		/* The event was dropped and counted. */
	}
}

/*
 * Queues a debug-only synthetic two-finger pan or pinch gesture.
 */
static void
bridge_jni_native_debug_touch(
	JNIEnv *env,
	jclass cls,
	jint kind,
	jint a,
	jint b,
	jfloat ratio)
{
	struct keiland_bridge_event event;
	int error;

	(void)env;
	(void)cls;

	/* Queues the synthetic touch event. */
	memset(&event, 0, sizeof(event));
	event.kind = KEILAND_BRIDGE_DEBUG_TOUCH;
	event.a = kind;
	event.b = a;
	event.c = b;
	event.f = ratio;
	error = keiland_bridge_push(&event, NULL, 0);
	if (error != 0) {
		/* The event was dropped and counted. */
	}
}
