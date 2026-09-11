# Reference

Status: current navigation index

Reference documents define stable or explicitly experimental commands,
configuration formats, headers, structures, constants, errors, and
permissions. Every compatibility claim must point to implementation and test
evidence; POSIX claims must agree with [WS001](../../plan/ws001/ws.md).

- [C interfaces and compatibility profile](compatibility-profile.md): feature
  selectors, ABI boundaries and conformance limits.
- [Console, graphics and system controls](control-devices.md): operations,
  ownership, permissions and the held GPU proposal.
- [Managed WLAN administration](managed-wlan.md) — current `net wifi`,
  credential-store, selection, state, recovery, and permission contract.
- [Network configuration console](network-console.md) — current candidate,
  atomic `commit`, temporary `commit confirmed`, and `rollback` contract.
- [Kernel boot parameters](kernel-boot-parameters.md) — current
  `boot0`–`boot3`, root-mode, `swap0`–`swap3`, and `init` contract for
  the four x86 production loaders, with the non-x86 NULL-source compatibility
  boundary.
- [evdev compatibility profile](evdev.md) — experimental UAPI with current
  input-core and USB HID producers; legacy console event/key-state UAPI has
  been removed while ordinary TTY and display operations remain.

## System administration

- [Init and service management](init-services.md): boot sequencing,
  `rc.conf`, `service.d`, FD 3 readiness, supervision, control, and shutdown.

- [x86 executable TLS](tls.md) — static PT_TLS, thread-pointer layout, pthread/fork lifetime, and dynamic-runtime boundary.

- [Atomic file publication](atomic-publication.md): no-replace rename, mv and directory durability.

- [Retained boot-source identities](boot-provenance.md): UEFI source selectors and configuration ambiguity.

- [Structured block and file command output](block-command-output.md): diskpart machine records, blkid export and stat formats.

- [Regular-file image formatters](image-formatters.md): UFS/ZEDSWAP2 initialization and read-only pristine verification.
