# q150 documentation reconciliation

Date: 2026-09-09
Status: completed for p007; p008 remains uncleared

Published [kernel/HAL architecture](../../../docs/architecture/kernel-and-hal.md),
[compatibility profile](../../../docs/reference/compatibility-profile.md),
[control devices](../../../docs/reference/control-devices.md) and
[boot/storage guide](../../../docs/howto/boot-and-storage.md). Updated managed
WLAN against p035/q124, including available-candidate selection, either radio
order, enable-before-set-key, lease-preserving repeated enable, nonblocking
scan snapshots and dhcpc /dev/console reporting.

Source/header inspection distinguishes ABI-dependent user pointers, literal
feature declarations from certification, open-description graphics ownership,
PID-1 shutdown authority, per-query visibility and mount snapshots from
reservations. Graphics mmap and GPU acceleration are not claimed. DOC-54
remains producer-held; direct I/O, UAS transport and full installation remain
unfinished. The root compatibility marker is now documented as the current
`zedBSD ufs root v1\n`, not the removed UFS1 marker.

Repaired consolidated swap/system and input/HID source references. Current
product navigation reaches the new documents. Existing historical queue and
test evidence was retained, including q015/q032 root-mode acceptance,
q128 TLS, q139 NVMe, q141 USB shutdown, q142 resource accounting, q147 real
graphical terminal and q148/q149 formatter/publication tests.

Verification:

```sh
build/NoctLang/build-static/noct \
  plan/ws009-documentation/tests/check-relative-links.noct docs
git diff --check -- docs plan/ws009-documentation
```

Product check passed 283 relative links; whitespace check passed. An independent
host inventory of all docs Markdown also checked relative-target existence.
No production code changed and no new runtime, physical or clean-build claim
is made. p007 closes DOC-10/11/12/50/51/52/55 at this source boundary;
future producer changes must update their contracts. p008's native preparation
and public NVMe installation recipe still require WS019-p004/p005 and renewed
end-to-end guide acceptance, so WS009 remains active.
