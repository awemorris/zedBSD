/* Only external OS/radio/store boundaries; no managed Wi-Fi policy here. */
#include "wifi-story.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <net/route.h>

struct story_world *story;
char story_output[65536];
static int daemon_fd = -1;
static int child_output_fd = -1;
static unsigned peer_uid[1024];
static int wifi_radio;
static int async_clients[4], async_servers[4], async_ready[2];
static unsigned async_next, async_accepted;
static int async_child;

void story_check(int ok, const char *what)
{
 if (!ok) { fprintf(stderr, "story assertion: %s (errno=%d)\n", what, errno); abort(); }
}

int story_socket(int domain, int type, int protocol)
{
 return socket(domain, type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), protocol);
}
int story_setsockopt(int fd, int level, int option, const void *value, socklen_t size)
{
 if (level == SOL_SOCKET && (option == SO_RCVTIMEO || option == SO_SNDTIMEO))
  return setsockopt(fd, 1, option == SO_RCVTIMEO ? 20 : 21, value, size);
 errno = EOPNOTSUPP; return -1;
}
int story_getsockopt(int fd, int level, int option, void *value, socklen_t *size)
{
 struct zedbsd_peercred *peer = value;
 story_check(level == SOL_SOCKET && option == SO_PEERCRED && *size == sizeof(*peer), "credential OS boundary");
 story_check(fd >= 0 && fd < 1024, "credential descriptor");
 peer->pid = getpid(); peer->euid = peer_uid[fd]; peer->egid = 1000;
 return 0;
}
int story_client_socket(int domain, int type, int protocol)
{
 int pair[2];
 if (async_child) {
  story_check(async_next < 4, "concurrent request count");
  return async_clients[async_next++];
 }
 story_check(domain == AF_UNIX && (type & 0xff) == SOCK_STREAM && protocol == 0, "client socket");
 story_check(daemon_fd == -1, "one synchronous client transport");
 if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return -1;
 daemon_fd = pair[1]; peer_uid[pair[1]] = story->uid;
 return pair[0];
}
int story_client_connect(int fd, const struct sockaddr *address, socklen_t size)
{
 (void)fd; (void)size; story_check(address->sa_family == AF_UNIX, "client address"); return 0;
}
int story_client_shutdown(int fd, int how)
{
 int server = daemon_fd;
 if (shutdown(fd, how) != 0) return -1;
 if (async_child) {
  story_check(write(async_ready[1], "R", 1) == 1, "publish listener readiness");
  return 0;
 }
 daemon_fd = -1;
 story_daemon_handle(server);
 return 0;
}

int story_clock_gettime(clockid_t clock, struct timespec *value)
{
 (void)clock;
 value->tv_sec = story->now / 1000000ULL;
 value->tv_nsec = (story->now % 1000000ULL) * 1000ULL;
 return 0;
}
int story_nanosleep(const struct timespec *requested, struct timespec *remaining)
{
 struct timespec yield = {0, 100000L};
 (void)remaining;
 __atomic_add_fetch(&story->now, requested->tv_sec * 1000000ULL + requested->tv_nsec / 1000ULL, __ATOMIC_SEQ_CST);
 return nanosleep(&yield, NULL);
}

int wifi_store_set_key_for_effective_user(const char *ssid, const char *key, int automatic, char *diagnostic, size_t capacity)
{
 struct wifi_conf_model model;
 unsigned slot = story->uid == 2000 ? 1 : 0;
 int error;
 wifi_conf_model_init(&model);
 error = 0;
 if (story->store_lengths[slot] != 0)
  error = wifi_conf_parse(story->stores[slot], story->store_lengths[slot], &model, diagnostic, capacity);
 if (error == 0) error = wifi_conf_set_key(&model, ssid, strlen(ssid), key, strlen(key), automatic, diagnostic, capacity);
 if (error == 0) error = wifi_conf_serialize(&model, story->stores[slot], sizeof(story->stores[slot]), &story->store_lengths[slot], diagnostic, capacity);
 wifi_conf_model_clear(&model);
 return error;
}
int wifi_store_load_for_user(uid_t uid, struct wifi_conf_model *model, char *diagnostic, size_t capacity)
{
 unsigned slot = uid == 2000 ? 1 : 0;
 if (story->invalid_store && uid == 2000) { errno = EINVAL; return -1; }
 if (story->store_lengths[slot] == 0) { errno = ENOENT; return -1; }
 return wifi_conf_parse(story->stores[slot], story->store_lengths[slot], model, diagnostic, capacity);
}
int wifi_store_load_for_effective_user(struct wifi_conf_model *model, char *diagnostic, size_t capacity)
{
 return wifi_store_load_for_user(story->uid, model, diagnostic, capacity);
}

