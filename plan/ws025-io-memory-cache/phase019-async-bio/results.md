# ws025-p019 results

Status: in progress (q108), 2026-09-07.

Implemented claim reference ownership with canonical leaf pinning, separated
BIO admission/frontier registration from dispatch, and added a distinct bounded
async interface in bio-async.h / disk-async.inc. Four lazy permanent workers,
four owned 64 KiB request slots per enabled endpoint and a two-entry queued
limit provide explicit admission bounds. Prepared writes copy caller data;
accepted requests and callbacks retain separate references. Running cancellation
retains ownership and returns EBUSY; queued cancellation completes once.

`temp/p019-claim-owner-ref.log` passes ordinary/sanitizer file/VM/claim checks
(411248 / 408651) including exclusion after the original claim owner releases.
Canonical leaf pinning was added afterward and requires the next regression run.
`temp/p019-admission-split` passes the 71-check frontier/observer fixture in both
variants. `temp/p019-async-media-epoch` passes the first 115-check async fixture
in both variants. Expanded cancellation/short-read/detach tests are in progress.

The initial queue test exposed persistence-epoch overloading: cancelling an
unissued write invalidated proof and incorrectly made unrelated requests stale.
A separate media epoch now changes on explicit reset/replacement/detach,
independently of ordinary completion failures. Saturation refuses new async
requests conservatively. Completion retains the worker owner through callback
return, so final cleanup is not moved into an interrupt completion callback.

`temp/p019-async-native-first` passes at 512 MiB using a link-only disk_ioctl
probe, the real kernel scheduler, a driver that cannot complete until submit
returns, and an actual USB sector read compared with synchronous data. The test
adds no production ioctl ABI. The probe kernel must be replaced by an ordinary
kernel before subsequent general acceptance. Phase p019 is not yet complete.

The expanded `temp/p019-async-loop-2` fixture passes ordinary/sanitizer
(1177 / 1167 checks, scheduling-dependent). It executes the production loop_submit
body with a real async upper request and synchronous lower BIO, verifying drain,
origin and generation propagation plus inode lifetime. It also covers 100
cancel/completion races, short-read failure, detach with queued and late BIOs,
callback-final-release, endpoint re-enable without another thread, and failed
HAL free keeping ownership/charge. Host workers are joined on fixture shutdown;
production workers remain bounded and idle for reuse.

`temp/p019-claim-pins-final.log` passes 411158 / 409010 file-cache checks after
claim leaf pinning; `temp/p019-frontier-final` passes 71 checks in each variant.
The supported pcat/pc98/amd64 builds pass in `temp/p019-final-*.log`. The ordinary
kernel was relinked and has no __wrap_disk_ioctl symbol. First general acceptance
stopped on the isolated WS016 claim fixture missing disk reference services;
that fixture now models the held leaf reference and tests last-owner release.
The final full storage/native gate is running; p019 remains in progress.


## Final p019 verdict — complete (2026-09-07)

`temp/p019-async-final` passes 1179 checks in each variant, including a full
64 KiB read, device-limit refusal before admission and write-before-flush FIFO.
Requests larger than a device's declared transfer limit return E2BIG rather than
silently violating its contract; callers split at the existing limit.
`temp/p019-limit-{pcat,pc98,amd64}.log` all pass after that admission refinement.
`../ws018-kernel-architecture/temp/ws025-p019-final/results.json` records FS50/50,
Wi-Fi30 in both variants and two native boots passing with the ordinary kernel.
The native async scheduler/USB probe described above remains the p019 runtime
acceptance; the final size admission check is additionally covered by the host
fixture and supported builds. `git diff --check` passes.

ASYNC01–ASYNC08 and FLUSH02–FLUSH06 are covered by the owned request fixture,
production loop adapter, frontier regression, retained p009 DMA retirement tests
(unchanged driver/DMA paths), and native tests. No active hardware cancellation
or native NVMe queue-depth gain is claimed. Endpoint and request lifetime are
bounded; final callback release waits for worker retirement. Physical runtime
remains user-accepted. No production test ioctl or probe remains in the ordinary
kernel, and no commit was created.
