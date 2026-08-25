# ws006-p004 QEMU evdev evidence

Date: 2026-08-25

The configured amd64 image was booted with `qemu-system-x86_64 -machine pc`.
After logging in as root, the guest opened the production keyboard node while
the same PS/2 keyboard remained attached to the console:

```sh
dd if=/dev/input/event0 bs=48 count=1 | od
```

QEMU monitor `sendkey c` produced two LP64 records (48 bytes). The first had
`type=EV_KEY`, `code=46` (`KEY_C`), and `value=1`; the second had
`type=EV_SYN`, `code=SYN_REPORT`, and `value=0`. The console simultaneously
echoed `c`, and the shell prompt returned after the reader exited. Boot reached
`init: system running` and `login:` before the check.

This is production guest evidence: `/dev/input/event0` was registered by the
console keyboard producer, not by a test-only fake device. Focused host tests
separately cover release, repeat, independent reader cursors, overflow, and
the 32/64-bit record layouts.
