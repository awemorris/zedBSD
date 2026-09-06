/* Production daemon dispatch, preparation, selection, L3 and retirement. */
#include "wifi-story.h"
#define socket story_socket
#define ioctl story_ioctl
#define setsockopt story_setsockopt
#define getsockopt story_getsockopt
#define open story_open
#define unlink story_unlink
#define execv story_execv
#define accept4 story_accept4
#define main story_daemon_unused_main
#include "userland/base/networkd/main.c"
#undef main

void story_daemon_handle(int descriptor)
{
 struct zedbsd_peercred peer;
 enum networkd_client_role role;
 story_check(authenticate_client(descriptor, &peer, &role) == 0, "peer authentication");
 handle_request(descriptor, role, &peer);
 close(descriptor);
}
void story_daemon_reset(void)
{
 size_t i;
 for (i = 0; i < NETWORKD_WLAN_RADIO_MAX; i++) clear_wifi_observation(i);
 networkd_managed_wlan_init(&managed_wlan);
 networkd_confirmed_init(&confirmed);
 memset(&wifi_work, 0, sizeof(wifi_work));
 memset(known_wlan_radios, 0, sizeof(known_wlan_radios));
 known_wlan_radio_count = 0;
 wifi_disable_pending = 0;
 automatic_retry_at = 0;
 automatic_candidate_skip = 0;
 retirement_retry_seconds = NETWORKD_WLAN_RESCAN_SECONDS;
 route_events = -1;
 route_event_sequence = 0;
}
void story_daemon_tick(void)
{
 run_due_work();
 if (wifi_pending_client >= 0) dispatch_pending_wifi();
}
void story_daemon_listen(int descriptor)
{
 control_listener = descriptor;
 wifi_wait_pump = descriptor >= 0 ? service_wifi_wait : NULL;
 networkd_wifi_child_set_pump(wifi_wait_pump);
}
void story_daemon_service(void)
{
 (void)service_wifi_wait();
 if (wifi_pending_client >= 0) dispatch_pending_wifi();
}
void story_daemon_event(unsigned index, int removed, int carrier)
{
 struct rtm_ifinfo event;
 memset(&event, 0, sizeof(event));
 event.rtm_version = RTM_VERSION;
 event.rtm_type = RTM_IFINFO;
 event.rtm_length = sizeof(event);
 event.rtm_sequence = ++route_event_sequence;
 event.rtm_ifindex = index;
 event.rtm_device_generation = 1;
 event.rtm_transition = removed ? RTM_IFINFO_REMOVAL :
  carrier ? RTM_IFINFO_CARRIER_UP : RTM_IFINFO_CARRIER_DOWN;
 event.rtm_if_flags = carrier ? IFF_UP | IFF_RUNNING : IFF_UP;
 process_route_event(&event);
}
int story_daemon_state(void) { return managed_wlan.state; }
unsigned story_daemon_owner(void) { return managed_wlan.owner_valid ? managed_wlan.owner_uid : (unsigned)-1; }
int story_daemon_l3(void) { return managed_wlan.connection.owns_l3; }
const char *story_daemon_interface(void) { return managed_wlan.connection.interface; }
