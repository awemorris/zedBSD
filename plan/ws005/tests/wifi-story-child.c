/* Real pipes, fork, poll, credentials on FD 4, signals and reaping. */
#include "wifi-story.h"
#define execv story_execv
#define waitpid story_waitpid
#include "userland/base/networkd/wifi-child.c"