int story_open(const char *path, int flags, ...)
{
 int fd;
 (void)flags;
 if (strcmp(path, "/etc/resolv.conf") == 0) {
  if (!story->resolver_present) { errno = ENOENT; return -1; }
  fd = memfd_create("wifi-story-resolver", 0);
  story_check(fd >= 0, "resolver fd");
  story_check(write(fd, story->resolver, strlen(story->resolver)) == (ssize_t)strlen(story->resolver), "resolver bytes");
  lseek(fd, 0, SEEK_SET); return fd;
 }
 if (strncmp(path, "/run/networkd-child.", 20) == 0) {
  if (flags & O_CREAT) {
   story_check(child_output_fd == -1, "DHCP output lifetime");
   child_output_fd = memfd_create("wifi-story-dhcp", 0);
  }
  story_check(child_output_fd >= 0, "DHCP output publication");
  lseek(child_output_fd, 0, SEEK_SET); return dup(child_output_fd);
 }
 errno = ENOENT; return -1;
}
int story_unlink(const char *path)
{
 if (strcmp(path, "/etc/resolv.conf") == 0) { story->resolver_present = 0; return 0; }
 if (strncmp(path, "/run/networkd-child.", 20) == 0) {
  if (child_output_fd >= 0) close(child_output_fd);
  child_output_fd = -1; return 0;
 }
 errno = ENOENT; return -1;
}

static void fill_bss(struct wlan_bss_record *bss, const char *ssid, unsigned identity)
{
 memset(bss, 0, sizeof(*bss));
 memcpy(bss->ssid, ssid, strlen(ssid)); bss->ssid_length = strlen(ssid);
 bss->bssid[0] = 2; bss->bssid[5] = identity;
 bss->channel = 6; bss->center_frequency_mhz = 2437;
 bss->rssi_dbm = -35; bss->beacon_interval_tu = 100;
 bss->capability = 0x11;
 bss->security = WLAN_SECURITY_PRIVACY | WLAN_SECURITY_WPA2 | WLAN_SECURITY_CCMP | WLAN_SECURITY_PSK;
}

