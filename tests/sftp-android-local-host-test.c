/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit test for Android SFTP local filesystem layer (sftp-local-android.c):
 * virtual path parsing/joining, MediaStore/SAF JNI dispatch, POSIX fallback I/O,
 * and MIKUTERM_PATH_MAX bounds.
 */

#include "apps/mikuterm-common/sftp-local.h"
#include "apps/terminal-android/jni/mikuterm-android.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks;
static int failures;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
	} \
} while (0)

/* ---- Minimal JNI mock table ---- */

static int g_register_tree_ok = 1;
static int g_stat_mediastore_found = 1;
static int g_mkdir_saf_calls;
static char g_last_method[128];

static jclass g_fake_cls = (jclass)(uintptr_t)0x1001;
static jobject g_fake_ctx = (jobject)(uintptr_t)0x1002;
static jmethodID g_mid_reg_tree = (jmethodID)(uintptr_t)0x2001;
static jmethodID g_mid_stat_doc = (jmethodID)(uintptr_t)0x2002;
static jmethodID g_mid_stat_ms = (jmethodID)(uintptr_t)0x2003;
static jmethodID g_mid_stat_tree = (jmethodID)(uintptr_t)0x2004;
static jmethodID g_mid_mkdir_tree = (jmethodID)(uintptr_t)0x2005;

static jboolean mock_exc_check(JNIEnv *e) { (void)e; return JNI_FALSE; }
static void mock_exc_clear(JNIEnv *e) { (void)e; }
static void *g_mock_ptrs[64];
static size_t g_mock_ptr_count;

static void
mock_del_local(JNIEnv *e, jobject o)
{
	size_t i;

	(void)e;
	for (i = 0U; i < g_mock_ptr_count; i++) {
		if (g_mock_ptrs[i] == (void *)(uintptr_t)o) {
			free(g_mock_ptrs[i]);
			g_mock_ptrs[i] = NULL;
		}
	}
}

static jmethodID
mock_get_static_mid(JNIEnv *e, jclass c, const char *name, const char *sig)
{
	(void)e;
	(void)c;
	(void)sig;
	(void)snprintf(g_last_method, sizeof(g_last_method), "%s", name);
	if (strcmp(name, "registerTreeUri") == 0)
		return g_mid_reg_tree;
	if (strcmp(name, "statSafDoc") == 0)
		return g_mid_stat_doc;
	if (strcmp(name, "statMediaStoreDownload") == 0)
		return g_mid_stat_ms;
	if (strcmp(name, "statSafTree") == 0)
		return g_mid_stat_tree;
	if (strcmp(name, "mkdirSafTree") == 0)
		return g_mid_mkdir_tree;
	return NULL;
}

static jstring
mock_new_str(JNIEnv *e, const char *s)
{
	char *dup;

	(void)e;
	dup = strdup(s != NULL ? s : "");
	if (dup != NULL && g_mock_ptr_count < 64U)
		g_mock_ptrs[g_mock_ptr_count++] = dup;
	return (jstring)(uintptr_t)dup;
}

static const char *
mock_get_str(JNIEnv *e, jstring s, jboolean *iscopy)
{
	(void)e;
	if (iscopy != NULL)
		*iscopy = JNI_FALSE;
	return (const char *)(uintptr_t)s;
}

static void
mock_rel_str(JNIEnv *e, jstring s, const char *utf)
{
	(void)e;
	(void)s;
	(void)utf;
}

static jobject
mock_call_obj(JNIEnv *e, jclass c, jmethodID m, ...)
{
	(void)e;
	(void)c;
	if (m == g_mid_reg_tree) {
		if (!g_register_tree_ok)
			return NULL;
		return (jobject)(uintptr_t)mock_new_str(e, "saf-tree://7");
	}
	if (m == g_mid_stat_doc)
		return (jobject)(uintptr_t)0x3001;
	if (m == g_mid_stat_ms)
		return g_stat_mediastore_found ? (jobject)(uintptr_t)0x3002 : NULL;
	if (m == g_mid_stat_tree)
		return (jobject)(uintptr_t)0x3003;
	return NULL;
}

static jboolean
mock_call_bool(JNIEnv *e, jclass c, jmethodID m, ...)
{
	(void)e;
	(void)c;
	if (m == g_mid_mkdir_tree) {
		g_mkdir_saf_calls++;
		return JNI_TRUE;
	}
	return JNI_FALSE;
}

static jsize
mock_get_arr_len(JNIEnv *e, jarray a)
{
	(void)e;
	(void)a;
	return 3;
}

static void
mock_get_long_reg(JNIEnv *e, jlongArray a, jsize start, jsize len, jlong *buf)
{
	(void)e;
	(void)start;
	(void)len;
	if (a == (jlongArray)(uintptr_t)0x3001) {
		buf[0] = 0;
		buf[1] = 4096;
		buf[2] = 1700000000;
	} else if (a == (jlongArray)(uintptr_t)0x3002) {
		buf[0] = 0;
		buf[1] = 1234;
		buf[2] = 1700000100;
	} else if (a == (jlongArray)(uintptr_t)0x3003) {
		buf[0] = 1;
		buf[1] = 0;
		buf[2] = 1700000200;
	}
}

