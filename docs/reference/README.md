# Reference

Status: active

Reference documents define stable or explicitly experimental commands,
configuration formats, headers, structures, constants, errors, and
permissions. Every compatibility claim must point to implementation and test
evidence; POSIX claims must agree with [WS001](../../plan/ws001-posix/ws.md).

- [evdev compatibility profile](evdev.md) — experimental UAPI and current
  input-core contract.
- [Kernel boot parameters](kernel-boot-parameters.md) — implemented
  `boot0`–`boot3`, root-mode, `swap0`–`swap3`, and `init` contract for
  the four x86 production loaders, with the non-x86 NULL-source compatibility
  boundary.

## System administration

- [Init and service management](init-services.md): boot sequencing,
  `rc.conf`, `service.d`, FD 3 readiness, supervision, control, and shutdown.
