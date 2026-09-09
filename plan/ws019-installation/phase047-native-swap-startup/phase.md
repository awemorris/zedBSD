# ws019-p047: native root file swap startup

Status: completed q180; see [results](results.md)
Parent: [WS019](../ws.md), p007 prerequisite
Timebox: 120 active minutes

Use the conventional /etc/fstab swap entry and existing /sbin/swapon command
after init mounts filesystems. Extend swapon with -a, and make mount -a skip
swap entries. No new installed helper command and no unsupported FAT boot-slot
syntax for native /swapfile. Preserve explicit swapon SOURCE behavior.

Share a bounded fstab record reader between mount and swapon rather than two
divergent parsers. Preserve existing four-column files, accept conventional
optional numeric dump/pass columns, comments and escaped path whitespace.
Reject truncated/overlong/malformed records with line diagnostics. Swap options
are defaults/sw, noauto, nofail; unsupported options fail explicitly. -a skips
noauto and non-swap entries, tolerates already-active sources, reports other
errors and attempts independent remaining entries. nofail only suppresses an
absent device/file, not arbitrary I/O errors. Empty table succeeds without
opening /dev/system. Do not implement unrelated swap priority/discard settings.

Init invokes swapon -a after mount -a and before services, with checked fork,
EINTR-aware wait and truthful failure diagnostics. Activation failure leaves
console/service recovery available. Default fstab has no swap entries, so
existing FAT boot-swap images do not attempt duplicate activation.

Host tests: parsing boundaries/escapes/columns, option refusal, duplicate and
mixed success/failure, noauto/nofail, open/ioctl/close failures; existing explicit
swap command regressions. Three builds. QEMU disposable native root fixture
with root copied from the immutable source and UFS /swapfile: source-free boot
must show active swap, repeated swapon -a is harmless, halt drains cleanly;
bad/missing entry reports failure without losing login. Full installer UI,
copy transaction, publication and paging stress remain p006/p007 acceptance.

Before executing, confirm fixture native root boot and init package inputs,
record the finite queue under standing autonomous authorization, then implement.
