/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Local filesystem layer for SFTP transfers on Android (API 29+ Scoped Storage).
 *
 * Virtual paths:
 *   mediastore://downloads[/rel]       MediaStore.Downloads (default download dir)
 *   saf-tree://<tree_id>[/rel]         SAF ACTION_OPEN_DOCUMENT_TREE directory
 *   saf-doc://<doc_id>/<display_name>  SAF ACTION_OPEN_DOCUMENT upload file
 *   /...                               Regular POSIX path fallback
 *
 * File chunk I/O uses pread()/pwrite() directly on detached file descriptors
 * from ParcelFileDescriptor.detachFd() to avoid per-chunk JNI crossings.
 */

#include "apps/mikuterm-common/sftp-local.h"
#include "apps/terminal-android/jni/mikuterm-android.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PREFIX_MEDIASTORE	"mediastore://downloads"
#define PREFIX_SAF_TREE		"saf-tree://"
#define PREFIX_SAF_DOC		"saf-doc://"

struct sftp_local_file {
	int fd;
};

static pthread_mutex_t local_lock = PTHREAD_MUTEX_INITIALIZER;
static char local_download_uri[MIKUTERM_PATH_MAX];

/* Splits "saf-tree://<id>[/rel]" into tree_id and rel_path (empty if root). */
static int
parse_saf_tree(
	const char *path,
	char *tree_id,
	size_t tree_id_cap,
	const char **rel_out)
{
	const char *p;
	const char *slash;
	size_t len;

	if (strncmp(path, PREFIX_SAF_TREE, strlen(PREFIX_SAF_TREE)) != 0)
		return 0;
	p = path + strlen(PREFIX_SAF_TREE);
	slash = strchr(p, '/');
	if (slash != NULL) {
		len = (size_t)(slash - p);
		*rel_out = slash + 1;
	} else {
		len = strlen(p);
		*rel_out = "";
	}
	if (len == 0U || len >= tree_id_cap)
		return 0;
	memcpy(tree_id, p, len);
	tree_id[len] = '\0';
	return 1;
}

/* Splits "saf-doc://<id>/<name>" into doc_id. */
static int
parse_saf_doc(
	const char *path,
	char *doc_id,
	size_t doc_id_cap)
{
	const char *p;
	const char *slash;
	size_t len;

	if (strncmp(path, PREFIX_SAF_DOC, strlen(PREFIX_SAF_DOC)) != 0)
		return 0;
	p = path + strlen(PREFIX_SAF_DOC);
	slash = strchr(p, '/');
	len = (slash != NULL) ? (size_t)(slash - p) : strlen(p);
	if (len == 0U || len >= doc_id_cap)
		return 0;
	memcpy(doc_id, p, len);
	doc_id[len] = '\0';
	return 1;
}

/* Returns the relative path inside "mediastore://downloads[/rel]". */
static int
parse_mediastore(
	const char *path,
	const char **rel_out)
{
	size_t plen;

	plen = strlen(PREFIX_MEDIASTORE);
	if (strncmp(path, PREFIX_MEDIASTORE, plen) != 0)
		return 0;
	if (path[plen] == '\0') {
		*rel_out = "";
		return 1;
	}
	if (path[plen] == '/') {
		*rel_out = path + plen + 1;
		return 1;
	}
	return 0;
}

/* Checks whether JNI exception occurred, clears it, and returns 1 if raised. */
static int
jni_clear_exc(
	JNIEnv *env)
{
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionClear(env);
		return 1;
	}
	return 0;
}

/* Sets or clears the persisted SAF download tree URI. */
void
sftp_local_android_set_download_uri(
	const char *uri)
{
	pthread_mutex_lock(&local_lock);
	if (uri != NULL && uri[0] != '\0')
		(void)snprintf(local_download_uri, sizeof(local_download_uri), "%s", uri);
	else
		local_download_uri[0] = '\0';
	pthread_mutex_unlock(&local_lock);
}

/*
 * Returns the active download destination root: persisted SAF tree if valid,
 * otherwise "mediastore://downloads".
 */