int story_ioctl(int fd, unsigned long command, ...)
{
 va_list args;
 void *argument;
 struct ifreq *ifr;
 struct story_radio *radio = NULL;
 unsigned i, count, links;
 (void)fd;
 va_start(args, command); argument = va_arg(args, void *); va_end(args);
 ifr = argument;
 if (command == SIOCGIFCONF) {
  struct ifconf *conf = argument;
  struct ifreq *items = (void *)(uintptr_t)conf->ifc_buf;
  count = 0;
  for (i = 0; i < STORY_RADIOS; i++) if (story->radios[i].present) {
   if (items != NULL) { memset(&items[count], 0, sizeof(*items)); snprintf(items[count].ifr_name, IFNAMSIZ, "wlan%u", i); }
   count++;
  }
  conf->ifc_len = count * sizeof(*items); return 0;
 }
 if (command == SIOCGRTENTRY || command == SIOCDELRT) {
  struct rtentry *route = argument;
  if (!story->route_present || (command == SIOCGRTENTRY && route->rt_index != 0)) { errno = ENOENT; return -1; }
  if (command == SIOCDELRT) { story->route_present = 0; return 0; }
  memset(route, 0, sizeof(*route)); route->rt_flags = RTF_UP | RTF_GATEWAY; route->rt_ifindex = story->route_index;
  route->rt_dst.sa_family = route->rt_genmask.sa_family = route->rt_gateway.sa_family = AF_INET;
  ((struct sockaddr_in *)&route->rt_gateway)->sin_addr.s_addr = 0x0100000a;
  return 0;
 }
 for (i = 0; i < STORY_RADIOS; i++) {
  char name[IFNAMSIZ]; snprintf(name, sizeof(name), "wlan%u", i);
  if (story->radios[i].present && (command == SIOCGIFNAME ?
      story->radios[i].index == (unsigned)ifr->ifr_ifindex : strcmp(ifr->ifr_name, name) == 0)) {
   radio = &story->radios[i]; wifi_radio = i;
   if (command == SIOCGIFNAME) strcpy(ifr->ifr_name, name);
   break;
  }
 }
 if (radio == NULL) { errno = ENODEV; return -1; }
 if (command == SIOCGIFNAME) return 0;
 if (command == SIOCGIFINDEX) { ifr->ifr_ifindex = radio->index; return 0; }
 if (command == SIOCGIFFLAGS) { ifr->ifr_flags = (radio->up ? IFF_UP : 0) | (radio->state == WLAN_STATE_CONNECTED ? IFF_RUNNING : 0); return 0; }
 if (command == SIOCSIFFLAGS) {
  if (ifr->ifr_flags & IFF_UP) {
   if (radio->up_error || radio->stop_pending) { errno = EBUSY; return -1; }
   radio->up = 1; if (radio->state == WLAN_STATE_DOWN) radio->state = WLAN_STATE_IDLE;
  } else {
   radio->up = 0; radio->down_calls++;
   radio->stop_pending = story->stop_fault;
   if (!radio->stop_pending) { radio->state = WLAN_STATE_DOWN; radio->scan = WLAN_SCAN_CANCELLED; }
  }
  return 0;
 }
 if (command == SIOCGIFADDR || command == SIOCGIFNETMASK || command == SIOCGIFBRDADDR ||
     command == SIOCSIFADDR || command == SIOCSIFNETMASK || command == SIOCSIFBRDADDR) {
  uint32_t *word = command == SIOCGIFADDR || command == SIOCSIFADDR ? &radio->address :
   command == SIOCGIFNETMASK || command == SIOCSIFNETMASK ? &radio->netmask : &radio->broadcast;
  struct sockaddr_in *address = (void *)&ifr->ifr_addr;
  if (command == SIOCSIFADDR || command == SIOCSIFNETMASK || command == SIOCSIFBRDADDR) *word = address->sin_addr.s_addr;
  else { memset(address, 0, sizeof(*address)); address->sin_family = AF_INET; address->sin_addr.s_addr = *word; }
  return 0;
 }
 if (command == SIOCSWLANSCAN) {
  struct wlan_scan_request *request = argument;
  if (request->action == WLAN_SCAN_START) {
   if (!radio->up || radio->stop_pending || radio->scan_error) { errno = EIO; return -1; }
   if (radio->scan != WLAN_SCAN_RUNNING) { radio->scans++; radio->polls = 0; radio->scan = WLAN_SCAN_RUNNING; radio->generation++; }
  } else radio->scan = WLAN_SCAN_CANCELLED;
  request->generation = radio->generation; request->state = radio->scan; return 0;
 }
 if (command == SIOCGWLANSCAN) {
  struct wlan_scan_status_request *request = argument;
  if (radio->list_error) { errno = EIO; return -1; }
  if (radio->scan == WLAN_SCAN_RUNNING && ++radio->polls > (unsigned)radio->scan_delay) { radio->scan = WLAN_SCAN_COMPLETE; radio->snapshot = radio->generation; }
  request->state = radio->scan; request->scan_generation = radio->generation;
  request->generation = radio->snapshot; request->cache_sequence = radio->snapshot;
  request->result_count = radio->snapshot && radio->visible ? 2 : 0;
  return 0;
 }
 if (command == SIOCGWLANBSS) {
  struct wlan_bss_request *request = argument;
  if (request->generation != radio->snapshot) { errno = ESTALE; return -1; }
  if (request->index >= 2 || !radio->visible) { errno = ENOENT; return -1; }
  fill_bss(&request->bss, request->index ? "scenario-B" : "scenario-A", request->index + 1);
  return 0;
 }
 if (command == SIOCSWLANCONNECT) {
  struct wlan_connect_request *request = argument;
  radio->connection_attempts++;
  if (radio->connect_error) { errno = radio->connect_error; return -1; }
  if (radio->rapid_scan > 0) { radio->rapid_scan--; errno = ENOENT; return -1; }
  if (!radio->up || radio->stop_pending) { errno = ENETDOWN; return -1; }
  if (!radio->visible) { errno = ENOENT; return -1; }
  radio->auth_error = request->passphrase_length != strlen("scenario-password") || memcmp(request->passphrase, "scenario-password", request->passphrase_length) != 0;
  radio->state = WLAN_STATE_AUTHENTICATING; radio->connect_polls = 0;
  radio->connect_generation = ++story->next_generation;
  memcpy(radio->ssid, request->ssid, request->ssid_length); radio->ssid[request->ssid_length] = 0;
  request->generation = radio->connect_generation; request->state = radio->state;
  memset(request->passphrase, 0, sizeof(request->passphrase)); request->passphrase_length = 0;
  return 0;
 }
 if (command == SIOCSWLANDISCONNECT) {
  struct wlan_disconnect_request *request = argument;
  if (radio->disconnect_error || radio->stop_pending) { errno = EBUSY; return -1; }
  radio->state = radio->up ? WLAN_STATE_IDLE : WLAN_STATE_DOWN;
  request->state = radio->state; request->generation = ++story->next_generation; return 0;
 }
 if (command == SIOCGWLANSTATUS) {
  struct wlan_status_request *request = argument;
  if (radio->status_error) { errno = radio->status_error; return -1; }
  if (radio->state >= WLAN_STATE_AUTHENTICATING && radio->state < WLAN_STATE_CONNECTED) {
   radio->connect_polls++;
   if (radio->auth_error) radio->state = WLAN_STATE_FAILED;
   else if (radio->connect_polls >= (radio->connect_delay ? radio->connect_delay : 3)) { radio->state = WLAN_STATE_CONNECTED; radio->connections++; }
   else if (radio->state < WLAN_STATE_FOUR_WAY) radio->state++;
  }
  request->state = radio->stop_pending ? WLAN_STATE_DISCONNECTING : radio->state;
  request->stop_flags = radio->stop_pending ? WLAN_STATUS_STOP_PENDING : 0;
  request->stop_error = radio->stop_error;
  request->administrative_up = radio->up; request->scan_state = radio->scan;
  request->operation_generation = radio->connect_generation;
  request->scan_generation = radio->generation; request->snapshot_generation = radio->snapshot;
  request->associated = request->key_installed = request->controlled_port = radio->state == WLAN_STATE_CONNECTED;
  request->terminal_error = radio->state == WLAN_STATE_FAILED ? EACCES : 0;
  links = 0; for (i = 0; i < STORY_RADIOS; i++) links += story->radios[i].state == WLAN_STATE_CONNECTED;
  if (links > story->max_links) story->max_links = links;
  return 0;
 }
 errno = EOPNOTSUPP; return -1;
}

