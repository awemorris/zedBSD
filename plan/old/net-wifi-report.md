# `net wifi` static instability review

Date: 2026-09-06
Scope: `userland/base/net`, `userland/base/networkd`, `userland/base/wifi`
Method: source-only review of the three-process path
`net` → ZNV2 socket → `networkd` → `fork`/`exec` `/sbin/wifi` → WLAN ioctls.
No code was changed and nothing was executed; this is a pre-scenario-test
prediction document.

## 0. Summary

The individual functions are, with two exceptions, internally correct. The
instability does not live in any single statement, which is why a
statement-local static checker sees nothing. It lives in three
**cross-file numeric contracts** that nobody owns:

1. **Timeout budget.** Every client-side deadline in `net` is smaller than the
   worst case that `networkd` can legitimately spend serving that same opcode.
   Every one of the five `net wifi` subcommands is affected. This is provable
   by arithmetic on constants alone (§1).
2. **Child record budget.** `wifi --machine connect` emits an *unbounded*
   number of progress records, while `networkd` hard-fails the child at 64
   records. Whether a connect survives depends on how many scan cycles fit in
   30 seconds, i.e. on RF conditions (§2).
3. **Child reaping.** `poll_child()` has no wakeup source for child exit. When
   both child pipes reach EOF before the child becomes reapable, `poll()`
   degenerates into a plain sleep for the entire remaining stage timeout (§3).

Plus two state-machine holes that produce hard, non-obvious failures (§4, §5).

Ranked by how likely each is to explain "sometimes it works, sometimes it
does not":

| # | Finding | Severity | Determinism |
| --- | --- | --- | --- |
| 1 | Client/server timeout budgets are mutually inconsistent for all five subcommands | High | Deterministic once the slow path is entered |
| 2 | `wifi connect` progress records are unbounded vs. the 64-record child cap | High | Timing/RF dependent — the classic flake |
| 3 | `poll_child()` sleeps the whole stage timeout on the EOF-before-reap race | High | Race; amplified by hundreds of child spawns per connect |
| 4 | `RECONNECTING` state is not handled by `WIFI_CONNECT` → `EBUSY` | Medium | Race with a `CARRIER_DOWN` route event |
| 5 | A degraded L3 cleanup permanently locks out `disable`/`disconnect`/`enable` | Medium | Deterministic once entered; unrecoverable without a restart |
| 6 | `wifi list` aborts on the first bad radio | Low | Deterministic |
| 7 | `run_command_until()` reports success when `waitpid()` fails | Low | Rare |
| 8 | Automatic work and recovery run before `poll()`, blocking `accept()` | Medium | Amplifies 1 |

---

## 1. The client deadline is smaller than the server's own worst case

`net` sets `SO_RCVTIMEO` per opcode in
[`backend_exchange_result()`](../../userland/base/net/main.c:840), chosen in
[`wifi_backend()`](../../userland/base/net/main.c:1437):

* `WIFI_ENABLE`, `WIFI_CONNECT` → 100 s
* `WIFI_LIST`, `WIFI_DISABLE`, `WIFI_DISCONNECT` → 15 s
* `WIFI_PROFILES_CHANGED` → 5 s (errors suppressed)

`networkd` never learns these numbers. Its own budget is built from
independent constants in `userland/base/networkd/main.c:53-58` and from the
per-child `10U` / `5U` timeouts hardcoded at each `run_wifi()` call site.
Worst cases, for `N` discovered WLAN radios:

| Subcommand | `net` waits | `networkd` worst case (N=1) | Overrun |
| --- | --- | --- | --- |
| `wifi enable` | 100 s | retire 10 + prepare 20N + `connect_automatic` 90 + 4× failure-retire 40 ≈ **160 s** | 60 s |
| `wifi connect` | 100 s | retire 10 + prepare 20N + select 30 + connect 35 + dhcp 10 + retire 10 ≈ **115 s** | 15 s |
| `wifi disable` | 15 s | retire 10 + `stop_wlan_radios` 30N ≈ **40 s** | 25 s |
| `wifi disconnect` | 15 s | retire 10 + `prepare_wlan_radios` 20N ≈ **30 s** | 15 s |
| `wifi list` | 15 s | 10 s per radio → **10N s** | N ≥ 2 |
| `set-key` notify | 5 s | may queue behind a 90 s automatic cycle | always possible |

Derivations:

