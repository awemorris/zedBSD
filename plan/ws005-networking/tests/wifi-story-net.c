/* Production net grammar, credential update and framed client. */
#include "wifi-story.h"
#define socket story_client_socket
#define connect story_client_connect
#define shutdown story_client_shutdown
#define setsockopt story_setsockopt
#define main story_net_unused_main
#include "userland/base/net/main.c"
#undef main
int story_net_command(int argc, char **argv)
{
 if (argc >= 3 && strcmp(argv[2], "set-key") == 0)
  return wifi_set_key_command(argc, argv);
 return wifi_command(argc, argv);
}
