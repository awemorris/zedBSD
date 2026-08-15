/* Host-compile layout fixture for the public ILP32/LP64 ABI. */
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <zedbsd/console.h>
#include <zedbsd/graphics.h>
#include <zedbsd/netif.h>
#include <zedbsd/signal.h>

#ifdef ZEDBSD_USER_ABI_LP64
_Static_assert(sizeof(void *) == 8, "LP64 pointer");
_Static_assert(sizeof(long) == 8, "LP64 long");
_Static_assert(sizeof(off_t) == 8, "LP64 off_t");
_Static_assert(sizeof(time_t) == 8, "LP64 time_t");
_Static_assert(sizeof(struct timeval) == 16, "LP64 timeval");
_Static_assert(sizeof(struct iovec) == 16, "LP64 iovec");
_Static_assert(sizeof(struct zedbsd_console_write_at) == 32,
    "LP64 console write-at");
_Static_assert(sizeof(struct zedbsd_graphics_blit) == 64,
    "LP64 graphics blit");
_Static_assert(sizeof(struct zedbsd_graphics_flush) == 16,
    "LP64 graphics flush");
_Static_assert(sizeof(struct zedbsd_graphics_glyph) == 56,
    "LP64 graphics glyph");
#else
_Static_assert(sizeof(void *) == 4, "ILP32 pointer");
_Static_assert(sizeof(long) == 4, "ILP32 long");
_Static_assert(sizeof(off_t) == 4, "ILP32 off_t");
_Static_assert(sizeof(time_t) == 8, "ILP32 time64 time_t");
_Static_assert(sizeof(struct timeval) == 12, "ILP32 time64 timeval");
_Static_assert(sizeof(struct iovec) == 8, "ILP32 iovec");
_Static_assert(sizeof(struct zedbsd_console_write_at) == 20,
    "ILP32 console write-at");
_Static_assert(sizeof(struct zedbsd_graphics_blit) == 56,
    "ILP32 graphics blit");
_Static_assert(sizeof(struct zedbsd_graphics_flush) == 8,
    "ILP32 graphics flush");
_Static_assert(sizeof(struct zedbsd_graphics_glyph) == 48,
    "ILP32 graphics glyph");
#endif

_Static_assert(sizeof(struct ifconf) == 16, "ifconf fixed ABI");
_Static_assert(sizeof(struct sigaction) == 24, "sigaction legacy ABI");

int main(void) { return 0; }