int
sftp_local_downloads_dir(
	char *path,
	size_t size)
{
	char uri_copy[MIKUTERM_PATH_MAX];
	JNIEnv *env;
	jclass cls;
	jobject ctx;
	jmethodID mid;
	jstring juri;
	jstring jvirt;
	const char *utf;
	int attached;
	int ok;

	if (path == NULL || size == 0U)
		return EINVAL;

	pthread_mutex_lock(&local_lock);
	(void)snprintf(uri_copy, sizeof(uri_copy), "%s", local_download_uri);
	pthread_mutex_unlock(&local_lock);

	if (uri_copy[0] != '\0' && mikuterm_jni_attach(&env, &attached) == 0) {
		cls = mikuterm_jni_sftp_storage_class();
		ctx = mikuterm_jni_app_context(env);
		ok = 0;
		if (cls != NULL && ctx != NULL) {
			mid = (*env)->GetStaticMethodID(
				env, cls, "registerTreeUri",
				"(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;");
			if (!jni_clear_exc(env) && mid != NULL) {
				juri = (*env)->NewStringUTF(env, uri_copy);
				if (juri != NULL) {
					jvirt = (jstring)(*env)->CallStaticObjectMethod(env, cls, mid, ctx, juri);
					if (!jni_clear_exc(env) && jvirt != NULL) {
						utf = (*env)->GetStringUTFChars(env, jvirt, NULL);
						if (utf != NULL) {
							if (snprintf(path, size, "%s", utf) < (int)size)
								ok = 1;
							(*env)->ReleaseStringUTFChars(env, jvirt, utf);
						}
						(*env)->DeleteLocalRef(env, jvirt);
					}
					(*env)->DeleteLocalRef(env, juri);
				}
			}
		}
		if (ctx != NULL)
			(*env)->DeleteLocalRef(env, ctx);
		mikuterm_jni_detach(attached);
		if (ok)
			return 0;
	}

	if (snprintf(path, size, "%s", PREFIX_MEDIASTORE) >= (int)size)
		return ENAMETOOLONG;
	return 0;
}

/* Queries metadata for path without following symlinks. */
int
sftp_local_stat(
	const char *path,
	struct sftp_local_entry *entry)
{
	char id[64];
	const char *rel;
	const char *slash;
	struct stat st;
	JNIEnv *env;
	jclass cls;
	jobject ctx;
	jmethodID mid;
	jstring j1;
	jstring j2;
	jlongArray arr;
	jlong vals[3];
	int attached;
	int found;

	if (path == NULL || entry == NULL)
		return EINVAL;
	memset(entry, 0, sizeof(*entry));
	slash = strrchr(path, '/');
	(void)snprintf(entry->name, sizeof(entry->name), "%s", slash != NULL ? slash + 1 : path);

	if (parse_saf_doc(path, id, sizeof(id))) {
		if (mikuterm_jni_attach(&env, &attached) != 0)
			return EIO;
		cls = mikuterm_jni_sftp_storage_class();
		ctx = mikuterm_jni_app_context(env);
		found = 0;
		if (cls != NULL && ctx != NULL) {
			mid = (*env)->GetStaticMethodID(env, cls, "statSafDoc", "(Landroid/content/Context;Ljava/lang/String;)[J");
			if (!jni_clear_exc(env) && mid != NULL) {
				j1 = (*env)->NewStringUTF(env, id);
				if (j1 != NULL) {
					arr = (jlongArray)(*env)->CallStaticObjectMethod(env, cls, mid, ctx, j1);
					if (!jni_clear_exc(env) && arr != NULL) {
						if ((*env)->GetArrayLength(env, arr) >= 3) {
							(*env)->GetLongArrayRegion(env, arr, 0, 3, vals);
							entry->is_dir = (vals[0] != 0);
							entry->size = (uint64_t)vals[1];
							entry->mtime = (uint32_t)vals[2];
							found = 1;
						}
						(*env)->DeleteLocalRef(env, arr);
					}
					(*env)->DeleteLocalRef(env, j1);
				}
			}
		}
		if (ctx != NULL)
			(*env)->DeleteLocalRef(env, ctx);
		mikuterm_jni_detach(attached);
		return found ? 0 : ENOENT;
	}

