/* Host libc jump buffer stays isolated from target libc headers. */
#include <setjmp.h>
static jmp_buf state;
int host_crash_run(void (*action)(void *),void *argument)
{
 if(setjmp(state))return 1;
 action(argument);return 0;
}
void host_crash_now(void) { longjmp(state,1); }
