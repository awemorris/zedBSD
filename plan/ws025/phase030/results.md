# ws025-p030 results

## 2026-09-10 現行状態

uncleared / 現行実装確認済み（67b28ce0）。現行動作・残る比較証拠は未確認。
既定4000、上限検査、MMIO設定、IRQ統計が残ることを静的確認。新規実装は追加せず、既存証拠と現行差分を照合する。
今回の確認はソース読取りのみ。詳細は[修正後照合](../post-rollback-review.md)。

## q143 QEMU comparison (2026-09-09)

Implemented `tests/run-imod-qemu.py` and the private USB/HID runner's validated
`USB_HID_XHCI_IMOD` override. Each cell uses a separate build/image. QMP discovers
the xHCI PCI BAR, reads RTSOFF, then reads interrupter-zero IMOD. Observed low
16 bits match the requested interval for **0, 160 and 4000**, and all three
USB/HID campaigns pass.

Evidence: `../temp/q143/result.json`, `i*/metadata.txt`, `i*/results.tsv`,
`i*-readback.json`, and `i*/xhci/guest.log`. Each includes ordinary USB-root
boot, capability discovery, keyboard-to-console routing, relative/absolute
events, stale-descriptor hotplug generations, and concurrent 64 MiB root-device
reads with pointer events. All private builds use `make -j16`. Shell/Python
syntax and relevant `git diff --check` pass. Production xHCI source and user
config hashes remain unchanged; default remains 4000.

Wall time in JSON includes builds and scripted key delays: it is **not**
device latency or a CPU comparison. The raw read probe checks successful byte
counts, not write persistence. No WLAN, write/fsync, IRQ/s, CPU, percentile
latency or new terminal-halt measurements were made in this queue. p030 remains
uncleared for those cells. Next software step: add a bounded native write/fsync
and readback workload with actual counters/timestamps to these same isolated
artifacts. Physical tuning remains separate; lack of hardware does not prevent
that implementation.

## Historical q125 result

Status: uncleared
Date: 2026-09-09

## 確認した事実

HEAD 34a1f6dの現ソースとq122/p024の既存証拠を確認した。
現行IMOD=4000。0/160/4000の実機比較が採用条件で、QEMUの同値結果は代用不可。現在テスト機SSHがtimeout。

## 未完了と再開

実機比較の実行環境と複合topologyを取得できず、既定値選択に必要なデータがない。

比較可能な実機topologyと遠隔または直接起動経路。

production実装・新規性能測定を行ったとは主張しない。既存挙動と既定値を維持した。

## q189 native storage measurements (2026-09-10)

QEMU cells 0, 160 and 4000 passed actual IMOD register readback, all 64
64-KiB write/fsync/exact-readback samples, and HID completion during the
paced workload. Current production xHCI and workload hashes agree across
accepted cells: `../temp/q189-accepted/result.json`. Values 0/160 come from
q189-comparison; 4000 from q189-4000-final. Each private image/build and
command log is retained. No production source/default/config change.

| IMOD | Clock resolution | write p50/p95/p99 ms | fsync p50/p95/p99 ms | readback p50/p95/p99 ms | Process CPU ms |
| --- | --- | --- | --- | --- | --- |
| 0 | 10 ms | 0/0/20 | 10/20/20 | 0/10/20 | 860.0 |
| 160 | 10 ms | 0/0/30 | 10/10/20 | 0/10/30 | 800.0 |
| 4000 | 10 ms | 0/0/20 | 10/10/10 | 0/10/20 | 780.0 |

Percentiles use nearest rank over 64 samples. CLOCK_MONOTONIC advertises
10-ms resolution; zero duration means below this clock's resolution, not
zero cost. Cached file readback is not a raw-device read latency. Process
CPU is getrusage tick-accounted user+system time for the workload only. It
is neither IRQ CPU nor machine utilization. Pacing/console output are
outside per-operation times but inside elapsed/process accounting. No
physical optimal value follows from these QEMU numbers. Default stays 4000.

