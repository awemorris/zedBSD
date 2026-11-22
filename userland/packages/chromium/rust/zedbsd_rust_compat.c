#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

int *__error(void)
{
	return __libc_errno_location();
}

int pthread_getthreadid_np(void)
{
	return (int)(uintptr_t)pthread_self();
}

int pthread_setname_np(pthread_t tid, const char *name)
{
	(void)tid;
	(void)name;
	return 0;
}

int pthread_attr_get_np(pthread_t tid, pthread_attr_t *attr)
{
	(void)tid;
	(void)attr;
	return ENOSYS;
}

int cpuset_getaffinity(int level, int which, int64_t id, size_t setsize, void *mask)
{
	(void)level;
	(void)which;
	(void)id;
	(void)setsize;
	(void)mask;
	errno = ENOSYS;
	return -1;
}

int _umtx_op(void *obj, int op, unsigned long val, void *uaddr, void *uaddr2)
{
	(void)obj;
	(void)op;
	(void)val;
	(void)uaddr;
	(void)uaddr2;
	errno = ENOSYS;
	return -1;
}