* `prepare_wlan_radios()` runs `wifi up` and `wifi search-start` per radio,
  each with a 10 s child timeout
  (`userland/base/networkd/main.c:3040`, `:3056`).
* `stop_wlan_radios()` runs `wifi disconnect`, `wifi search-stop` and
  `wifi down` per radio, each 10 s (`:3103`, `:3107`, `:3111`).
* `connect_automatic()` is given `now + 90 s`
  (`:2240`, and `:1056` for the background path), but the per-attempt
  `retire_managed_connection()` and `stop_losing_scans()` calls run *outside*
  that deadline, so the function can overrun it by 10 s per attempt
  (`NETWORKD_WLAN_ATTEMPTS` is 4).
* `WIFI_CONNECT` uses `now + 30 s` for `select_manual_radio()` (`:2204`) and a
  *fresh* `now + 60 s` for `run_managed_connect()` (`:2219`) — the two are
  additive, not a shared budget.

Observable symptom: `net: networkd is unavailable: Connection timed out`
(from the `unavailable:` label in
[`backend_exchange_result()`](../../userland/base/net/main.c:888)) even though the
operation is still running and will succeed. The user retries, the daemon is
still busy with the first request, the retry also fails — and then a later
`net wifi list` shows the state actually did change. That reads exactly as
"unstable".

`wifi disable` is the cleanest reproduction: 15 s client vs. ≥40 s server for a
single radio, with no RF dependency at all.

**Suggested direction.** Derive the client deadline from the server's declared
stage budget instead of hardcoding it: publish one table of per-opcode stage
budgets in `protocol.h`, have `networkd` bound its own work by it, and have
`net` use that value plus one transport margin. Alternatively, make long WLAN
operations asynchronous (`NETWORKD_RESULT_IN_PROGRESS` is already defined in
`protocol.h:80` and never used anywhere) and let `net` poll.

## 2. `wifi connect` can exceed the 64-record child cap

`networkd_wifi_child_run()` fails the child if it ever emits more than
`NETWORKD_WIFI_CHILD_RECORD_MAX` (64) newline-terminated records:

```c
/* userland/base/networkd/wifi-child.c:826-833 */
for (index = 0U; index < (size_t)count; index++)
        if (temporary[index] == '\n')
                result->output_records++;
if (copied != (size_t)count || result->output_records >
    NETWORKD_WIFI_CHILD_RECORD_MAX)
        function_result = EOVERFLOW;
```

`EOVERFLOW` is not a truncation — `poll_child()` breaks out, `terminate_child()`
SIGTERMs the child, and `run_wifi()` returns failure.

For `wifi list` the budget is exact and deliberate: `wifi` clamps machine-mode
output to `WIFI_MACHINE_BSS_MAX` = 62 BSS records
(`userland/base/wifi/main.c:37,705`), plus one scan record plus one terminal
record = exactly 64. Good.

For `wifi connect` **there is no such clamp.** In `connect_command()` most
progress records are one-shot (`start_announced`, `busy_announced`,
`retry_announced`, and the `displayed_states` bitmask), and the scan countdown
in `wait_for_scan()` is de-duplicated by `displayed_scan_seconds`, so it is
capped at ~30 records for the 30 s `WIFI_CONNECT_SECONDS` window. But this one
is not gated at all:

```c
/* userland/base/wifi/main.c:1076-1083 — inside the retry for(;;) */
if (!wifi_quiet &&
    ((wifi_machine &&
    printf("WIFI1 connect state=selecting generation=0 "
    "error=0\n") < 0) || ...
```

It fires once per completed scan cycle, and the outer `for (;;)` re-issues
`SIOCSWLANCONNECT` and re-scans until the 30 s deadline. So the record count is

```
2 (starting + terminal)
+ up to 3 one-shot progress records
+ up to 3 state-transition records
+ up to 30 scan-countdown records
+ K   ("selecting", one per scan cycle)
+ 1   (print_failure_status on the failure exit)
```

which overflows 64 for roughly `K > 28` when scans are ~1 s, or `K > 56` when
the cache is already fresh and no countdown record is emitted. `K` is purely a
function of how fast the driver completes a scan and how often the target BSS
is absent from the cache — i.e. of RF conditions. This is a strong candidate
for the reported flakiness, and it interacts badly with `BUG-009` (marginal
RTL8822BU link budget): the weaker the signal, the more scan cycles, the more
likely the connect is killed by its own supervisor rather than by a real
failure. The reported error is then an opaque `EOVERFLOW`/`EILSEQ`, not
"SSID not found".