int story_wifi_printf(const char *format, ...)
{
 va_list args;
 int result;
 va_start(args, format);
 if (story->radios[wifi_radio].malformed && strstr(format, "WIFI1 scan state=") != NULL)
  result = printf("corrupted-radio-record\n");
 else result = vprintf(format, args);
 va_end(args); return result;
}
int story_execv(const char *path, char *const argv[])
{
 int argc, status, fd;
 unsigned i;
 for (fd = 3; fd < 256; fd++) if (fd != 4 || strcmp(path, "/sbin/wifi") != 0) close(fd);
 if (strcmp(path, "/sbin/wifi") == 0) {
  for (argc = 0; argv[argc] != NULL; argc++) {}
  story->last_child_connect = argc >= 2 && strcmp(argv[argc - 2], "connect") == 0;
  status = story_wifi_main(argc, (char **)argv);
  fflush(NULL);
  if (story->child_deadline && strcmp(argv[argc - 1], "list") == 0) {
   close(1); close(2); sleep(20);
  }
  if (story->eof_exit_delay) { close(1); close(2); usleep(100000); }
  _exit(status);
 }
 story_check(strcmp(path, "/sbin/dhcpc") == 0, "only the DHCP process edge is modeled");
 story->dhcp_count++;
 if (story->dhcp_fail) _exit(1);
 for (i = 0; i < STORY_RADIOS; i++) if (story->radios[i].state == WLAN_STATE_CONNECTED) {
  story->radios[i].address = 0x0200000a; story->radios[i].netmask = 0x00ffffff; story->radios[i].broadcast = 0xff00000a;
  story->route_index = story->radios[i].index; story->route_present = 1;
 }
 story->resolver_present = 1; strcpy(story->resolver, "nameserver 10.0.0.1\n");
 _exit(0);
}

