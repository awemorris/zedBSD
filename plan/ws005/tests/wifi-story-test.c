/* Actual net -> ZNV2 -> networkd -> fork/pipe -> wifi acceptance stories. */
#include "wifi-story.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned current_story;
static unsigned steps;

static void command(const char *words, int success)
{
 char bytes[256];
 char *argv[10], *word, *save;
 int argc = 0, result, saved_stdout;
 FILE *output;
 uint64_t before = story->now;
 snprintf(bytes, sizeof(bytes), "net wifi %s", words);
 for (word = strtok_r(bytes, " ", &save); word; word = strtok_r(NULL, " ", &save)) argv[argc++] = word;
 argv[argc] = NULL;
 fflush(NULL);
 output = tmpfile(); story_check(output != NULL, "command output file");
 saved_stdout = dup(1); story_check(saved_stdout >= 0, "save stdout");
 story_check(dup2(fileno(output), 1) == 1, "capture stdout");
 result = story_net_command(argc, argv);
 fflush(stdout); dup2(saved_stdout, 1); close(saved_stdout);
 rewind(output); memset(story_output, 0, sizeof(story_output));
 fread(story_output, 1, sizeof(story_output) - 1, output); fclose(output);
 steps++;
 if ((result == 0) != success) {
  fprintf(stderr, "story %02u step %u command [%s] result=%d state=%d output=%s\n",
   current_story, steps, words, result, story_daemon_state(), story_output);
  abort();
 }
 story_check(story->now - before <= 105000000ULL, "whole command deadline");
 story_check(story->max_links <= 1, "at most one managed L2 link");
}
static void tick(void)
{
 fflush(NULL); story->now += 6000000ULL; story_daemon_tick();
 story_check(story->max_links <= 1, "background single connection");
}
static void connected(const char *name)
{
 unsigned attempt;
 for (attempt = 0; attempt < 8 && story_daemon_state() != NETWORKD_WLAN_CONNECTED; attempt++) tick();
 if (story_daemon_state() != NETWORKD_WLAN_CONNECTED)
  fprintf(stderr, "story %02u failed convergence state=%d dhcp=%u\n", current_story, story_daemon_state(), story->dhcp_count);
 story_check(story_daemon_state() == NETWORKD_WLAN_CONNECTED, "connected policy");
 story_check(story_daemon_l3(), "connected L3 token");
 story_check(strcmp(story_daemon_interface(), name) == 0, "selected interface");
}
static void reset(unsigned id)
{
 unsigned i;
 story_daemon_reset(); memset(story, 0, sizeof(*story));
 current_story = id; steps = 0; story->now = 1000000ULL; story->uid = 1000;
 for (i = 0; i < STORY_RADIOS; i++) {
  story->radios[i].present = 1; story->radios[i].visible = 1;
  story->radios[i].index = i + 10; story->radios[i].scan = WLAN_SCAN_IDLE;
 }
}
static void key(const char *name, int automatic)
{
 char words[128];
 snprintf(words, sizeof(words), "set-key %s scenario-password%s", name, automatic ? " auto" : "");
 command(words, 1);
}
static void state(int expected)
{
 if (story_daemon_state() != expected)
  fprintf(stderr, "story %02u state=%d expected=%d\n", current_story, story_daemon_state(), expected);
 story_check(story_daemon_state() == expected, "policy state");
}
static void normal_start(void)
{
 key("scenario-A", 1); command("enable", 1); connected("wlan0");
}
static void finish(void)
{
 unsigned i;
 command("disable", 1); state(NETWORKD_WLAN_DISABLED);
 story_check(!story_daemon_l3(), "no retained L3 token after disable");
 for (i = 0; i < STORY_RADIOS; i++) if (story->radios[i].present)
  story_check(!story->radios[i].up && !story->radios[i].stop_pending &&
   story->radios[i].state != WLAN_STATE_CONNECTED, "all radios physically stopped");
}
static void run_story(unsigned id)
{
 unsigned before;
 uint64_t selection_started;
 pid_t child;
 reset(id);
 switch (id) {
 case 1:
  normal_start(); command("list", 1); command("disconnect", 1);
  command("connect scenario-A", 1); finish();
  story_check(story->dhcp_count == 2, "one DHCP acquisition per connection"); break;
 case 2:
  command("enable", 1); key("scenario-A", 1); connected("wlan0"); command("list", 1); finish(); break;
 case 3:
  normal_start(); before = story->dhcp_count; command("enable", 1); command("list", 1);
  story_check(story->dhcp_count == before, "repeated enable preserves L3"); finish(); break;
 case 4:
  command("enable", 1); command("enable", 1); key("scenario-A", 1); connected("wlan0"); finish(); break;
 case 5:
  command("enable", 1); command("set-key scenario-A wrong-password auto", 1); tick();
  story_check(story_daemon_state() != NETWORKD_WLAN_CONNECTED, "wrong key is not connected");
  key("scenario-A", 1); connected("wlan0"); finish(); break;
 case 6:
  normal_start(); command("disconnect", 1); key("scenario-B", 1); tick();
  state(NETWORKD_WLAN_MANUAL_DISCONNECTED); command("list", 1); command("connect scenario-B", 1); finish(); break;
 case 7:
  key("scenario-A", 1); tick(); command("list", 1); state(NETWORKD_WLAN_DISABLED);
  story_check(story->dhcp_count == 0 && !story->radios[0].up, "profile update preserves disabled radios");
  command("enable", 1); connected("wlan0"); finish(); break;
 case 8:
  command("connect scenario-A", 0); key("scenario-A", 0); command("enable", 1);
  command("connect scenario-A", 1); connected("wlan0"); finish(); break;
 case 9:
  normal_start(); before = story->dhcp_count; command("connect unknown", 0); command("list", 1);
  state(NETWORKD_WLAN_CONNECTED); story_check(story->dhcp_count == before, "unknown target preserves connection"); finish(); break;
 case 10:
  normal_start(); command("disconnect", 1); command("disconnect", 1); command("enable", 1); connected("wlan0"); finish(); break;
 case 11:
  command("enable", 1); command("disable", 1); command("disable", 1); command("enable", 1);
  story->stop_fault = 1; command("disable", 0); state(NETWORKD_WLAN_RETIRING);
  command("list", 0); story_check(strstr(story_output, "stop-pending=1") != NULL, "pending hardware stop is observable");
  command("enable", 0); story->stop_fault = 0;
  story->radios[0].stop_pending = story->radios[1].stop_pending = 0;
  story->radios[0].state = story->radios[1].state = WLAN_STATE_DOWN;
  story->radios[0].scan = story->radios[1].scan = WLAN_SCAN_CANCELLED;
  tick(); state(NETWORKD_WLAN_DISABLED); command("enable", 1); finish(); break;
 case 12:
  key("scenario-A", 0); command("enable", 1); tick(); command("list", 1);
  state(NETWORKD_WLAN_AUTO_SEARCHING); story_check(story->dhcp_count == 0, "manual profile is not automatic");
  command("connect scenario-A", 1); finish(); break;
 case 13:
  normal_start(); story->uid = 2000; story->invalid_store = 1;
  command("enable", 0); story_check(story_daemon_owner() == 1000, "invalid takeover preserves owner");
  command("list", 1); state(NETWORKD_WLAN_CONNECTED); story->uid = 1000; finish(); break;
 case 14:
  normal_start(); story->uid = 2000; command("disconnect", 0); command("connect scenario-A", 0);
  key("scenario-B", 1); tick(); command("list", 1); state(NETWORKD_WLAN_CONNECTED);
  story_check(story_daemon_owner() == 1000, "other owner cannot take policy"); story->uid = 1000; finish(); break;
 case 15:
  story->radios[0].present = story->radios[1].present = 0; command("enable", 1); key("scenario-A", 1); tick();
  state(NETWORKD_WLAN_AUTO_SEARCHING); story->radios[0].present = 1; connected("wlan0"); command("list", 1); finish(); break;
 case 16:
  story->radios[0].up_error = EIO; key("scenario-A", 1); command("enable", 1); connected("wlan1"); command("list", 1); finish(); break;
 case 17:
  story->radios[0].scan_error = EIO; key("scenario-A", 1); command("enable", 1); connected("wlan1"); command("list", 1); finish(); break;
 case 18:
  command("enable", 1); command("list", 1); story->radios[0].list_error = EIO;
  command("list", 0); story_check(strstr(story_output, "interface=wlan1") != NULL, "partial healthy cache remains visible");
  story->radios[0].list_error = 0; command("list", 1);
  story->radios[0].status_error = EIO; command("list", 0);
  story_check(strstr(story_output, "interface=wlan0 stop-pending=0 status-error=") != NULL, "known radio remains visible on status failure");
  story->radios[0].status_error = 0; command("list", 1); finish(); break;
 case 19:
  story->radios[0].malformed = 1; key("scenario-A", 1); command("enable", 1); connected("wlan1");
  command("list", 0); story->radios[0].malformed = 0; finish(); break;
 case 20:
  story->radios[0].scan_delay = story->radios[1].scan_delay = 2;
  key("scenario-A", 1); command("enable", 1); command("list", 1);
  connected("wlan0"); finish(); break;
 case 21:
  key("scenario-A", 0); command("enable", 1); story->radios[0].rapid_scan = 20;
  command("connect scenario-A", 1); story_check(story->radios[0].rapid_scan == 0, "rapid retry workload consumed");
  command("list", 1); finish(); break;
 case 22:
  story->eof_exit_delay = 1; command("enable", 1); command("list", 1);
  story->child_deadline = 1; command("list", 0); story->child_deadline = 0;
  command("list", 1); finish(); break;
 case 23:
  normal_start(); story->radios[0].state = WLAN_STATE_IDLE; story_daemon_event(10, 0, 0);
  state(NETWORKD_WLAN_RECONNECTING); command("connect scenario-A", 1); finish(); break;
 case 24:
  normal_start(); story->radios[0].address = 0x3300000a; strcpy(story->resolver, "nameserver 10.0.0.99\n");
  story->route_present = 0; command("disconnect", 1);
  story_check(story->radios[0].address == 0x3300000a && story->resolver_present, "external L3 replacement survives retirement");
  command("enable", 1); connected("wlan0"); finish(); break;
 case 25:
  normal_start(); story->radios[0].disconnect_error = EBUSY; command("disconnect", 0); state(NETWORKD_WLAN_RETIRING);
  before = story->dhcp_count; story_daemon_event(10, 0, 0); tick();
  story_check(story->dhcp_count == before, "retirement suppresses reconnect");
  story->radios[0].disconnect_error = 0; command("disconnect", 1); command("enable", 1); connected("wlan0"); finish(); break;
 case 26:
  key("scenario-A", 0); command("enable", 1); story->dhcp_fail = 1; command("connect scenario-A", 0);
  story_check(!story_daemon_l3() && story->radios[0].state != WLAN_STATE_CONNECTED, "failed DHCP retires L2/L3");
  story->dhcp_fail = 0; command("connect scenario-A", 1); finish(); break;
 case 27:
  key("scenario-A", 1); key("scenario-B", 1); command("enable", 1);
  story->radios[0].connect_error = EIO; story->radios[0].disconnect_error = EBUSY; tick();
  state(NETWORKD_WLAN_RETIRING); story_check(story->radios[1].connections == 0, "failed cleanup blocks second candidate");
  command("disable", 0); story->radios[0].connect_error = story->radios[0].disconnect_error = 0;
  command("disable", 1); command("enable", 1); connected("wlan0"); finish(); break;
 case 28:
  normal_start(); story->radios[0].present = 0; story_daemon_event(10, 1, 0);
  story->radios[0].index = 110; story->radios[0].present = 1; story->radios[0].state = WLAN_STATE_IDLE;
  story->radios[0].address = 0x4400000a; command("list", 1); command("disable", 1);
  story_check(story->radios[0].address == 0x4400000a, "old identity cannot clear replacement L3");
  command("enable", 1); connected("wlan0"); finish(); break;
 case 29:
  key("scenario-A", 1); command("enable", 1); story->radios[0].connect_delay = 100;
  child = story_concurrent_begin(); tick(); story_concurrent_end(child);
  state(NETWORKD_WLAN_DISABLED); steps += story->concurrent_requests;
  story->radios[0].connect_delay = 0; command("enable", 1); connected("wlan0"); finish(); break;
 case 30:
  key("scenario-A", 0); command("enable", 1); story->child_wait_fault = 1;
  command("connect scenario-A", 0); story_check(story->child_wait_fault == 0, "lost wait result injected on connect");
  command("connect scenario-A", 1); finish(); break;
 case 31:
  /* The scan finishes during the idle interval after selection timed out. */
  story->radios[1].present = 0;
  story->radios[0].scan_delay = 100000;
  key("scenario-A", 1); command("enable", 1); tick();
  state(NETWORKD_WLAN_AUTO_SEARCHING);
  story->radios[0].scan = WLAN_SCAN_COMPLETE;
  story->radios[0].snapshot = story->radios[0].generation;
  tick();
  state(NETWORKD_WLAN_CONNECTED);
  story_check(story->radios[0].scans == 1, "late completed scan must be consumed before restarting");
  finish(); break;
 case 32:
  /* Consuming an empty snapshot must still allow a later fresh scan. */
  story->radios[1].present = 0;
  story->radios[0].scan_delay = 100000;
  story->radios[0].visible = 0;
  key("scenario-A", 1); command("enable", 1); tick();
  story->radios[0].scan = WLAN_SCAN_COMPLETE;
  story->radios[0].snapshot = story->radios[0].generation;
  tick(); state(NETWORKD_WLAN_AUTO_SEARCHING);
  story->radios[0].visible = 1;
  story->radios[0].scan_delay = 0;
  tick(); state(NETWORKD_WLAN_CONNECTED);
  story_check(story->radios[0].scans == 2, "consumed empty scan must refresh for a newly visible AP");
  finish(); break;
 case 33:
  story->radios[0].scan_delay = 100000;
  key("scenario-A", 1); command("enable", 1); selection_started = story->now;
  connected("wlan1");
  story_check(story->now - selection_started < 10000000ULL, "ready later radio must not wait for slow earlier radio");
  finish(); break;
 case 34:
  story->radios[1].scan_delay = 100000;
  key("scenario-A", 1); command("enable", 1); selection_started = story->now;
  connected("wlan0");
  story_check(story->now - selection_started < 10000000ULL, "ready first radio must not wait for slow later radio");
  finish(); break;
 case 35:
  story->radios[0].scan_delay = 100000;
  key("scenario-A", 0); command("enable", 1); selection_started = story->now;
  command("connect scenario-A", 1); connected("wlan1");
  story_check(story->now - selection_started < 5000000ULL, "manual target uses a completed radio without ordering wait");
  finish(); break;
 case 36:
  story->radios[0].connect_error = EIO;
  story->radios[1].scan_delay = 3;
  key("scenario-A", 1); command("enable", 1); connected("wlan1");
  story_check(story->radios[0].connection_attempts != 0, "early failing candidate was exercised");
  finish(); break;

 }
 printf("story %02u PASS steps=%u simulated-us=%llu\n", id, steps, (unsigned long long)story->now);
 fflush(stdout);
}
int main(void)
{
 unsigned id, only = 0;
 unsigned fd, before = 0, after = 0;
 for (fd = 0; fd < 256; fd++) before += fcntl(fd, F_GETFD) != -1;
 story = mmap(NULL, sizeof(*story), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
 story_check(story != MAP_FAILED, "shared radio boundary");
 if (getenv("STORY_ID")) only = (unsigned)atoi(getenv("STORY_ID"));
 for (id = 1; id <= 36; id++) if (!only || only == id) run_story(id);
 story_daemon_reset(); munmap(story, sizeof(*story));
 for (fd = 0; fd < 256; fd++) after += fcntl(fd, F_GETFD) != -1;
 story_check(after == before, "all parent pipe/socket/temporary descriptors retired");
 return 0;
}
