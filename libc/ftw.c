/* SUS file-tree traversal. SPDX-License-Identifier: Zlib */
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

#if defined(ZEDBSD_DYNAMIC_LIBC)
#define FTW_THREAD_LOCAL _Thread_local
#else
#define FTW_THREAD_LOCAL
#endif

struct walk_context {
	int (*callback)(const char *, const struct stat *, int, struct FTW *);
	int flags;
	dev_t root_device;
};

static int
base_offset(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash == NULL ? 0 : (int)(slash + 1 - path);
}

static int
walk_path(struct walk_context *context, char *path, int level)
{
	struct stat status;
	struct FTW info = { base_offset(path), level };
	DIR *directory;
	struct dirent *entry;
	int kind, result, saved_errno;

	if ((context->flags & FTW_PHYS) ? lstat(path, &status) : stat(path, &status)) {
		if (lstat(path, &status) == 0 && S_ISLNK(status.st_mode))
			kind = (context->flags & FTW_PHYS) ? FTW_SL : FTW_SLN;
		else {
			memset(&status, 0, sizeof(status));
			kind = FTW_NS;
		}
		return context->callback(path, &status, kind, &info);
	}
	if (S_ISLNK(status.st_mode))
		return context->callback(path, &status, FTW_SL, &info);
	if (!S_ISDIR(status.st_mode))
		return context->callback(path, &status, FTW_F, &info);
	if ((context->flags & FTW_MOUNT) && level != 0 &&
	    status.st_dev != context->root_device)
		return 0;
	directory = opendir(path);
	if (directory == NULL)
		return context->callback(path, &status, FTW_DNR, &info);
	if (!(context->flags & FTW_DEPTH)) {
		result = context->callback(path, &status, FTW_D, &info);
		if (result != 0) { closedir(directory); return result; }
	}
	while ((entry = readdir(directory)) != NULL) {
		size_t length = strlen(path), name_length = strlen(entry->d_name);
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
		if (length + 1 + name_length >= PATH_MAX) {
			closedir(directory); errno = ENAMETOOLONG; return -1;
		}
		path[length] = '/';
		memcpy(path + length + 1, entry->d_name, name_length + 1);
		result = walk_path(context, path, level + 1);
		path[length] = '\0';
		if (result != 0) { closedir(directory); return result; }
	}
	saved_errno = errno;
	if (closedir(directory) != 0) return -1;
	errno = saved_errno;
	if (context->flags & FTW_DEPTH)
		return context->callback(path, &status, FTW_DP, &info);
	return 0;
}

int
nftw(const char *path, int (*callback)(const char *, const struct stat *, int,
    struct FTW *), int descriptors, int flags)
{
	struct walk_context context;
	struct stat status;
	char buffer[PATH_MAX];
	(void)descriptors;
	if (path == NULL || callback == NULL || descriptors < 1 ||
	    (flags & ~(FTW_PHYS | FTW_MOUNT | FTW_DEPTH | FTW_CHDIR))) {
		errno = EINVAL; return -1;
	}
	if (strlen(path) >= sizeof(buffer)) { errno = ENAMETOOLONG; return -1; }
	strcpy(buffer, path);
	if (stat(path, &status) != 0 && lstat(path, &status) != 0)
		memset(&status, 0, sizeof(status));
	context.callback = callback;
	context.flags = flags;
	context.root_device = status.st_dev;
	return walk_path(&context, buffer, 0);
}

static FTW_THREAD_LOCAL int (*ftw_callback)(const char *, const struct stat *, int);
static int ftw_adapter(const char *p, const struct stat *s, int k, struct FTW *f)
{ (void)f; return ftw_callback(p, s, k); }

int
ftw(const char *path, int (*callback)(const char *, const struct stat *, int),
    int descriptors)
{
	int result;
	if (callback == NULL) { errno = EINVAL; return -1; }
	ftw_callback = callback;
	result = nftw(path, ftw_adapter, descriptors, 0);
	ftw_callback = NULL;
	return result;
}
