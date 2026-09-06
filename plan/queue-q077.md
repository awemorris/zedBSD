# Queue: retire legacy /diskN scaffolding, protect mounts and finish storage acceptance

Last updated: 2026-09-05

QID: `q077`

Queue status: finished

Queue finished: **Yes**

Parent: [master plan](master.md)

Previous Queue: [q076](queue-q076.md)

Authorization: the user explicitly requested Queue selection and execution
after reviewing ws018-p013's removal plan. This includes the proposed shared
ws019 runtime validation, not a new storage design. Standing permission allows
`git commit -m "WIP"` and push. Existing planning edits belong to this work.

Authorization extension: after KA-T121 reproduced a baseline mounted-directory
mutation defect, the user explicitly requested thorough audit and correction.
This selects ws018-p014, superseding the original stop on this namespace defect.
The original failing result is retained, not waived or attributed to cleanup.

The user's next follow-up explicitly broadens audit to the whole filesystem;
ws018-p015 records this additional audit within the same review/runtime budget.

Functional-correctness extension (2026-09-05): the user now explicitly requests
filesystem-wide functional audit **and correction**, including the previously
held UFS/overlay findings FS-A05--FS-A09. P015 owns these repairs and their
production-linked normal/sanitizer/fault/concurrency verification. Security
audit is not the requested deliverable. Existing runtime caps remain in force.

## Execution registry

| Priority | WS / Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws018-p013](ws018-kernel-architecture/phase013-legacy-disk-mount-residue/phase.md) | completed | Remove proven-unused synthetic rootfs, inode-only mount APIs/marker and six manifest references; preserve current mount/boot behavior |
| 1a | [ws018-p014](ws018-kernel-architecture/phase014-mounted-namespace-protection/phase.md) | completed | Audit and fix covered-directory mutation, aliases and namespace lifetime/transaction races; prerequisite for p013 protection gate and storage runtime |
| 1b | [ws018-p015](ws018-kernel-architecture/phase015-filesystem-wide-audit/phase.md) | completed | Filesystem-wide risk audit: core/driver/I/O/lifetime/permission boundaries, test coverage and explicit residual findings |
| 2 | [ws019-p010](ws019-installation/phase010-conservative-partition-reload/phase.md) | completed | Validation only: explicit ro/rw/root EBUSY and reboot; depends on p013 host/build gates |
| 3 | [ws019-p011](ws019-installation/phase011-userspace-partition-editing/phase.md) | completed | Validation only: mounted addition persists, exits 3, no live replacement, reboot discovers child |
| 4 | [ws019-p012](ws019-installation/phase012-explicit-auxiliary-mounts/phase.md) | completed | Validation only: explicit mounts work; no automatic auxiliary mounts before/after reboot |

## Shared finite budget and safety boundary

The authorized p014 extension sets a four-active-hour review window from its
start for audit, correction and shared validation (announced to the user).
At most two new amd64 QEMU launches total, each with 120-second boot and
600-second whole-cell bounds, using the corrected root-level `/q076` target
and the test-only waitpid observer. Record failures; a second launch requires
a diagnosed in-scope correction. Do not silently extend this window.

Repeat source/caller audits before deletion. Normal and ASan/UBSan host
regressions precede maintained amd64, i386 PC/AT and PC-98 `make -j16` builds.
Audit all six manifests, distinguishing this from non-x86 build/runtime claims.
Never use aggregate `make check` or repository `.internal/` material.

Only fresh disposable media/OVMF copies may be written. Preserve configured
boot/root/overlay/swap, live path-based mount/namei, ROOT/SWAPFILE/LOOPFILE
protections, and unconditional disk-wide EBUSY. No forced reload, real disks,
installer, formatting command, public nested-mount API expansion, non-x86 boot-policy
removal or unrelated cleanup. Test-fixture image construction remains allowed.

Namespace/lifetime defects directly implicated by mounted-directory mutation
are now in p014 scope; unrelated defects still require re-planning.
Q076 remains an immutable record of its four failed or
partially successful cells; a new PASS must supply its own acceptance evidence.

Functional continuation checkpoint (2026-09-05 22:54 JST): final normal/sanitizer
gates and all three maintained x86 builds pass. Shared runtime launch 1 of 2
is selected at `plan/ws019-installation/temp/q077-resume-01`, with
`--mount-protection`; result is pending, not acceptance. This remains within
the original four-active-hour window (the retained baseline was made at 21:07
JST, before the p014 extension); no window reset or extra launch is assumed.

## Final result — 2026-09-05

All selected Phases completed. [Q077 evidence](ws018-kernel-architecture/tests/q077-results.md)
records the functional fixes, limitations, normal/sanitizer results, all three
maintained x86 builds and two passing shared QEMU cells. The second cell uses
the final overlay rename correction; no third launch occurred. WS018 and WS019
P/W/M books are synchronized. The next formatter/installer work is outside
this finished Queue and was not executed. No commit or push was performed.
