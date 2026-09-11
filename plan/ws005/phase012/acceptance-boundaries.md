# Post-rewrite acceptance boundary design

This is the verification stage after the user's requested structural repair.
Do not substitute 30 isolated state-object assertions for the 30 command stories.

Use separate host fixture translation units for production net Wi-Fi dispatch,
networkd request/policy dispatch, wifi command execution, the Wi-Fi child runner,
and shared OS/radio state. Include production sources with only OS-edge adapters.

- Invoke the actual `wifi_command` and `wifi_set_key_command` dispatch targets
  from net/main.c for the six public forms. Retain actual
  wifi-conf parsing/validation/serialization, ZNV2 request framing and response
  decoding. The credential store adapter uses synthetic per-UID models; the
  separate existing store-publication fixtures cover real file publication.
- Use real socketpairs for client/daemon frames. The synchronous client shutdown
  adapter services its prepared daemon endpoint, then the client reads the real
  framed response. Authenticate synthetic immutable peers at getsockopt's OS
  edge; invoke the real daemon authentication and request handlers.
- Run the real wifi child runner with real host pipes, fork, poll, signals and
  waitpid. Replace its exec boundary with the actual wifi command entrypoint
  compiled in another TU. Honor CLOEXEC when replacing exec in-process, retain
  real FD 4 credential delivery, and exit with the actual command status. Do not
  hand-author successful WIFI1 streams or replace their parser.
- Allocate radio observations and simulated monotonic time in shared mmap memory
  so child ioctl effects survive fork. Model only interface/WLAN ioctls, scan
  state versus retained snapshot generation, auth outcomes and finite RF delay.
- Link actual netutil helpers through the same ioctl adapter where practical.
  At networkd's DHCP process boundary, inject process completion/wait errors and
  IPv4/route/resolver changes; keep actual ownership planning and retirement.
  DHCP also uses an actual child process, with its executable replaced at the
  boundary by the configured L3 effect and exit status.
- For concurrent-control stories, fork a real fixture net client on another
  socketpair. A readiness pipe represents the daemon listener and accept returns
  that prepared endpoint. Delay notification until the shared radio shows an
  in-flight connection, so list/profile/disable exercise the real wait callback
  and deferred-dispatch path. Include actual EOF-to-exit runner windows.

Every story asserts each command's exit/result, fixed simulated deadline and
single-link invariant, with policy/owner/interface and L2/L3 checkpoints and a
clean recovery endpoint. Async enable
requires a bounded observe/tick step before a story assumes connected state;
immediate stop after enable is also a valid ordering. Record all 30 outcomes
individually, then run sanitizers and relevant maintained regressions.

Host radio doubles are not real RTL8822BU/AX211 RF acceptance. Retain that limit
in the results rather than describing simulated associations as hardware tests.