The pilot completed I/O but its collector rejected samples preceded by
console input echo; q189-comparison's first 4000 run likewise completed I/O
but the start wait rejected a shell-prompt prefix. Both are preserved failed
harness runs. Marker matching now permits such prefixes while still requiring
exact ordered 0–63 samples, one success record and actual HID overlap. Only
4000 was rerun after the wait fix; successful 0/160 data was preserved.

Phase disposition: uncleared, q189 finished. This cycle completes the planned
write/fsync/readback measurement increment. Remaining software cells include
driver IRQ/rate and broader CPU instrumentation, high-speed storage and WLAN
concurrency, and explicit recovery/shutdown correlation for this workload.
Physical controller latency/tuning remains unmeasured. Next implementation
can add bounded driver counters and topology cells without waiting for real
hardware; physical results alone can justify physical default tuning.

## q246: IRQ collector implemented, native measurement deferred

Three aggregate xHCI counters distinguish handler entry, EINT/FATAL ownership and
consumed ring events. The paced workload reports deltas and monotonic interval;
collector derives driver callback rates. Independently sampled counters can skew
at boundaries; no delta-entry >= delta-owned invariant is asserted.
I/O stats v11; hal.h untouched. Default IMOD remains 4000.
amd64 disk-image `/tmp/zedbsd-q246-amd64.log` exited 0. Guest compilation and
collector positive/negative checks passed (`../temp/q246/parser.log`).
No native IRQ measurement yet; no PCAT/PC98 build in this queue. User approval
of the HAL proposal takes priority; resume 4000 then 0/160 cells after migration.
p030 remains uncleared.

## q255: native default-4000 IRQ interval

Private IMOD 4000 campaign PASS: actual register readback 0x0fa0, USB storage/HID
functional campaign and source/config integrity. Evidence
../temp/q255-imod/{result.json,i4000-result.json,i4000-readback.json}, plus
i4000/xhci/guest.log and i4000/metadata.txt. All build/runtime processes terminal.
Private build used; ordinary production artifacts/default were not replaced.

The 64 paced 64-KiB write/fsync/exact-readback samples pass with HID overlap.
Observation: 16.5 seconds, 1181 handler entries, 1181 owned entries, 1181 events;
71.5758 handler entries/second. Guest process CPU 750000 us. These are aggregate
xHCI callback counters and paced workload CPU, not hardware IRQ service time.
Raw per-operation timings and percentiles are retained in i4000-result.json.

q246 instrumentation now has native positive-activity evidence. Still uncleared:
compare 0/160 with the same current instrumentation, then address remaining
USB2/WLAN and recovery coverage. Physical-controller optimization is unmeasured;
no reason to change default 4000 from this single QEMU cell.

## q256: current 0/160/4000 IRQ comparison

Private 0 and 160 campaigns PASS, with actual interval readback, all HID/USB
checks, 64 confirmed write/fsync/readback samples and positive IRQ counters.
q255 and q256 xHCI source and workload hashes match. Evidence
../temp/q256-imod/{result.json,i0-result.json,i160-result.json,comparison.json}.
Production source/config integrity passes; private builds only, all jobs terminal.

| IMOD | Handler entries | Interval seconds | Entries/s | Guest process CPU seconds |
| --- | --- | --- | --- | --- |
| 0 | 1178 | 16.47 | 71.524 | 0.71 |
| 160 | 1178 | 16.48 | 71.481 | 0.72 |
| 4000 (q255) | 1181 | 16.50 | 71.576 | 0.75 |

Observed rates are nearly equal under this paced QEMU workload. The 10-ms guest
clock and single campaign per setting do not establish a physical latency or CPU
optimum. Keep default 4000. IRQ/rate instrumentation and current three-setting
comparison are now covered. Remaining p030 work: USB2/WLAN topology coverage,
recovery gates and physical comparison as applicable; do not mark these passed
from the current one-controller USB/HID campaign. Phase remains uncleared.

## q257: USB2 xHCI / IMOD 4000

Added validated test-only USB_HID_XHCI_USB2_ONLY and comparator --usb2. QEMU
xHCI uses p2=4,p3=0; default test topology unchanged. The actual root port reset
PORTSC=0x00000e03 verifies speed ID 3 (USB2 high speed), and IMOD readback is 4000.
No production driver or HAL API changes. Shell syntax and native campaign pass.

