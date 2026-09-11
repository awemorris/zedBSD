# Q085 orchestration rewrite

The six public forms and the direct wifi ioctl/machine interface are retained.
The daemon remains a single policy owner; RF and DHCP work must not create a
second independent policy engine.

1. Separate request dispatch, profile preflight, policy transitions, radio
   preparation, selection, connection/DHCP, and retirement. One request context
   owns response and credential storage and emits exactly one final response.
2. `enable` establishes or reasserts policy and schedules automatic work. Its
   success means enabled; connection completion is observed through `list`.
   Saving an automatic key after enable schedules the same worker. Repeating
   enable on a healthy connection does not restart L2 or DHCP.
3. Explicit connect validates its exact profile and topology before retiring a
   live connection. Once admitted it suppresses automatic selection until its
   bounded attempt succeeds or reaches the manual idle state.
4. Disconnect records manual pause before teardown. Disable only publishes
   disabled after all required normalization succeeds. Interrupted/failed
   teardown retains a retryable ownership token and cannot be RF-recovered.
5. One work context defines total deadline, useful-work interval, reserved
   teardown interval, background/foreground role, and cancellation. Primitive
   timers are subordinate to that context; repeated stages cannot renew it.
6. The child wait loop services control readiness. Lists observe cached scan
   snapshots with their age without cancelling RF work; profile notifications
   remain responsive. Automatic and RF recovery yield for authorized stop
   requests, retire in-flight L2 work, then the outer loop dispatches the retained
   request. Reasserting the same enabled policy is harmless. A
   foreground conflict receives a correlated busy result rather than a silent
   transport timeout. Wired SHOW is a read-only observation; authorized wired
   mutations also make background Wi-Fi yield, then enter the same ordinary
   dispatch and confirmed-transaction checks. Incomplete requests have four
   bounded nonblocking ingress slots and an absolute five-second deadline.
   No recursive state-changing request dispatch.
7. A token distinguishes owned resources, externally replaced resources, and
   failed OS mutations. Successful partial cleanup updates the token. Replaced
   resources are preserved and their obsolete claims released; real cleanup
   errors retain the remaining claims for retry. The pre-DHCP baseline remains
   attached to the connection until a verifiable snapshot establishes the
   resulting ownership, including partial mutations after DHCP failure.
   Oversized resolver contents are explicitly present but unowned, without
   preventing IPv4/route snapshots. Persistent retirement I/O failures back off
   from five to sixty seconds; retry count never justifies losing ownership.
8. Radio failures are local to that radio. Lists return healthy radio output
   alongside explicit partial errors; selection does not treat malformed or
   failed radio snapshots as candidates. Direct list remains nonblocking.
   One scan wave freezes candidate order; four attempts per wave share its
   deadline. Subsequent waves advance past failed candidates so a failing
   prefix cannot starve later profiles. Profile changes reset that cursor.

Response waits are shared with the client: enable/connect allow 90 seconds of
daemon work and other radio requests 30 seconds, each with 15 seconds of client
transport margin. Profile notifications allow 2 seconds plus that margin.
Radio transactions reserve their final 10 seconds for cleanup. Snapshot
observation during active work does not launch a nested radio operation.
Deferred control shortens background work to at most the ten-second cleanup
tail. Wired client response budgets include fifteen seconds for yielding;
confirmed DISARM retains its 250 ms transport behavior.

Fresh and cached list results share snapshot age/availability metadata. A cache
miss or eviction is explicit, and output exhaustion preserves complete records
plus a truncation marker and final policy state. Cached record allocations share
a total 32,748-byte bound; the observation path does not place a full policy
request or intermediate record-conversion array on the active child's stack.

The 30 saved command stories are acceptance after implementation. Focused
compilation and primitive checks are engineering checks, not acceptance claims.
