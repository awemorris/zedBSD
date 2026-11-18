# Desktop thread balancer

Status: current for `CONFIG_KERN_DESKTOP_BALANCER`

The desktop thread balancer is an optional part of the kernel that helps the
scheduler keep a desktop responsive on a machine whose CPUs are not all alike
(performance and efficiency cores).  It watches what threads do and whether a
person is using the machine, and tells the scheduler which kind of core and
which priority level each user thread would rather have.  It is not a
scheduler: where and when a thread runs stays the scheduler's choice, and a
request the scheduler cannot honour at that moment is refused and asked again
at a later look.

## 1. Enabling it

| Setting | Meaning |
| --- | --- |
| `CONFIG_KERN_DESKTOP_BALANCER=y` (build) | Builds the balancer.  The default is `n`; a kernel built with `n` has no balancer and behaves as it did without one. |
| `desktop_balancer=off` (boot line) | In a kernel that has the balancer, keeps its thread from starting.  The scheduler then leaves every thread where and at the level it would without the balancer, and the report below still exists. |

The kinds of core come from the kernel's thermal and CPU-performance report.
That needs `CONFIG_KERN_THERMAL`, and on x86 also
`CONFIG_DRIVER_X86_POWER_CONTROL`.  Without them every CPU is of one kind and
only the priority levels apply; no thread is moved between kinds.

## 2. What it decides

The balancer looks at every user thread about ten times a second while a
person uses the machine or did in the last minute, and once a second while
nobody does.  It gives each thread the first rule that matches:

| Rule | Thread | Kind of core |
| --- | --- | --- |
| nice below zero | the process's nice value is negative | fast |
| nice above zero | the process's nice value is positive | efficient |
| busy | computing for 200 ms or more in user mode without a system call | efficient |
| presenter | its process put an image on a display in the last second | fast, raised |
| GPU | its process put work on a GPU in the last second | fast, raised |
| terminal | its process has a controlling terminal and someone is at the machine | fast, only where the whole core is free |
| other | everything else | none |

A thread that uses half or more of the CPU over the last looks is heavy and is
not raised above the default level, so that it cannot hold other threads back.
A nice value also moves the priority level a step or two, but no user thread
goes above the kernel's default level, and a user thread that waits too long
behind higher levels runs one quantum at a higher level so that it is not
starved.  A thread is moved at most once in half a second.

## 3. The report

`kern.desktop_balancer` (`CTL_KERN`, `KERN_DESKTOP_BALANCER`, read only)
returns `struct desktop_balancer_info` of `<uapi/sysctl.h>`:

- `flags`: `KERN_DESKTOP_BALANCER_BUILT` (the kernel has it),
  `KERN_DESKTOP_BALANCER_RUNNING` (its thread runs, which is not so after
  `desktop_balancer=off`) and `KERN_DESKTOP_BALANCER_HYBRID` (the CPUs are of
  two kinds).
- `state`: what the person is doing: away, quiet or active.
- `passes`, `pass_ns_max`, `pass_ns_total`: how many looks, and the longest and
  total time of them.
- `rule_threads`: how many threads each rule took at the last look.
- `hints`, `moves`, `refused`, `handoffs`, `handoff_retries`,
  `handoff_failures`, `starved`, `kernel_held`: what the scheduler did with the
  hints; `places` and `places_refused`: the moves the balancer asked for and
  the scheduler took or refused.
- `cpu_count` and `cpus`: each CPU's kind, whether it may be used, its speed
  with its SMT sibling idle and busy, and its core.

A program checks `version` and `struct_size` before it reads further.  A
kernel without the balancer answers the request with an error.