static struct JNINativeInterface_ g_jni_vtbl = {
	.ExceptionCheck = mock_exc_check,
	.ExceptionClear = mock_exc_clear,
	.DeleteLocalRef = mock_del_local,
	.GetStaticMethodID = mock_get_static_mid,
	.NewStringUTF = mock_new_str,
	.GetStringUTFChars = mock_get_str,
	.ReleaseStringUTFChars = mock_rel_str,
	.CallStaticObjectMethod = mock_call_obj,
	.CallStaticBooleanMethod = mock_call_bool,
	.GetArrayLength = mock_get_arr_len,
	.GetLongArrayRegion = mock_get_long_reg
};

static JNIEnv g_mock_env = &g_jni_vtbl;

int mikuterm_jni_attach(JNIEnv **env_out, int *attached_out) { *env_out = &g_mock_env; *attached_out = 0; return 0; }
void mikuterm_jni_detach(int attached) { (void)attached; }
jclass mikuterm_jni_sftp_storage_class(void) { return g_fake_cls; }
jobject mikuterm_jni_app_context(JNIEnv *env) { (void)env; return g_fake_ctx; }

int
main(void)
{
	char path[MIKUTERM_PATH_MAX];
	char small[16];
	char tmpdir[] = "/tmp/mikuterm-local-XXXXXX";
	char temp_path[MIKUTERM_PATH_MAX];
	char final_path[MIKUTERM_PATH_MAX];
	struct sftp_local_entry st;
	struct sftp_local_file *f;
	uint64_t rsize;
	uint32_t rmtime;
	char buf[32];
	size_t got;

	/* 1. Default downloads dir without persisted SAF URI -> mediastore://downloads. */
	sftp_local_android_set_download_uri(NULL);
	CHECK(sftp_local_downloads_dir(path, sizeof(path)) == 0);
	CHECK(strcmp(path, "mediastore://downloads") == 0);

	/* 2. Persisted SAF URI -> saf-tree://7, fallback when revoked. */
	sftp_local_android_set_download_uri("content://com.android.externalstorage.documents/tree/primary%3ADownload");
	g_register_tree_ok = 1;
	CHECK(sftp_local_downloads_dir(path, sizeof(path)) == 0);
	CHECK(strcmp(path, "saf-tree://7") == 0);
	g_register_tree_ok = 0;
	CHECK(sftp_local_downloads_dir(path, sizeof(path)) == 0);
	CHECK(strcmp(path, "mediastore://downloads") == 0);

	/* 3. sftp_local_join_path and bounds. */
	CHECK(sftp_local_join_path(path, sizeof(path), "mediastore://downloads", "a.txt") == 0);
	CHECK(strcmp(path, "mediastore://downloads/a.txt") == 0);
	CHECK(sftp_local_join_path(path, sizeof(path), "saf-tree://7/", "sub") == 0);
	CHECK(strcmp(path, "saf-tree://7/sub") == 0);
	CHECK(sftp_local_join_path(small, sizeof(small), "mediastore://downloads", "toolong.txt") == ENAMETOOLONG);
	CHECK(sftp_local_join_path(NULL, sizeof(path), "a", "b") == EINVAL);

	/* 4. sftp_local_stat across virtual schemes. */
	CHECK(sftp_local_stat("mediastore://downloads", &st) == 0);
	CHECK(st.is_dir == 1);
	g_stat_mediastore_found = 1;
	CHECK(sftp_local_stat("mediastore://downloads/sub/file.bin", &st) == 0);
	CHECK(st.is_dir == 0 && st.size == 1234U && strcmp(st.name, "file.bin") == 0);
	g_stat_mediastore_found = 0;
	CHECK(sftp_local_stat("mediastore://downloads/missing.bin", &st) == ENOENT);

	CHECK(sftp_local_stat("saf-doc://12/report.pdf", &st) == 0);
	CHECK(st.is_dir == 0 && st.size == 4096U && strcmp(st.name, "report.pdf") == 0);

	CHECK(sftp_local_stat("saf-tree://1/sub", &st) == 0);
	CHECK(st.is_dir == 1);

	/* 5. sftp_local_mkdir virtual schemes. */
	CHECK(sftp_local_mkdir("mediastore://downloads/sub") == 0);
	g_mkdir_saf_calls = 0;
	CHECK(sftp_local_mkdir("saf-tree://1/sub") == 0);
	CHECK(g_mkdir_saf_calls == 1);

	/* 6. POSIX fallback temp write -> commit -> read -> remove roundtrip. */
	CHECK(mkdtemp(tmpdir) != NULL);
	CHECK(sftp_local_join_path(final_path, sizeof(final_path), tmpdir, "hello.txt") == 0);
	CHECK(sftp_local_open_temp_write(tmpdir, "hello.txt", temp_path, sizeof(temp_path), &f) == 0);
	CHECK(sftp_local_write_at(f, 0ULL, "hello", 5U) == 0);
	CHECK(sftp_local_set_mtime(f, 1700000000U) == 0);
	sftp_local_close(f);
	CHECK(sftp_local_commit_temp(temp_path, final_path) == 0);
	CHECK(sftp_local_open_read(final_path, &f, &rsize, &rmtime) == 0);
	CHECK(rsize == 5U);
	memset(buf, 0, sizeof(buf));
	CHECK(sftp_local_read_at(f, 0ULL, buf, sizeof(buf), &got) == 0);
	CHECK(got == 5U && memcmp(buf, "hello", 5U) == 0);
	sftp_local_close(f);
	CHECK(sftp_local_remove(final_path) == 0);
	CHECK(rmdir(tmpdir) == 0);

	printf("checks %d, failures %d\n", checks, failures);
	if (failures == 0) {
		printf("sftp-android-local-host-test: PASS\n");
		return 0;
	}
	printf("sftp-android-local-host-test: FAIL\n");
	return 1;
}
