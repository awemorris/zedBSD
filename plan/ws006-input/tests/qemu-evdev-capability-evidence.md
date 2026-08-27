# WS006 IN-T12 capability-discovery evidence

Last updated: 2026-08-28

Phase: [`ws006-p005`](../phase005-evdev-capability-state/phase.md)

## Final run

Command:

```sh
plan/ws006-input/tests/qemu-evdev-capability.sh
```

Result: **PASS**

- QEMU: `qemu-system-x86_64` 10.0.11, `-machine pc`, 512 MiB, four CPUs.
- Dedicated image SHA-256:
  `94182c13ed220f22de19916fb99251f62d3111db7ad93e2222027f19d3d8b8f4`.
- `config.mk` SHA-256 before and after:
  `3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6`.
- Guest probe SHA-256:
  `4de566d232c6e913e6b6c7aca2bd38fc86dc748a4caf7b8d1a00836d9c97fb93`.
- The disposable guest reached normal init/login and the root shell.
- Capability-only discovery found exactly one keyboard and one relative
  pointer. It did not use a fixed event number, device name, identity, or
  product ID to assign either role.
- Both nodes passed zero, short, exact, and oversized bitmap copy tests,
  oversized zero-fill, `EVIOCGKEY`, and `ENOTTY`/no-mutation rejection for a
  malformed direction, unsupported event type, and unsupported ABS axis.
- The final logical guest log contained no `IN-T12 FAIL`, kernel panic, fault,
  VFS failure, I/O error, or segmentation fault marker.

The decisive records were:

```text
IN-T12 caps path=/dev/input/event0 ev=0,1 ... roles=keyboard boundaries=pass
IN-T12 caps path=/dev/input/event1 ev=0,1,2 ... rel=0,1 roles=relative-pointer boundaries=pass
IN-T12 PASS devices=2 keyboards=1 relative-pointers=1
```

## Harness findings before the final run

The first execution exposed a missing `?` key translation in the QEMU monitor
controller; that was a runner defect and was corrected. A later execution saw
the PS/2 mouse backend transiently return `ENODEV` while QEMU monitor keyboard
release events were still draining. The probe now waits for injected input to
settle and uses five bounded one-second open attempts. It does not spin or
silently ignore a device that remains unavailable.

## Focused host and build evidence

- IN-T11 strict capability/state fixture: PASS.
- IN-T11 ASan/UBSan fixture: PASS.
- IN-T00 LP64 and ILP32 syntax/layout fixtures: PASS.
- IN-T10 independent queue fixture: PASS.
- IN-T20 keymap fixture: PASS.
- `make -j16 build/amd64/vmunix`: PASS (`amd64 vmunix check: PASS`).
- `make -j16`: PASS.
- `git diff --check`: PASS.

`make check` and `.internal/` were not used.
