/* Host-compile layout fixture for the public ILP32/LP64 ABI. */
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <termios.h>
#include <zedbsd/poll.h>
#include <zedbsd/select.h>
#include <zedbsd/console.h>
#include <zedbsd/graphics.h>
#include <zedbsd/fcntl.h>
#include <zedbsd/auxv.h>
#include <zedbsd/netif.h>
#include <zedbsd/signal.h>
#include <zedbsd/socket.h>
#include <zedbsd/syscall.h>
#include <zedbsd/sysctl.h>
#include <zedbsd/system.h>
#include <zedbsd/resource.h>
#include <zedbsd/quota.h>
#include <zedbsd/snapshot.h>

#ifdef ZEDBSD_USER_ABI_LP64
_Static_assert(sizeof(void *) == 8, "LP64 pointer");
_Static_assert(sizeof(long) == 8, "LP64 long");
_Static_assert(sizeof(off_t) == 8, "LP64 off_t");
_Static_assert(sizeof(time_t) == 8, "LP64 time_t");
_Static_assert(sizeof(struct timeval) == 16, "LP64 timeval");
_Static_assert(sizeof(struct iovec) == 16, "LP64 iovec");
_Static_assert(sizeof(struct zedbsd_sendmsg_request) == 48,
    "LP64 sendmsg request");
_Static_assert(sizeof(struct zedbsd_recvmsg_request) == 72,
    "LP64 recvmsg request");
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
_Static_assert(sizeof(struct zedbsd_sendmsg_request) == 36,
    "ILP32 sendmsg request");
_Static_assert(sizeof(struct zedbsd_recvmsg_request) == 60,
    "ILP32 recvmsg request");
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
_Static_assert(sizeof(siginfo_t) == 128, "siginfo fixed ABI");
_Static_assert(sizeof(union sigval) == 8, "sigval fixed ABI");
_Static_assert(sizeof(struct zedbsd_sigaltstack) == 24,
    "sigaltstack request fixed ABI");
_Static_assert(sizeof(mcontext_t) == 64, "mcontext fixed ABI");
_Static_assert(sizeof(ucontext_t) == 128, "ucontext fixed ABI");
_Static_assert(offsetof(ucontext_t, uc_mcontext) == 24,
    "ucontext machine-context offset");
_Static_assert(sizeof(struct zedbsd_bufcache_stats) == 104,
    "bufcache stats fixed ABI");
_Static_assert(sizeof(struct zedbsd_system_resources) == 136,
    "resource snapshot fixed ABI");
_Static_assert(sizeof(struct pollfd) == 8, "pollfd fixed ABI");
_Static_assert(sizeof(zedbsd_fd_set_t) == 4, "fd_set fixed ABI");
_Static_assert(sizeof(struct termios) == 44, "termios fixed ABI");
_Static_assert(sizeof(struct zedbsd_flock_request) == 32,
    "flock request fixed ABI");
_Static_assert(offsetof(struct zedbsd_flock_request, start) == 8,
    "flock start fixed offset");
_Static_assert(sizeof(struct zedbsd_rlimit) == 16,
    "rlimit fixed ABI");
_Static_assert(sizeof(struct winsize) == 8, "winsize fixed ABI");
_Static_assert(sizeof(struct zedbsd_quota_ctl) == 96,
    "quota control fixed ABI");
_Static_assert(sizeof(struct zedbsd_snapshot_ctl) == 48,
    "snapshot control fixed ABI");
_Static_assert(ZEDBSD_SYS_sysctl == 103, "sysctl syscall ABI");
_Static_assert(ZEDBSD_SYS_ppoll == 104, "ppoll syscall ABI");
_Static_assert(ZEDBSD_SYS_pselect == 105, "pselect syscall ABI");
_Static_assert(ZEDBSD_SYS_sigaltstack == 106, "sigaltstack syscall ABI");
_Static_assert(ZEDBSD_SYS_sigtimedwait == 107, "sigtimedwait syscall ABI");
_Static_assert(ZEDBSD_SYS_sigqueue == 108, "sigqueue syscall ABI");
_Static_assert(ZEDBSD_SYS_thread_create == 109, "thread-create syscall ABI");
_Static_assert(ZEDBSD_SYS_usync == 116, "usync syscall ABI");
_Static_assert(ZEDBSD_SYS_socketpair == 117, "socketpair syscall ABI");
_Static_assert(ZEDBSD_SYS_sendmsg == 118, "sendmsg syscall ABI");
_Static_assert(ZEDBSD_SYS_recvmsg == 119, "recvmsg syscall ABI");
_Static_assert(ZEDBSD_SYS_fchdir == 120, "fchdir syscall ABI");
_Static_assert(ZEDBSD_SYS_mknodat == 121, "mknodat syscall ABI");
_Static_assert(ZEDBSD_SYS_getrlimit == 122, "getrlimit syscall ABI");
_Static_assert(ZEDBSD_SYS_setrlimit == 123, "setrlimit syscall ABI");
_Static_assert(ZEDBSD_SYS_waitid == 124, "waitid syscall ABI");
_Static_assert(ZEDBSD_SYS_quotactl == 146, "quotactl syscall ABI");
_Static_assert(ZEDBSD_SYS_snapshotctl == 147, "snapshotctl syscall ABI");
_Static_assert(AT_PHDR == 3 && AT_BASE == 7 && AT_ENTRY == 9,
    "ELF auxv ABI");

int main(void) { return 0; }
