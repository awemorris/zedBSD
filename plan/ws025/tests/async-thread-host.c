#include <pthread.h>
void async_host_exit(void) { pthread_exit(NULL); }
