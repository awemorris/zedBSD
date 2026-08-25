#include <stddef.h>
#include <dev/evdev/input.h>
#include <linux/input.h>
#include <zedbsd/input.h>

_Static_assert(sizeof(struct input_id) == 8, "input_id ABI");
_Static_assert(sizeof(struct input_absinfo) == 24, "input_absinfo ABI");
_Static_assert(offsetof(struct input_event, type) == sizeof(struct timeval),
	       "input_event type offset");
_Static_assert(offsetof(struct input_event, value) ==
		   sizeof(struct timeval) + 4,
	       "input_event value offset");
#ifdef ZEDBSD_USER_ABI_LP64
_Static_assert(sizeof(struct input_event) == 24, "LP64 input_event ABI");
#else
_Static_assert(sizeof(struct input_event) == 20, "ILP32 input_event ABI");
#endif
_Static_assert(EV_SYN == 0 && EV_KEY == 1 && EV_REL == 2 && EV_ABS == 3,
	       "event type values");
_Static_assert(KEY_A == 30 && BTN_LEFT == 0x110 && REL_X == 0 &&
		   ABS_MT_POSITION_X == 0x35,
	       "event code values");

int
main(void)
{
	return 0;
}