**Suggested direction.** Either gate `state=selecting` behind a
`select_announced` flag (matching the other three), or make the record cap a
function of the stage timeout, or count only *distinct* record kinds. A regression
fixture that feeds a synthetic 100-record `WIFI1` stream through
`networkd_wifi_child_run()` would pin this permanently.

## 3. `poll_child()` has no wakeup source for child exit

```c
/* userland/base/networkd/wifi-child.c:989-1033 (condensed) */
while (!state->child_reaped || state->output_pipe[0] >= 0 ||
    state->diagnostic_pipe[0] >= 0) {
        append_output(...);        /* closes output_pipe[0] on EOF   */
        append_diagnostic(...);    /* closes diagnostic_pipe[0] on EOF */
        write_secret(...);
        reap_child(state, WNOHANG);
        ...
        descriptors[0].fd = state->secret_pipe[1];      /* may be -1 */
        descriptors[1].fd = state->output_pipe[0];      /* may be -1 */
        descriptors[2].fd = state->diagnostic_pipe[0];  /* may be -1 */
        poll_result = poll(descriptors, 3U, milliseconds);
}
```

The loop exits only when the child is reaped **and** both pipes are closed.
There is a reachable state where all three descriptors are `-1` but
`child_reaped` is still 0: both pipes hit EOF, and the immediately following
`waitpid(WNOHANG)` returns 0 because the child has closed its descriptors but
has not yet reached zombie state.

In that state `poll()` is called with three negative descriptors. POSIX says a
negative `fd` is ignored and `revents` is zeroed, so `poll()` simply sleeps for
`milliseconds` — which is *the entire remaining stage timeout*. Nothing wakes
it: `SIGCHLD` is left at `SIG_DFL` in `networkd` (only `SIGHUP`, `SIGPIPE`,
`SIGTERM`, `SIGINT` are installed, `userland/base/networkd/main.c:216-219`), so
no handler exists to interrupt `poll()` with `EINTR`.

Consequences:

* A `wifi up` / `search-start` / `disconnect` / `search-stop` / `down` that
  should take a few milliseconds instead takes its full 10 s.
* A `wifi list` inside `select_manual_radio()` / `select_profile_radio()` takes
  its full 5 s, burning one sixth of the 30 s selection budget in one shot.
  Those loops poll every 100 ms, so a single `net wifi connect` spawns dozens
  to hundreds of children — the race gets many chances per command.
* The deterministic version of this: when `execv()` fails (missing or
  unexecutable `/sbin/wifi`) the child explicitly `close()`s stdout and stderr
  and *then* `_exit(127)`
  (`userland/base/networkd/wifi-child.c:737-743`, and
  `exit_child_setup_failure()` at `:647-662`). The parent is guaranteed to see
  both EOFs strictly before the child can be reaped, so the "fail fast, ENOENT"
  path costs a full 10 s per radio instead of milliseconds.

Combined with §1 this is the amplifier: it converts operations whose nominal
cost is a couple of ioctls into operations that consume their entire declared
timeout, which is what pushes the aggregate past the client deadline.

**Suggested direction.** Once both pipes are closed there is nothing left to
drain, so a blocking `reap_child(state, 0)` is safe and correct. Otherwise
clamp the poll interval when `nfds` is effectively zero, or use a self-pipe /
`ppoll` with `SIGCHLD` unblocked.

## 4. `WIFI_CONNECT` does not handle the `RECONNECTING` state

[`handle_wifi_request()`](../../userland/base/networkd/main.c:1898) drains queued
route events *first*, with `allow_recovery = 0`:

```c
if (route_events >= 0 && process_route_events(0) != 0) { ... }
```

A pending `RTM_IFINFO_CARRIER_DOWN` for the connected radio makes
`networkd_managed_wlan_event()` set the state to `NETWORKD_WLAN_RECONNECTING`
(`userland/base/networkd/managed-wlan.c:402-406`), and because
`allow_recovery == 0`, `process_route_event()` returns without running
recovery — the state stays `RECONNECTING` with the connection record intact.

The connect path then only retires when the state is exactly `CONNECTED`:

```c
/* userland/base/networkd/main.c:2134-2137 */
if (request->header.opcode == NETWORKD_OP_WIFI_CONNECT &&
    managed_wlan.state == NETWORKD_WLAN_CONNECTED &&
    retire_managed_connection(NETWORKD_WLAN_MANUAL_DISCONNECTED, 1) != 0) {
```