Evidence ../temp/q257-usb2/i4000-result.json and guest.log: full USB/HID campaign,
64 confirmed write/fsync/readback samples and HID overlap pass. 1178 handler
entries/owned/events over 16.51 s = 71.3507 callbacks/s; guest process CPU 0.76 s.
Private source/config integrity passes and all jobs terminal. Ordinary artifacts
not replaced. This verifies USB2 behavior at current default, not other intervals
or WLAN traffic. Next compare 0/160 under USB2, then remaining WLAN/recovery scope.
p030 remains uncleared; default 4000 maintained.

## q258: USB2 interval comparison complete

USB2 0/160 private campaigns PASS. Both verify actual interval and root speed
ID 3, full USB/HID input and hotplug campaign, 64 confirmed write/fsync/readback
samples with HID overlap. Source/workload identity matches q257 4000. Evidence
../temp/q258-usb2/{result.json,i0-result.json,i160-result.json,comparison.json}.
Private source/config integrity passes; all jobs terminal, defaults unchanged.

| IMOD | Handler entries | Interval seconds | Entries/s | Guest CPU seconds |
| --- | --- | --- | --- | --- |
| 0 | 1184 | 16.48 | 71.845 | 0.72 |
| 160 | 1181 | 16.51 | 71.532 | 0.76 |
| 4000 (q257) | 1178 | 16.51 | 71.351 | 0.76 |

USB2 and original USB3-root comparisons now have current counter evidence across
all three intervals. Single paced QEMU observations and 10-ms guest time resolution
do not establish physical optimality. Default 4000 retained. Still uncleared for
WLAN concurrency and remaining recovery/physical acceptance from the phase; do
not keep listing the completed USB2 interval matrix as outstanding.

## q259: default media recovery and unavailable WLAN host

Read-only SSH readiness check to user-authorized 10.0.10.25 failed with
`No route to host` (exit 255). No remote state changed. WLAN concurrency remains
uncleared; resume when routing and SSH work, then inspect attached radios and
active processes before assigning pass-through devices.

Independent default-4000 ordinary-kernel disposable QEMU campaign passes
writeback/fsync/remount and equal-capacity media replacement. Idle replacement
re-enumerates and exposes the new marker; replacement while mounted rejects
old cached reads with ENXIO and does not rebind the mounted namespace.
Evidence ../temp/q259-media/{results.json,guest.log,source.sha256,source.json}.
This covers specific REC03/REC05/REC06 cases; continuous UA/reset, pending BIO
recovery and late completions are not proven by this run. Source image unchanged,
QEMU terminal, no production build/source changes. p030 remains uncleared.

## q260: current actual BOT recovery gate

Repaired reservation runner's retired io-stats.c and p031 fragment dependency;
it uses current io.c, and accepts explicit fixture names. The xHCI fixture now
references the consolidated source path. Only storage was selected/executed.
No production changes, no unrelated full test suite execution.

../temp/q260-storage contains commands/build/run logs and scope.json. Actual
usb-storage.c fixture passes ordinary and ASan/UBSan/leaks: one owned reset plus
one current-UA retry succeeds; repeated UA revokes/fails; repeated transport
failure terminates after exactly one reset. Control-worker attach/publication/
stop/join failures retain ownership until safe teardown, and mode/partition
control recovery cases pass. These are controlled transport/scheduler tests,
not a native reset-under-IMOD timing test. They refresh REC01 and portions of
REC02/REC04 evidence after refactoring. Existing native q259 media cases retained.

p030 remains uncleared for unavailable WLAN/physical comparison and any remaining
end-to-end pending-I/O/late-completion coverage. Do not list bounded BOT reset as
unimplemented, or infer every REC04/REC06 case from this fixture. All jobs terminal.

## q261: current USB core reservation and late completion

Selected USB core reservation fixture passes ordinary and ASan/UBSan/leaks,
../temp/q261-usb/{commands.json,usb-ordinary.log,usb-sanitize.log,scope.json}.
Actual usb.c with controlled HCD: reservation growth failures preserve the old
owner; warm transfers allocate nothing; cancel failures refuse buffer reuse;
freeing the caller while completion is delayed retains the reservation until
completion, then releases it once. Shared staging follows the same lifetime and
no stale client pointer is copied into after timeout. Final allocation count
returns to baseline. No production or test changes required in this queue.

