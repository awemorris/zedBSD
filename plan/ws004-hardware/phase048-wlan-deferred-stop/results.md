# Q085 / P048 implementation and verification

Date: 2026-09-06. User-authorized review-3 design, implemented in the working
tree based on `a3f1ea3`. No commit, private credential access or RF experiment.

## Implemented contract

- `net/core.c` creates a dedicated WLAN retirement thread separately from packet
  processing. It visits pending stations every second; failed attempts back off
  for 1/2/4/8/16 seconds, capped at 16 seconds without abandoning the stop.
  Each callback retains a distinct station work pin. Station ownership already
  retains the net-device reference. Callbacks run outside common spinlocks and
  without an ordinary common active lease that would block their own barrier.
- AX211 and RTL8822BU publish stop intent before synchronous close. The same
  checked close helper runs on retry under the driver's lifecycle mutex. Open
  cannot reuse the epoch while pending or work-pinned. Detach cancels future
  claims and must join an existing claim; shutdown refuses premature release.
  A persistent hardware/producer failure remains pending with resources retained.
- STATUS uses a lifetime-protected read-only common snapshot even when ordinary
  station admission is closed. A pending stop reports DISCONNECTING, no forward
  administrative/controlled-port admission, and explicit stop flags/error.
  Reading status does not take an ordinary operation lease that delays stop.
- `wifi down` verifies the post-close WLAN snapshot. `networkd` retains known
  radio identity on a failed status read, exposes partial-list errors, checks
  stop completion, and retains RETIRING while a global disable is incomplete.
  It retries normalization without requiring another enable. Identity checks
  prevent an old retirement token from targeting a replacement with the same name.
- RTL8822B retains transport in STOPPING after failed stop, startup cleanup or
  channel cleanup. Forward operations are rejected; a later stop issues real
  register inverses. Only a successful checked stop clears the radio object.
  Adapter quarantine/recovery recognizes this state. Best-effort emergency RF
  writes alone no longer erase the information needed for checked teardown.

## UAPI compatibility

`wlan_status_request` remains 136 bytes; stop_flags is at offset 120 and stop_error
at 124, using the first two old reserved words. The remaining two reserved words,
request version and ioctl number are unchanged. These fields are output only and
are not validated as input. Existing zero-initialized requests remain valid;
amd64/i386 layout checks pass. An old kernel returns zero in the reserved area:
this preserves layout but does **not** supply the new checked-stop semantics to
a new client. Acceptance here uses the matched new kernel and userland contract.

## Defects exposed while verifying

1. The common scanner retained RUNNING after a busy final scan_stop, but its timer
   did not retry that final publish-pending state. Added the timed stop retry;
   completion is published only after the driver's actual successful inverse.
2. Clearing RTL's radio on a failed stop made a later stop return success without
   touching hardware. Retained STOPPING also closes the same gap in unsuccessful
   initialization/channel cleanup. The focused test injects a failed write,
   denies power/start/tune, then verifies additional stop writes without a new up.
3. Child pipe setup could lose the original fcntl error while closing descriptors.
   Preserve errno across cleanup; the child runner analyzer is now clean.
4. Older fixtures expected permanent AX211 quarantine after a successful checked
   stop, only one common close pass, or unconditional RTL OFF after an error.
   Updated those expectations while retaining failed-drain resource assertions.
   These are changed production contracts, not waived failures.

Initial failures remain in disposable `p048-rtl-core-before.log`,
`p048-rtl-core-final.log`, `p048-rtl-final2.log`, and
`p048-intel-ax211-pci{,-final,-final2,-final3,-final4}.log`. Final evidence follows.

## Focused runtime evidence

Logs below are under `plan/ws005-networking/temp/q085-wifi-scenarios/`.
The maintained runners execute production code with explicit external doubles.

| Runner under `plan/ws004-hardware/tests/` | Evidence / result |
| --- | --- |
| run-wlan-common-core-test.sh | `p048-core-final.log`: ordinary, ASan/UBSan, analyzer, amd64/i386 ABI PASS |
| run-usb-rtl8822bu-driver-test.sh | `p048-rtl-final3.log`: ordinary, ASan/UBSan, analyzer PASS |
| run-rtl8822b-core-test.sh | `p048-rtl-core-final2.log`: ordinary, ASan/UBSan, analyzer PASS |
| run-intel-ax211-wlan-common-integration-test.sh | `p048-ax-common.log`: ordinary, ASan/UBSan, analyzer and ABI PASS |
| run-intel-ax211-pci-test.sh | `p048-intel-ax211-pci-final5.log`: ordinary, ASan/UBSan, analyzer and syntax PASS |
| run-intel-ax211-runtime-start-test.sh | `p048-intel-ax211-runtime-start.log`: ordinary, ASan/UBSan, analyzer and syntax PASS |
| run-intel-ax211-boot-test.sh | `p048-intel-ax211-boot.log`: ordinary, ASan/UBSan, analyzer and syntax PASS |

Common tests cover retry without up, backoff, persistent error, read-only status
during pending work, blocking callback/detach/cancel join, shutdown while pending,
and station-slot reuse without stale callbacks. PCI tests run the actual AX211
adapter with a retained operation lease, then an IRQ-drain failure, then successful
independent stop and reopen. Common locking/work scheduling is tested in the
separate common fixture; PCI/MMIO/IRQ are controlled boundaries in the adapter
fixture. RTL tests run the actual USB adapter and low-level radio code with USB
and common-station boundary doubles.

## Integrated acceptance and builds

All 30 actual command/daemon/child stories pass ordinary and ASan/UBSan runs:
[individual outcomes and reproducible commands](../../ws005-networking/phase012-wifi-command-scenarios/results.md).
That harness also provides the practical command/daemon integration smoke using
real host socketpairs, authentication dispatch, framing, fork, pipes, poll,
signals and waitpid. It does not substitute a second managed-policy model.

Serialized `make -j16 ZEDBSD_CONFIG=config/ci/config-{amd64,pcat,pc98}.mk`
passes all three configurations. Logs: `p048-final-{amd64,pcat,pc98}.log`.
Sysroot headers were refreshed before the final builds. `git diff --check` passes.

No QEMU boot, physical RTL8822BU/AX211 association, AP observation, throughput,
or 2.4/5 GHz RF stability measurement was performed in this cycle. Deterministic
driver and command acceptance cannot establish those hardware properties.
The worker's one-second cadence and backoff are retry policy, not a guarantee
that faulty hardware or a caller that never returns will stop in finite time.
