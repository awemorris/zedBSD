# Queue: USB 1.1 concurrency, Intel Mac GPT repair, and checked recovery

Last updated: 2026-08-31

QID: `q047`

Queue status: in-progress

Queue finished: **No**

Authorization: the user selected Report Protocol, prohibited event-number
reuse while an old fd survives, required correct USB 1.1 operation rather than
an xHCI-only first release, and directed execution to continue. The standing
authorization permits finite follow-on Queues while deferring any newly found
human decision.

Timebox: none. P031 and p033 are complete: all focused, configured-production,
regression, repository-build, and shared forced HW-T25 QEMU gates pass. At that
buildable USB boundary, q047 consumed the maintainer's accepted Noct SHA;
runtime `--path`, NOCT-T084, and the zedbuild byte-primitives gate pass. Only
the unrelated NOCT-T082 `--compile --app --path=...` form remains uncleared.
WS020 p003 now passes one fresh uninterrupted six-cell `MAC-T020` matrix. Its
refreshed `MAC-T021` handoff artifact at hash `f811a0f5...` reached the Intel
Mac kernel but exposed a host-relocated GPT compatibility failure. P006 now
passes its host, relocated/pristine QEMU, exact-login, and repeated six-cell
automatic gates; the exact `692160cf...331d` artifact is published for one
non-blocking provisional Intel Mac boot. P031 and p006 automatic work release
p032.
The intervening `ws002-p022` console-login stall is also complete: a USB
submit-commit local-IRQ self-wait was captured and repaired, its deterministic
old-order regression passes, and ordinary initial plus final-five exact-login
boots release p006's login gate.

Parent: [master plan](master.md)

Previous Queue: [q046](queue-q046.md)

## Purpose

Remove the two general-USB prerequisites which block the already planned USB
HID producer Phase. P031 has made UHCI/EHCI concurrent and hotpluggable for USB
1.1 HID plus Storage. P032 remains to add a single checked endpoint-halt and
conservative direct-root reset contract shared by xHCI/UHCI/EHCI.
The p031 stress run also exposed an independent amd64 framebuffer-console
cursor race; p033 completed that repair using p031's final forced QEMU run.
At that buildable USB boundary, q047 consumed the maintainer's released Noct
`--path` repair. Runtime module loading, the ordinary production build, and
zedbuild byte primitives pass; only the unrelated compile/application CLI form
remains uncleared. The WS020 automatic Variant matrix and refreshed UEFI-only
handoff preflight pass. The first refreshed physical observation then exposed
a host-relocated physical-end GPT with a stale compact PMBR; p006 owns the
GPT-over-Protective-MBR precedence repair before the separately retained final
campaign.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p031` | [Phase](ws004-hardware/phase031-legacy-hcd-concurrent-hotplug/phase.md) | completed | UHCI/EHCI independent endpoints, request-local retirement, periodic/asynchronous progress, worker-context detach/reinsert, all focused/configured/regression/build gates, and both QEMU topologies pass |
| 2 | `ws004-p033` | [Phase](ws004-hardware/phase033-amd64-framebuffer-console-serialization/phase.md) | completed | One early-safe lock and strict cell/pixel bounds pass HW-T27 host gates and forced standalone/paired HW-T25 QEMU without console fault, corruption, or stall |
| 3 | `ws008-p010` | [Phase](ws008-noct/phase010-host-script-cli-contract-repair/phase.md) | uncleared only at unrelated `NOCT-T082`; accepted SHA, runtime `--path`, `NOCT-T084`, and zedbuild byte primitives pass | Pin the maintainer-accepted Noct repair, rebuild the host toolchain, and prove the production `--path`/`require` contract without a downstream Noct patch |
| 4 | `ws020-p003` | [Phase](ws020-intel-mac/phase003-qemu-acceptance/phase.md) | completed | One fresh uninterrupted `MAC-T020` matrix passes all six strict cells without weakening exact `login:` or negative absence/settle acceptance |
| 5 | `ws002-p022` | [Phase](ws002-services/phase022-intermittent-console-login/phase.md) | completed | USB submit-commit local-IRQ self-wait repaired; deterministic old-order gate, unchanged `MAC-T022`, and ordinary initial plus final-five exact-login boots pass without retry-to-pass |
| 6 | `ws020-p006` | [Phase](ws020-intel-mac/phase006-relocated-physical-gpt/phase.md) | automatic complete; one provisional physical boot pending externally | Valid GPT precedence and strict validation pass host/sanitizer/analyzer, first-only relocated/pristine QEMU, exact login, and uninterrupted six-cell gates; published SHA-256 `692160cf...331d`, UUID `A93F-BBBE` |
| 7 | `ws004-p032` | [Phase](ws004-hardware/phase032-usb-endpoint-device-recovery/phase.md) | in progress; p031 and p006 automatic prerequisites complete | The common core orders device-side clear-halt before HCD ring/toggle recovery, implements bounded direct-root reset, removes the Storage private-state hack, and passes HW-T26 |

## Fixed boundaries

- Do not implement the production USB HID/evdev driver in this Queue; that is
  `ws006-p008` after both prerequisites pass.
- Do not add a user-visible UAPI, HCD-name check, vendor quirk, hub/transaction-
  translator support, multi-independent-owner reset, or class reset callback.
- Preserve exact URB callback, cancellation, drain, DMA-retirement, PCI IRQ,
  shutdown, and fail-closed quarantine contracts from p009/p011/p015/p016.
- Keep `BUG-003` short-IN/error classification separate unless it becomes the
  direct blocker; then record and defer it rather than broadening q047.
- Keep p033 limited to the amd64 PC/AT output lock and cell/framebuffer bounds.
  The repeated PS/2 key producer, USB/HID ownership, other architectures, and
  public console API remain unchanged.
- Use dedicated WS004 fixtures, ordinary/sanitizer/analyzer modes, configured
  amd64/i386 builds, and disposable QEMU media. Do not run aggregate
  `make check`, consume `.internal/`, or modify Noct source.

## Completion definition

q047 finishes after every selected item is `completed`, `uncleared`, or left
`pending` solely because its earlier Queue dependency is uncleared. P033's
HW-T27 host gates and shared fresh QEMU evidence are complete. P031's complete
ownership/evidence result releases p032. At that buildable boundary, p010
proved the refreshed host Noct runtime and ordinary production path but remains
uncleared only at the unrelated NOCT-T082 compile/application form. P003 is
complete from one strict six-cell PASS. P022 is complete from its captured
USB submit-commit race, deterministic negative control, unchanged `MAC-T022`,
and ordinary initial plus five fresh-copy exact-login boots. P006 has cleared
its automatic host/relocated-media/pristine-media regression boundary and
published one exact artifact; its provisional Mac boot and p004 final
repetitions remain external.
Completing p032 makes `ws006-p008` implementation-ready but does not itself
claim USB HID, evdev hotplug, or physical keyboard/mouse acceptance. The
separately declared physical five-boot campaign still prevents whole-WS020
completion.