pid_t story_waitpid(pid_t child, int *status, int options)
{
 pid_t result = waitpid(child, status, options);
 /* The child is really reaped before injecting a lost wait result. */
 if (result > 0 && story->child_wait_fault && story->last_child_connect) {
  story->child_wait_fault = 0; errno = ECHILD; return -1;
 }
 return result;
}

int story_accept4(int listener, struct sockaddr *address, socklen_t *length, int flags)
{
 char byte;
 int fd;
 (void)address; (void)length;
 story_check(listener == async_ready[0] && flags == SOCK_CLOEXEC, "real wait-pump accept edge");
 if (read(listener, &byte, 1) != 1 || async_accepted >= 4) { errno = EAGAIN; return -1; }
 fd = async_servers[async_accepted++];
 story_check(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0, "accepted descriptor CLOEXEC");
 return fd;
}
pid_t story_concurrent_begin(void)
{
 unsigned i;
 int pair[2], result;
 pid_t child;
 async_next = async_accepted = 0;
 for (i = 0; i < 4; i++) {
  story_check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "concurrent socketpair");
  async_clients[i] = pair[0]; async_servers[i] = pair[1]; peer_uid[pair[1]] = story->uid;
 }
 story_check(pipe(async_ready) == 0, "listener readiness pipe");
 story_daemon_listen(async_ready[0]);
 fflush(NULL); child = fork(); story_check(child >= 0, "concurrent client fork");
 if (child == 0) {
  char key[] = "scenario-password";
  char *list[] = {"net", "wifi", "list", NULL};
  char *profile[] = {"net", "wifi", "set-key", "scenario-B", key, "auto", NULL};
  char *disconnect[] = {"net", "wifi", "disconnect", NULL};
  char *disable[] = {"net", "wifi", "disable", NULL};
  close(async_ready[0]);
  for (i = 0; i < 4; i++) close(async_servers[i]);
  async_child = 1;
  for (i = 0; i < 100000; i++) {
   unsigned state = story->radios[0].state;
   if (state >= WLAN_STATE_AUTHENTICATING && state <= WLAN_STATE_FOUR_WAY) break;
   usleep(100);
  }
  story_check(i < 100000, "commands arrive during an actual child connection");
  result = story_net_command(3, list); story_check(result == 0, "concurrent list response"); story->concurrent_requests++;
  result = story_net_command(6, profile); story_check(result == 0, "concurrent profile notification"); story->concurrent_requests++;
  result = story_net_command(3, disconnect); story_check(result == 0, "concurrent disconnect recovery"); story->concurrent_requests++;
  result = story_net_command(3, disable); story_check(result == 0, "concurrent disable recovery"); story->concurrent_requests++;
  fflush(NULL); close(async_ready[1]); _exit(0);
 }
 close(async_ready[1]);
 for (i = 0; i < 4; i++) close(async_clients[i]);
 return child;
}
void story_concurrent_end(pid_t child)
{
 int status = 0;
 unsigned i;
 pid_t result = 0;
 for (i = 0; i < 10000 && result == 0; i++) {
  story_daemon_service(); result = waitpid(child, &status, WNOHANG);
  if (result == 0) usleep(1000);
 }
 story_check(result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "bounded concurrent client reap");
 story_check(story->concurrent_requests == 4, "all concurrent responses completed");
 story_daemon_listen(-1); close(async_ready[0]);
 story_check(async_accepted == 4, "all requests used real wait-pump ingress");
}