	if (parse_mediastore(path, &rel)) {
		if (rel[0] == '\0') {
			entry->is_dir = 1;
			return 0;
		}
		if (mikuterm_jni_attach(&env, &attached) != 0)
			return EIO;
		cls = mikuterm_jni_sftp_storage_class();
		ctx = mikuterm_jni_app_context(env);
		found = 0;
		if (cls != NULL && ctx != NULL) {
			mid = (*env)->GetStaticMethodID(env, cls, "statMediaStoreDownload", "(Landroid/content/Context;Ljava/lang/String;)[J");
			if (!jni_clear_exc(env) && mid != NULL) {
				j1 = (*env)->NewStringUTF(env, rel);
				if (j1 != NULL) {
					arr = (jlongArray)(*env)->CallStaticObjectMethod(env, cls, mid, ctx, j1);
					if (!jni_clear_exc(env) && arr != NULL) {
						if ((*env)->GetArrayLength(env, arr) >= 3) {
							(*env)->GetLongArrayRegion(env, arr, 0, 3, vals);
							entry->is_dir = (vals[0] != 0);
							entry->size = (uint64_t)vals[1];
							entry->mtime = (uint32_t)vals[2];
							found = 1;
						}
						(*env)->DeleteLocalRef(env, arr);
					}
					(*env)->DeleteLocalRef(env, j1);
				}
			}
		}
		if (ctx != NULL)
			(*env)->DeleteLocalRef(env, ctx);
		mikuterm_jni_detach(attached);
		return found ? 0 : ENOENT;
	}

	if (parse_saf_tree(path, id, sizeof(id), &rel)) {
		if (mikuterm_jni_attach(&env, &attached) != 0)
			return EIO;
		cls = mikuterm_jni_sftp_storage_class();
		ctx = mikuterm_jni_app_context(env);
		found = 0;
		if (cls != NULL && ctx != NULL) {
			mid = (*env)->GetStaticMethodID(env, cls, "statSafTree", "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)[J");
			if (!jni_clear_exc(env) && mid != NULL) {
				j1 = (*env)->NewStringUTF(env, id);
				j2 = (*env)->NewStringUTF(env, rel);
				if (j1 != NULL && j2 != NULL) {
					arr = (jlongArray)(*env)->CallStaticObjectMethod(env, cls, mid, ctx, j1, j2);
					if (!jni_clear_exc(env) && arr != NULL) {
						if ((*env)->GetArrayLength(env, arr) >= 3) {
							(*env)->GetLongArrayRegion(env, arr, 0, 3, vals);
							entry->is_dir = (vals[0] != 0);
							entry->size = (uint64_t)vals[1];
							entry->mtime = (uint32_t)vals[2];
							found = 1;
						}
						(*env)->DeleteLocalRef(env, arr);
					}
				}
				if (j1 != NULL)
					(*env)->DeleteLocalRef(env, j1);
				if (j2 != NULL)
					(*env)->DeleteLocalRef(env, j2);
			}
		}
		if (ctx != NULL)
			(*env)->DeleteLocalRef(env, ctx);
		mikuterm_jni_detach(attached);
		return found ? 0 : ENOENT;
	}

	if (lstat(path, &st) != 0)
		return errno != 0 ? errno : EIO;
	entry->is_dir = S_ISDIR(st.st_mode);
	entry->is_reparse = S_ISLNK(st.st_mode);
	entry->size = (uint64_t)st.st_size;
	entry->mtime = (uint32_t)st.st_mtime;
	return 0;
}