So with `RECONNECTING` no retire happens, and
`networkd_managed_wlan_begin_connect()` rejects the request because
`connection_active()` is still true → `EBUSY`
(`managed-wlan.c:150-153`). `net wifi connect SSID` fails with
`Device or resource busy` for no reason the user can see, and succeeds if
retried a moment later — after the main loop's post-request
`recover_managed_wlan()` (`main.c:301`) has resolved the state.

Note the ordering: recovery runs *after* `handle_request()`, but the request
handler is what consumes the events that create the need for recovery.

**Suggested direction.** Either drain route events and settle recovery before
dispatching a WLAN request, or make the `WIFI_CONNECT` retire condition
`state != AUTO_SEARCHING && state != MANUAL_DISCONNECTED` rather than
`state == CONNECTED`.

## 5. A degraded L3 cleanup permanently locks out every WLAN command

`networkd_managed_wlan_plan_l3_cleanup()` is deliberately fail-closed: if the
live address/netmask/broadcast, the default route, or the byte contents of the
resolver file no longer match the ownership token recorded at connect time, it
sets `degraded` and returns `ESTALE`
(`userland/base/networkd/managed-wlan.c:246-344`). `clear_interface_l3()`
propagates that as `-1`, and `retire_managed_connection()` then returns `-1`
**without clearing the connection**:

```c
/* userland/base/networkd/main.c:1204-1207 */
if (cleanup_error != 0) {
        errno = cleanup_error;
        return -1;
}
```

Everything that must retire first now fails permanently:

* `wifi disable` → `NETWORKD_RESULT_DEGRADED` / `ESTALE`, Wi-Fi stays enabled.
* `wifi disconnect` → same.
* `wifi enable` → fails at "retire prior Wi-Fi owner".
* `recover_managed_wlan()` → `retire_managed_connection(...) != 0` returns
  early (`main.c:959-961`), so `schedule_automatic_work()` is never called
  and the background retry stops for good.

The trigger is ordinary: a DHCP renewal that changes the lease, anything that
rewrites `/etc/resolv.conf`, or an operator `ifconfig`. Once hit, the only exit
is restarting `networkd`. From the user's seat this is "`net wifi` stopped
working and nothing I type helps".

**Suggested direction.** Distinguish "cannot safely remove someone else's
resource" from "cannot retire the connection". A degraded cleanup should
release ownership, report `DEGRADED` once, and still move the state machine
forward, rather than pinning the daemon in an unusable state.

## 6–8. Lower severity

**6. `wifi list` aborts on the first failing radio.**
[`main.c:1910`](../../userland/base/networkd/main.c:1910) uses
`for (radio_index = 0U; result == 0 && ...)`, so one radio that fails `run_wifi`
makes the whole `net wifi list` fail — even though `prepare_wlan_radios()`
(`:3022-3070`) deliberately tolerates per-radio failure. Inconsistent policy
between two loops over the same array. On a machine with one good and one
flaky radio, `net wifi list` will look intermittent.

The same inconsistency exists inside `select_profile_radio()`: a `run_wifi`
failure marks the radio terminal and continues (`:3310-3315`), but a
`networkd_wifi_child_parse_list()` failure aborts the *entire* automatic
connect cycle (`:3333-3336`).

**7. `run_command_until()` can report success for an unreaped child.**

```c
/* userland/base/networkd/main.c:4229-4340 (condensed) */
status = 0;
...
else if (result < 0 && errno != EINTR)
        child_done = 1;      /* status is still 0 */
...
if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { ... return -1; }
return 0;                    /* WIFEXITED(0) is true, WEXITSTATUS(0) is 0 */
```

A `waitpid()` failure is reported as a successful DHCP run. Separately, the
deadline-driven timeout sets `errno = ETIMEDOUT` and then unconditionally
overwrites it with `EIO` at the `WIFEXITED` check, and the `ticks > tick_limit`
guard does not fire on the deadline path — so a DHCP timeout is reported as a
generic I/O error.

**8. Background work runs before `poll()`.**
The main loop calls `run_due_work()` at the top of every iteration
(`main.c:262`), before `poll()`. `run_automatic_work()` can occupy the process
for up to 90 s (`:1056`), and `recover_managed_wlan()` up to ~35 s, while the
listening socket has a backlog of 8 (`:768`). Because `AUTO_SEARCHING`
reschedules every `NETWORKD_WLAN_RESCAN_SECONDS` (5 s), a system that is
enabled but cannot associate spends most of its wall clock inside a blocking
automatic cycle and only briefly in `accept()`. Whether an interactive
`net wifi list` succeeds then depends on which 5-second window the user typed
in — a textbook source of "sometimes it works".

