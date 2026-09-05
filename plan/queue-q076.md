# Queue: userspace partition administration and conservative reload

Last updated: 2026-09-05

QID: `q076`

Queue status: finished

Queue finished: **Yes** — two completed items; three uncleared runtime gates

Authorization: on 2026-09-05 the user approved userspace GPT/MBR editing and
conservative kernel reload, then explicitly requested Phase detail, Queue
selection and execution. This supersedes the unexecuted read-only proposal.
Standing permission permits `WIP` commits and pushes.

Parent: [master plan](master.md)

Previous Queue: [q075](queue-q075.md)

## Execution registry

| Priority | WS / Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws019-p002](ws019-installation/phase002-readonly-block-gpt-administration/phase.md) | completed | Basic disk/mount UAPI and 512/4096 raw I/O; host, ABI, build and guest queries passed |
| 2 | [ws019-p010](ws019-installation/phase010-conservative-partition-reload/phase.md) | uncleared | Reload implemented; host exclusion/fault tests and guest idle replacement passed; final explicit rw/ro/root/reboot acceptance pending |
| 3 | [ws019-p003](ws019-installation/phase003-diskpart-readonly/phase.md) | completed | Userspace GPT/MBR inspection; parser/CLI, build and guest list/show passed |
| 4 | [ws019-p011](ws019-installation/phase011-userspace-partition-editing/phase.md) | uncleared | Writer/notification implemented; host fault/CLI and guest GPT/MBR round-trips passed; mounted-add/reboot acceptance pending |
| 5 | [ws019-p012](ws019-installation/phase012-explicit-auxiliary-mounts/phase.md) | uncleared | Auto-mount loop removed and absence verified at login with overlay/swap; explicit mount/reboot regression remains |

## Execution boundary

Review p002/p003 after two active hours each, p010/p011 after four each.
This twelve-hour review ceiling is not a promise to consume the budget.
The timebox and scope were presented before code work. If unrelated lifecycle,
loader or filesystem redesign is necessary, record uncleared and return to
planning; do not execute dependent work without its prerequisites.

Host production-linked ordinary/sanitizer/fault/ABI gates precede maintained
amd64/i386 builds with `make -j16`; never run aggregate `make check`.
At most two final amd64 QEMU cells, each with 120-second boot / 600-second
whole-cell bounds: idle auxiliary disk, then mounted/root busy and reboot.
Use only explicit disposable image copies. Preserve failed cells; repeat only
after a diagnosed correction within this budget.

Additional authorization: during execution the user requested the auxiliary
auto-mount correction in p012. Its 30-minute review timebox and at most two
additional integrated QEMU launches were presented before implementation.
The original two launches are retained as failed evidence: `q076-idle-01`
encountered a legitimately busy auto-mounted disk (and a second unsupported
NVMe controller); `q076-combined-02` confirmed EBUSY but the harness incorrectly
read `$?` on a new shell input line. Neither established writer acceptance.
The fresh p012 window uses one NVMe plus IDE auxiliary media and a guest-only
waitpid observer, without changing production shell behavior.

## Result and resume boundary

[Recorded evidence and exact remaining gates](ws019-installation/tests/q076-results.md).
Both added launches are retained: `q076-explicit-03` injected its helper rootfs
into the ESP instead of the configured payload (harness error);
`q076-explicit-04` passed query/idle GPT+MBR editing but attempted `/mnt/q076`,
which the existing root-level-only mount API rejects with EINVAL. No kernel
panic or writer corruption was observed. The failed cell's GPT and MBR images
each equal their original complete images after add/delete.

The runner now resolves the unique existing payload and uses `/q076` for the
explicit mount. Do not expand mount pathname support or weaken EBUSY here.
No fifth launch was attempted. A newly approved finite Queue must rerun the
corrected combined cell (and at most one diagnosed correction) to clear
p010/p011/p012. Current implementation is WIP, not fully storage-acceptance
cleared. Installer/formatters have not been executed.

No physical disks, production filesystem formatting, whole-disk initialization, partition
data moves/resizes, extended-MBR editing, installer or boot-variable writes,
or forced reload. Installer-v1 remains non-table-writing. Its source
provenance is not part of the mount/basic-disk query ABI.
Creating FAT solely inside fresh disposable test fixtures is not a production
formatting command or permission to format existing media.