/* Lists entries in a local directory. */
int
sftp_local_list(
	const char *dir_path,
	struct sftp_local_entry **entries,
	size_t *count)
{
	char id[64];
	const char *rel;
	JNIEnv *env;
	jclass cls;
	jobject ctx;
	jmethodID mid;
	jstring j1;
	jstring j2;
	jobjectArray arr;
	jsize len;
	jsize i;
	size_t n;
	struct sftp_local_entry *list;
	int attached;
	DIR *dp;
	struct dirent *de;
	char full[MIKUTERM_PATH_MAX];
	struct stat st;
	size_t cap;

	if (dir_path == NULL || entries == NULL || count == NULL)
		return EINVAL;
	*entries = NULL;
	*count = 0U;

	if (parse_saf_tree(dir_path, id, sizeof(id), &rel)) {
		if (mikuterm_jni_attach(&env, &attached) != 0)
			return EIO;
		cls = mikuterm_jni_sftp_storage_class();
		ctx = mikuterm_jni_app_context(env);
		list = NULL;
		n = 0U;
		if (cls != NULL && ctx != NULL) {
			mid = (*env)->GetStaticMethodID(env, cls, "listSafTree", "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;");
			if (!jni_clear_exc(env) && mid != NULL) {
				j1 = (*env)->NewStringUTF(env, id);
				j2 = (*env)->NewStringUTF(env, rel);
				if (j1 != NULL && j2 != NULL) {
					arr = (jobjectArray)(*env)->CallStaticObjectMethod(env, cls, mid, ctx, j1, j2);
					if (!jni_clear_exc(env) && arr != NULL) {
						len = (*env)->GetArrayLength(env, arr);
						n = (size_t)(len / 4);
						if (n > 0U)
							list = calloc(n, sizeof(*list));
						if (list != NULL) {
							for (i = 0; (size_t)i < n; i++) {
								jstring js_name = (jstring)(*env)->GetObjectArrayElement(env, arr, i * 4);
								jstring js_dir = (jstring)(*env)->GetObjectArrayElement(env, arr, i * 4 + 1);
								jstring js_sz = (jstring)(*env)->GetObjectArrayElement(env, arr, i * 4 + 2);
								jstring js_mt = (jstring)(*env)->GetObjectArrayElement(env, arr, i * 4 + 3);
								const char *s;
								if (js_name != NULL) {
									s = (*env)->GetStringUTFChars(env, js_name, NULL);
									if (s != NULL) {
										(void)snprintf(list[i].name, sizeof(list[i].name), "%s", s);
										(*env)->ReleaseStringUTFChars(env, js_name, s);
									}
									(*env)->DeleteLocalRef(env, js_name);
								}
								if (js_dir != NULL) {
									s = (*env)->GetStringUTFChars(env, js_dir, NULL);
									if (s != NULL) {
										list[i].is_dir = (s[0] == '1');
										(*env)->ReleaseStringUTFChars(env, js_dir, s);
									}
									(*env)->DeleteLocalRef(env, js_dir);
								}
								if (js_sz != NULL) {
									s = (*env)->GetStringUTFChars(env, js_sz, NULL);
									if (s != NULL) {
										list[i].size = strtoull(s, NULL, 10);
										(*env)->ReleaseStringUTFChars(env, js_sz, s);
									}
									(*env)->DeleteLocalRef(env, js_sz);
								}
								if (js_mt != NULL) {
									s = (*env)->GetStringUTFChars(env, js_mt, NULL);
									if (s != NULL) {
										list[i].mtime = (uint32_t)strtoul(s, NULL, 10);
										(*env)->ReleaseStringUTFChars(env, js_mt, s);
									}
									(*env)->DeleteLocalRef(env, js_mt);
								}
							}
						}
						(*env)->DeleteLocalRef(env, arr);
					}
				}
				if (j1 != NULL)
					(*env)->DeleteLocalRef(env, j1);
				if (j2 != NULL)
					(*env)->DeleteLocalRef(env, j2);
			}
		}
		if (ctx != NULL)
			(*env)->DeleteLocalRef(env, ctx);
		mikuterm_jni_detach(attached);
		*entries = list;
		*count = n;
		return 0;
	}

	dp = opendir(dir_path);
	if (dp == NULL)
		return errno != 0 ? errno : EIO;
	cap = 0U;
	list = NULL;
	n = 0U;
	while ((de = readdir(dp)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (n == cap) {
			size_t ncap = (cap == 0U) ? 16U : cap * 2U;
			struct sftp_local_entry *tmp = realloc(list, ncap * sizeof(*list));
			if (tmp == NULL) {
				free(list);
				closedir(dp);
				return ENOMEM;
			}
			list = tmp;
			cap = ncap;
		}
		memset(&list[n], 0, sizeof(list[n]));
		(void)snprintf(list[n].name, sizeof(list[n].name), "%s", de->d_name);
		if (sftp_local_join_path(full, sizeof(full), dir_path, de->d_name) == 0 && lstat(full, &st) == 0) {
			list[n].is_dir = S_ISDIR(st.st_mode);
			list[n].is_reparse = S_ISLNK(st.st_mode);
			list[n].size = (uint64_t)st.st_size;
			list[n].mtime = (uint32_t)st.st_mtime;
		}
		n++;
	}
	closedir(dp);
	*entries = list;
	*count = n;
	return 0;
}

/* Frees an entry list returned by sftp_local_list. */
void
sftp_local_list_free(
	struct sftp_local_entry *entries,
	size_t count)
{
	(void)count;
	free(entries);
}

/* Opens a local file or SAF document URI for reading. */
int
sftp_local_open_read(
	const char *path,
	struct sftp_local_file **file,
	uint64_t *size,
	uint32_t *mtime)
{
	struct sftp_local_entry st;
	struct sftp_local_file *f;
	JNIEnv *env;
	jclass cls;
	jobject ctx;
	jmethodID mid;
	jstring j1;
	jstring j2;
	int attached;
	int fd;
	int err;

	if (path == NULL || file == NULL)
		return EINVAL;
	*file = NULL;

	if (strncmp(path, PREFIX_SAF_DOC, strlen(PREFIX_SAF_DOC)) == 0 ||
	    strncmp(path, "content://", 10) == 0) {
		err = sftp_local_stat(path, &st);
		if (err != 0)
			return err;
		if (mikuterm_jni_attach(&env, &attached) != 0)
			return EIO;
		cls = mikuterm_jni_sftp_storage_class();
		ctx = mikuterm_jni_app_context(env);
		fd = -1;
		if (cls != NULL && ctx != NULL) {
			mid = (*env)->GetStaticMethodID(env, cls, "openFd", "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)I");
			if (!jni_clear_exc(env) && mid != NULL) {
				j1 = (*env)->NewStringUTF(env, path);
				j2 = (*env)->NewStringUTF(env, "r");
				if (j1 != NULL && j2 != NULL) {
					fd = (int)(*env)->CallStaticIntMethod(env, cls, mid, ctx, j1, j2);
					if (jni_clear_exc(env))
						fd = -1;
				}
				if (j1 != NULL)
					(*env)->DeleteLocalRef(env, j1);
				if (j2 != NULL)
					(*env)->DeleteLocalRef(env, j2);
			}
		}
		if (ctx != NULL)
			(*env)->DeleteLocalRef(env, ctx);
		mikuterm_jni_detach(attached);
		if (fd < 0)
			return EIO;
		f = calloc(1U, sizeof(*f));
		if (f == NULL) {
			close(fd);
			return ENOMEM;
		}
		f->fd = fd;
		if (size != NULL)
			*size = st.size;
		if (mtime != NULL)
			*mtime = st.mtime;
		*file = f;
		return 0;
	}

	fd = open(path, O_RDONLY | O_NOFOLLOW);
	if (fd < 0)
		return errno != 0 ? errno : EIO;
	err = sftp_local_stat(path, &st);
	if (err != 0) {
		close(fd);
		return err;
	}
	f = calloc(1U, sizeof(*f));
	if (f == NULL) {
		close(fd);
		return ENOMEM;
	}
	f->fd = fd;
	if (size != NULL)
		*size = st.size;
	if (mtime != NULL)
		*mtime = st.mtime;
	*file = f;
	return 0;
}

/* Reads up to size bytes at offset using pread(). */
int
sftp_local_read_at(
	struct sftp_local_file *file,
	uint64_t offset,
	void *buf,
	size_t size,
	size_t *got)
{
	ssize_t n;

	if (file == NULL || file->fd < 0 || got == NULL)
		return EINVAL;
	*got = 0U;
	n = pread(file->fd, buf, size, (off_t)offset);
	if (n < 0)
		return errno != 0 ? errno : EIO;
	*got = (size_t)n;
	return 0;
}

/* Creates a temporary download file (MediaStore IS_PENDING=1 or SAF part) and opens its fd. */
int
sftp_local_open_temp_write(
	const char *dir_path,
	const char *final_name,
	char *temp_path,
	size_t temp_cap,
	struct sftp_local_file **file)
{
	char id[64];
	const char *rel;
	struct sftp_local_file *f;
	JNIEnv *env;
	jclass cls;
	jobject ctx;
	jmethodID mid;
	jmethodID mid_open;
	jstring j1;
	jstring j2;
	jstring j3;
	jstring juri;
	jstring jmode;
	const char *utf;
	int attached;
	int fd;

	if (dir_path == NULL || final_name == NULL || temp_path == NULL || file == NULL)
		return EINVAL;
	*file = NULL;
	temp_path[0] = '\0';

	if (parse_mediastore(dir_path, &rel)) {
		if (mikuterm_jni_attach(&env, &attached) != 0)
			return EIO;
		cls = mikuterm_jni_sftp_storage_class();
		ctx = mikuterm_jni_app_context(env);
		fd = -1;
		if (cls != NULL && ctx != NULL) {
			mid = (*env)->GetStaticMethodID(env, cls, "createPendingDownload", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
			mid_open = (*env)->GetStaticMethodID(env, cls, "openFd", "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)I");
			if (!jni_clear_exc(env) && mid != NULL && mid_open != NULL) {
				j1 = (*env)->NewStringUTF(env, rel);
				j2 = (*env)->NewStringUTF(env, final_name);
				if (j1 != NULL && j2 != NULL) {
					juri = (jstring)(*env)->CallStaticObjectMethod(env, cls, mid, ctx, j1, j2);
					if (!jni_clear_exc(env) && juri != NULL) {
						utf = (*env)->GetStringUTFChars(env, juri, NULL);
						if (utf != NULL) {
							if (snprintf(temp_path, temp_cap, "%s", utf) < (int)temp_cap) {
								jmode = (*env)->NewStringUTF(env, "rw");
								if (jmode != NULL) {
									fd = (int)(*env)->CallStaticIntMethod(env, cls, mid_open, ctx, juri, jmode);
									if (jni_clear_exc(env))
										fd = -1;
									(*env)->DeleteLocalRef(env, jmode);
								}
							}
							(*env)->ReleaseStringUTFChars(env, juri, utf);
						}
						(*env)->DeleteLocalRef(env, juri);
					}
				}
				if (j1 != NULL)
					(*env)->DeleteLocalRef(env, j1);
				if (j2 != NULL)
					(*env)->DeleteLocalRef(env, j2);
			}
		}
		if (ctx != NULL)
			(*env)->DeleteLocalRef(env, ctx);
		mikuterm_jni_detach(attached);
		if (fd < 0)
			return EIO;
		f = calloc(1U, sizeof(*f));
		if (f == NULL) {
			close(fd);
			return ENOMEM;
		}
		f->fd = fd;
		*file = f;
		return 0;
	}

	if (parse_saf_tree(dir_path, id, sizeof(id), &rel)) {
		if (mikuterm_jni_attach(&env, &attached) != 0)
			return EIO;
		cls = mikuterm_jni_sftp_storage_class();
		ctx = mikuterm_jni_app_context(env);
		fd = -1;
		if (cls != NULL && ctx != NULL) {
			mid = (*env)->GetStaticMethodID(env, cls, "createSafTemp", "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
			mid_open = (*env)->GetStaticMethodID(env, cls, "openFd", "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)I");
			if (!jni_clear_exc(env) && mid != NULL && mid_open != NULL) {
				j1 = (*env)->NewStringUTF(env, id);
				j2 = (*env)->NewStringUTF(env, rel);
				j3 = (*env)->NewStringUTF(env, final_name);
				if (j1 != NULL && j2 != NULL && j3 != NULL) {
					juri = (jstring)(*env)->CallStaticObjectMethod(env, cls, mid, ctx, j1, j2, j3);
					if (!jni_clear_exc(env) && juri != NULL) {
						utf = (*env)->GetStringUTFChars(env, juri, NULL);
						if (utf != NULL) {
							if (snprintf(temp_path, temp_cap, "%s", utf) < (int)temp_cap) {
								jmode = (*env)->NewStringUTF(env, "rw");
								if (jmode != NULL) {
									fd = (int)(*env)->CallStaticIntMethod(env, cls, mid_open, ctx, juri, jmode);
									if (jni_clear_exc(env))
										fd = -1;
									(*env)->DeleteLocalRef(env, jmode);
								}
							}
							(*env)->ReleaseStringUTFChars(env, juri, utf);
						}
						(*env)->DeleteLocalRef(env, juri);
					}
				}
				if (j1 != NULL)
					(*env)->DeleteLocalRef(env, j1);
				if (j2 != NULL)
					(*env)->DeleteLocalRef(env, j2);
				if (j3 != NULL)
					(*env)->DeleteLocalRef(env, j3);
			}
		}
		if (ctx != NULL)
			(*env)->DeleteLocalRef(env, ctx);
		mikuterm_jni_detach(attached);
		if (fd < 0)
			return EIO;
		f = calloc(1U, sizeof(*f));
		if (f == NULL) {
			close(fd);
			return ENOMEM;
		}
		f->fd = fd;
		*file = f;
		return 0;
	}

	if (snprintf(temp_path, temp_cap, "%s/.%s.mikuterm.part", dir_path, final_name) >= (int)temp_cap)
		return ENAMETOOLONG;
	fd = open(temp_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return errno != 0 ? errno : EIO;
	f = calloc(1U, sizeof(*f));
	if (f == NULL) {
		close(fd);
		return ENOMEM;
	}
	f->fd = fd;
	*file = f;
	return 0;
}

/* Writes size bytes at offset using pwrite(). */
int
sftp_local_write_at(
	struct sftp_local_file *file,
	uint64_t offset,
	const void *buf,
	size_t size)
{
	const uint8_t *p;
	size_t left;
	ssize_t n;

	if (file == NULL || file->fd < 0)
		return EINVAL;
	p = (const uint8_t *)buf;
	left = size;
	while (left > 0U) {
		n = pwrite(file->fd, p, left, (off_t)offset);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return errno != 0 ? errno : EIO;
		}
		p += n;
		offset += (uint64_t)n;
		left -= (size_t)n;
	}
	return 0;
}

/* Sets modification time when supported by the underlying file descriptor. */
int
sftp_local_set_mtime(
	struct sftp_local_file *file,
	uint32_t mtime)
{
	struct timespec ts[2];

	if (file == NULL || file->fd < 0)
		return EINVAL;
	ts[0].tv_sec = (time_t)mtime;
	ts[0].tv_nsec = 0;
	ts[1].tv_sec = (time_t)mtime;
	ts[1].tv_nsec = 0;
	(void)futimens(file->fd, ts);
	return 0;
}

/* Closes the detached file descriptor and frees the handle. */
void
sftp_local_close(
	struct sftp_local_file *file)
{
	if (file == NULL)
		return;
	if (file->fd >= 0)
		close(file->fd);
	free(file);
}

/* Commits a temporary download (IS_PENDING=0 for MediaStore, rename for SAF/POSIX). */
int
sftp_local_commit_temp(
	const char *temp_path,
	const char *final_path)
{
	const char *slash;
	const char *final_name;
	JNIEnv *env;
	jclass cls;
	jobject ctx;
	jmethodID mid;
	jstring j1;
	jstring j2;
	int attached;
	jboolean ok;

	if (temp_path == NULL || final_path == NULL)
		return EINVAL;

	if (strncmp(temp_path, "content://", 10) == 0) {
		if (mikuterm_jni_attach(&env, &attached) != 0)
			return EIO;
		cls = mikuterm_jni_sftp_storage_class();
		ctx = mikuterm_jni_app_context(env);
		ok = JNI_FALSE;
		if (cls != NULL && ctx != NULL) {
			if (strncmp(final_path, PREFIX_MEDIASTORE, strlen(PREFIX_MEDIASTORE)) == 0) {
				mid = (*env)->GetStaticMethodID(env, cls, "commitPendingDownload", "(Landroid/content/Context;Ljava/lang/String;)Z");
				if (!jni_clear_exc(env) && mid != NULL) {
					j1 = (*env)->NewStringUTF(env, temp_path);
					if (j1 != NULL) {
						ok = (*env)->CallStaticBooleanMethod(env, cls, mid, ctx, j1);
						if (jni_clear_exc(env))
							ok = JNI_FALSE;
						(*env)->DeleteLocalRef(env, j1);
					}
				}
			} else {
				slash = strrchr(final_path, '/');
				final_name = (slash != NULL) ? slash + 1 : final_path;
				mid = (*env)->GetStaticMethodID(env, cls, "commitSafTemp", "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Z");
				if (!jni_clear_exc(env) && mid != NULL) {
					j1 = (*env)->NewStringUTF(env, temp_path);
					j2 = (*env)->NewStringUTF(env, final_name);
					if (j1 != NULL && j2 != NULL) {
						ok = (*env)->CallStaticBooleanMethod(env, cls, mid, ctx, j1, j2);
						if (jni_clear_exc(env))
							ok = JNI_FALSE;
					}
					if (j1 != NULL)
						(*env)->DeleteLocalRef(env, j1);
					if (j2 != NULL)
						(*env)->DeleteLocalRef(env, j2);
				}
			}
		}
		if (ctx != NULL)
			(*env)->DeleteLocalRef(env, ctx);
		mikuterm_jni_detach(attached);
		return ok ? 0 : EIO;
	}

	if (rename(temp_path, final_path) != 0)
		return errno != 0 ? errno : EIO;
	return 0;
}

/* Removes a temporary or target file. */
int
sftp_local_remove(
	const char *path)
{
	JNIEnv *env;
	jclass cls;
	jobject ctx;
	jmethodID mid;
	jstring j1;
	int attached;
	jboolean ok;

	if (path == NULL)
		return EINVAL;

	if (strncmp(path, "content://", 10) == 0) {
		if (mikuterm_jni_attach(&env, &attached) != 0)
			return EIO;
		cls = mikuterm_jni_sftp_storage_class();
		ctx = mikuterm_jni_app_context(env);
		ok = JNI_FALSE;
		if (cls != NULL && ctx != NULL) {
			mid = (*env)->GetStaticMethodID(env, cls, "deleteUri", "(Landroid/content/Context;Ljava/lang/String;)Z");
			if (!jni_clear_exc(env) && mid != NULL) {
				j1 = (*env)->NewStringUTF(env, path);
				if (j1 != NULL) {
					ok = (*env)->CallStaticBooleanMethod(env, cls, mid, ctx, j1);
					if (jni_clear_exc(env))
						ok = JNI_FALSE;
					(*env)->DeleteLocalRef(env, j1);
				}
			}
		}
		if (ctx != NULL)
			(*env)->DeleteLocalRef(env, ctx);
		mikuterm_jni_detach(attached);
		return ok ? 0 : EIO;
	}

	if (unlink(path) != 0 && errno != ENOENT)
		return errno != 0 ? errno : EIO;
	return 0;
}

/* Creates a local directory (no-op for MediaStore, recursive for SAF tree). */
int
sftp_local_mkdir(
	const char *path)
{
	char id[64];
	const char *rel;
	JNIEnv *env;
	jclass cls;
	jobject ctx;
	jmethodID mid;
	jstring j1;
	jstring j2;
	int attached;
	jboolean ok;

	if (path == NULL)
		return EINVAL;
	if (parse_mediastore(path, &rel))
		return 0;

	if (parse_saf_tree(path, id, sizeof(id), &rel)) {
		if (mikuterm_jni_attach(&env, &attached) != 0)
			return EIO;
		cls = mikuterm_jni_sftp_storage_class();
		ctx = mikuterm_jni_app_context(env);
		ok = JNI_FALSE;
		if (cls != NULL && ctx != NULL) {
			mid = (*env)->GetStaticMethodID(env, cls, "mkdirSafTree", "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Z");
			if (!jni_clear_exc(env) && mid != NULL) {
				j1 = (*env)->NewStringUTF(env, id);
				j2 = (*env)->NewStringUTF(env, rel);
				if (j1 != NULL && j2 != NULL) {
					ok = (*env)->CallStaticBooleanMethod(env, cls, mid, ctx, j1, j2);
					if (jni_clear_exc(env))
						ok = JNI_FALSE;
				}
				if (j1 != NULL)
					(*env)->DeleteLocalRef(env, j1);
				if (j2 != NULL)
					(*env)->DeleteLocalRef(env, j2);
			}
		}
		if (ctx != NULL)
			(*env)->DeleteLocalRef(env, ctx);
		mikuterm_jni_detach(attached);
		return ok ? 0 : EIO;
	}

	if (mkdir(path, 0755) != 0 && errno != EEXIST)
		return errno != 0 ? errno : EIO;
	return 0;
}

/* Joins dir and name with '/'. */
int
sftp_local_join_path(
	char *out,
	size_t out_cap,
	const char *dir,
	const char *name)
{
	size_t dlen;
	int written;

	if (out == NULL || out_cap == 0U || dir == NULL || name == NULL)
		return EINVAL;
	dlen = strlen(dir);
	if (dlen > 0U && dir[dlen - 1U] == '/')
		written = snprintf(out, out_cap, "%s%s", dir, name);
	else
		written = snprintf(out, out_cap, "%s/%s", dir, name);
	if (written < 0 || (size_t)written >= out_cap)
		return ENAMETOOLONG;
	return 0;
}
