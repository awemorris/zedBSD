/* Production direct primitive, ioctl and time boundaries only. */
#include "wifi-story.h"
#define ARG_MAX 16384
#define ioctl story_ioctl
#define socket story_socket
#define clock_gettime story_clock_gettime
#define nanosleep story_nanosleep
#define printf story_wifi_printf
#define main story_wifi_main
#include "userland/base/wifi/main.c"
