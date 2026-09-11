# WS015 architecture review cases

Parent: [WS015](../ws.md)

The only current Phase is architectural discussion. These are design review
cases, not executable μITRON-conformance or timing tests. Executable fixtures
will be copied or created here only after their owning Phase is authorized.

`ws015-p001` is under manual hold `MB-007`. These cases remain the review
checklist but must not be treated as an active Queue or as implementation
acceptance until the user explicitly resumes the discussion.

| Case | Required design result |
| --- | --- |
| `RT-D001` | Exact μITRON edition/profile, compatibility claim, deviations, public types, and static-configuration model are explicit |
| `RT-D002` | RT/POSIX CPU masks, minimum topology, scheduler ownership, boot failure, and disabled-build behavior are deterministic |
| `RT-D003` | Every timer, IRQ, IPI, deferred-work, allocator, lock, TLB/cache, and logging path allowed on an RT core has a bounded or excluded contract |
| `RT-D004` | ELF admission covers code, data, BSS, TLS, stack, page tables, UAPI, objects, and communication memory without demand faults, reclaim, or swap |
| `RT-D005` | Prohibited RT operations and memory faults terminate/contain the RT task deterministically rather than entering an unbounded POSIX path |
| `RT-D006` | Intra-RT mailbox and RT/POSIX bridge payload, pointer, copy, ownership, capacity, ordering, timeout, and backpressure semantics are unambiguous |
| `RT-D007` | POSIX broker restart, duplicate/stale requests, unavailable service, and unbounded POSIX latency do not violate the declared RT contract |
| `RT-D008` | Explicit crash notification, heartbeat loss, resident emergency action, watchdog/reset, and excluded shared-kernel failures define an honest recovery boundary |
| `RT-D009` | A named board/configuration, workload envelope, clock, measurement method, maximum latency, run duration, and failure threshold define each microsecond claim |
| `RT-D010` | QEMU functional evidence, physical timing evidence, compatibility evidence, licensing/naming review, and WS ownership are separated |