The included recovery fixture supplies collaborators; its renamed original main
is not executed by this selected test. Therefore this run is specifically
reservation/cancel/late-completion evidence, not the entire inherited recovery
suite. Complements q260 BOT and q259 native media evidence for REC04/06 portions.
WLAN host availability, physical timing and native recovery correlation remain
separate; p030 stays uncleared. All jobs terminal.

## q262: current HCD reservation/SG boundary

Actual consolidated pci-xhci.c fixture passes ordinary and ASan/UBSan/leaks,
../temp/q262-xhci/{commands.json,xhci-ordinary.log,xhci-sanitize.log,scope.json}.
Checks allocation failure, retained/warm reservations, busy-owner isolation,
separate reclaim reserve and generation exhaustion. SG planner produces high
address TRBs, correct chain/IOC/short-transfer accounting and 64-KiB boundary
splits; rejects invalid count/size/overflow. Final coherent/vector ownership
counts balance. No source repair was needed for the selected fixture.

High DMA here is controlled address encoding, not actual device DMA; p037 native
high-DMA gate is not cleared by this result. Current core/BOT/HCD limited host
regressions now pass (q260-262) alongside q259 native media and q255-258 interval
matrices. Remaining WLAN/native recovery correlation is distinct. Do not repeat
these unchanged host gates without a relevant change or new failure.
All jobs terminal; production source/default unchanged; p030 stays uncleared.

## q275 evidence reconciliation / WLAN readiness

Read-only SSH again exits 255 with No route to host. No remote mutations.
Re-read p025 final REC01–06 table and p029 q230 acceptance: recovery functionality
is implemented/accepted; p030-specific experiment combinations are the remaining
scope, not another complete recovery rewrite. See [remaining evidence](remaining-evidence.md)
for precise boundaries and next native media interval fixture work. No current
physical latency or WLAN claim. No production changes or redundant host reruns.

## q276: explicit native media artifact / IMOD verification

Maintained writeback runner accepts --image, --root-image and --expect-imod
(0..65535), validates images/options before guest creation, and uses a separate
QMP socket with existing IMOD capture helper. Captures actual BAR/RTSOFF/IMOD
before workload and after media exchange, records source/root hashes and
completion/source-integrity in campaign.json, including runtime failure.
Existing monitor-driven commands and media oracle remain unchanged.

Default 4000 smoke PASS (`temp/q276-media`): both MMIO captures report 4000,
writeback/fsync/two remount readbacks, idle equal-size replacement, mounted old
cache rejection without namespace rebinding. Source unchanged. Deliberate
expected=0 against the same default image rejects at readback before workload
(`temp/q276-mismatch`, completed=false/source_unchanged=true). Out-of-range
65536 rejects during argument parsing; syntax check passes. Both QEMU processes
confirmed absent after cleanup. No production rebuild/change this queue.

Next compare 0/160 artifacts using these exact options and same root image;
retain native register evidence rather than trusting compile flags. Physical
latency/WLAN/other delayed-completion scope remains uncleared.

## q277: media recovery across 0/160/4000

Dedicated IMOD0 and IMOD160 image builds/native runs PASS. Both actual MMIO
readbacks match before workload and after exchange. Same workload-root hash
as q276 default4000; all pass writeback/fsync/two remount readback, idle
equal-size replacement, mounted old-cache rejection without namespace rebind.
Evidence `temp/q277-i0`, `temp/q277-i160`, comparison in
`temp/q277-images/comparison.json` also includes q276 default4000. Source images
unchanged. Ordinary default build restored, no link wrappers; logs
`/tmp/zedbsd-q277-{i0-build,i160-build,default}.log`. No production changes.

These native media REC03/05/06 subsets are now covered at all three intervals.
Do not repeat them as missing; this is not native pending-BIO/late-completion
fault injection, USB2 replication, WLAN concurrency or physical latency.
Default4000 remains unchanged; p030 uncleared for its remaining scope.