`WIFI_LIST` is also not owner-gated (it returns before the `owner_allowed()`
check at `:1941`), so any member of group `network` can hold the single-threaded
control loop for `10 × N` seconds per call.

### Hygiene notes (no behavioural claim)

* `networkd_protocol_read_frame()` reads the frame header one byte at a time
  (`userland/base/net/protocol.c:305-318`). `SO_RCVTIMEO` applies per `read()`,
  so the nominal 15 s / 100 s client budget is really "per byte" and the true
  bound is up to 32× that.
* `write_secret()`'s comment says it "clears the local secret immediately after
  delivery" (`wifi-child.c:935`) but no clear is performed there; the secret is
  retained deliberately for the `contains_bytes()` redaction check in
  `finish_child()`. Comment and code disagree.
* Stack depth on the connect path is roughly 180–200 KB
  (`handle_request` ~37 KB + `handle_wifi_request` ~75 KB +
  `run_managed_connect` ~35 KB + `append_wifi_output`'s 32 KB `plain[]`),
  driven by three separate 32 KB buffers. Fine on an 8 MB stack, worth knowing
  if `networkd` ever runs with a reduced limit.
* `NETWORKD_RESULT_NO_CANDIDATE` and `NETWORKD_RESULT_IN_PROGRESS`
  (`protocol.h:79-80`) are declared and never produced or consumed.

---

## 9. What the scenario tests should target

Ordered so that a failure discriminates between the findings above.

1. **`net wifi disable` with one radio, from a connected state.**
   Predicted: `net: networkd is unavailable` at ~15 s, while `networkd`
   completes the disable ~25 s later. Confirms §1 with no RF dependency.
   Follow with `net wifi list` — it should report `state=disabled`, proving the
   client timed out on work that actually succeeded.
2. **`net wifi list` on a two-radio machine.** Predicted client timeout at
   15 s against a 20 s server path. Same class as 1, different constant.
3. **Rename or `chmod -x /sbin/wifi`, then `net wifi list`.** Predicted: 10 s
   per radio instead of an immediate `ENOENT`. This is the deterministic probe
   for §3.
4. **`net wifi connect` against a weak/marginal AP (the `BUG-009` room
   position), 20+ repetitions.** Count the `WIFI1` records the child emitted.
   Predicted: intermittent failure with `EOVERFLOW`/`EILSEQ` rather than a
   sensible "SSID not visible", correlating with the number of scan cycles.
   That is §2.
5. **`net wifi enable` with a saved profile whose AP is powered off.**
   Predicted: client gives up at 100 s while `connect_automatic` runs its four
   attempts. §1 on the long path.
6. **Down the AP to force `CARRIER_DOWN`, then immediately
   `net wifi connect SSID`.** Predicted: intermittent `EBUSY` from §4, which
   disappears on retry.
7. **Connect, then change the lease or rewrite `/etc/resolv.conf`, then
   `net wifi disable`.** Predicted: `ESTALE` and a permanently stuck daemon
   (§5). Verify that no subsequent `net wifi` command can recover.
8. **`net wifi list` in a loop while `net wifi enable` is auto-searching with
   no reachable profile.** Predicted: alternating success and
   "networkd is unavailable" as the 5 s rescan cycle blocks `accept()` (§8).

For 2, 4 and 8, capture `networkd`'s stderr — the per-stage diagnostics it
already prints (`managed-cleanup stage=… child-error=… child-records=…`,
`main.c:1194-1200`) will distinguish §2 (`child-records` near 64) from §3
(full-timeout stages) from §5 (`stage=l3 error=116`).

## 10. What static analysis alone cannot settle

* The actual distribution of `K` (scan cycles per connect) in §2 — only the
  hardware and RF environment decide whether the 64-record cap is crossed.
* How often the EOF-before-reap window in §3 is lost in practice on this
  scheduler; the structural defect is certain, the hit rate is not.
* Whether the kernel WLAN driver can emit scan states that make
  `select_manual_radio()` never reach a terminal snapshot within 30 s, which
  would produce a different "SSID not visible" path from the same symptom.
