# ws025-p029 results

## 2026-09-10 現行状態

uncleared / 修正後確認待ち（67b28ce0）。旧q230受け入れは履歴として保持。
UAS関連とDMA/scratchの修正後確認が必要。現行commitのQEMU受け入れは未実施。実機不要というユーザー基準を維持。
今回の確認はソース読取りのみ。詳細は[修正後照合](../post-rollback-review.md)。

Current: q222 revoked-mount VM ownership preflight implemented; full phase remains uncleared. See
[requirement/evidence ledger](acceptance-audit.md); mounted lost-medium teardown
is reproduced as BUG-021. Historical entries below retain their original scope.

## Historical q144/q145 result

QEMU high/super-speed UAS enumeration and actual descriptor capture pass. The
production capability parser is implemented and passes 10,209 ordinary checks,
10,209 ASan/UBSan checks, and amd64/PCAT/PC98 CI builds. Full UAS transport is
not implemented; p029 remains uncleared. There is a usable QEMU backend, so the
user's condition for moving UAS to Future is not met. See
[capabilities, implementation and transport design](transport-design.md).

## Historical q125 result

Status: uncleared
Date: 2026-09-09

## 確認した事実

HEAD 34a1f6dの現ソースとq122/p024の既存証拠を確認した。
直近実機はWLANとホストUSB Ethernetであり、UAS storageのdescriptor/stream能力は取得していない。現在テスト機SSHがtimeout。

## 未完了と再開

対象UAS機器とdescriptorが未確認。現Phaseの実機先行条件を満たさず、架空descriptorに合わせたdriverは実装しない。

UAS対象のdescriptorと利用可能な実機試験経路。

production実装・新規性能測定を行ったとは主張しない。既存挙動と既定値を維持した。


## q191: high-speed command protocol component (2026-09-10)

Implemented production command IU encoding and explicit WAIT_STATUS / DATA /
COMPLETE / FAILED transitions in usb-uas.c, with declarations in usb-uas.h.
LUN peripheral addressing, nonzero big-endian tag, SIMPLE CDB padding and READY /
Sense byte layout were checked against local build/qemu-pc98/hw/usb/dev-uas.c;
this source inspection is not a new run of the installed QEMU backend.

One READY grants one matching-direction transfer. Completion cannot arrive
while data is owned; successful SCSI status requires the expected byte count.
CHECK CONDITION can report failure before data or after a short transfer. Sense
length and qualifier, IU type, tag, framing and state are validated. Protocol
failure poisons state; only an eventual owner that has retired DMA and the device
task may initialize a fresh command. No claim of timeout recovery is made by
this state-only component. No USB class binding or disk has been registered.

Evidence:
- tests/run-uas-command-host.sh compiles production source: ordinary and
  ASan/UBSan PASS, /tmp/zedbsd-q191-command-final.log. Tests cover exact wire
  bytes, both directions, no-data completion, early CHECK CONDITION, short /
  oversized data, unexpected/duplicate READY, status during data, all unknown
  IU IDs, malformed lengths, wrong tags, qualifier and poisoned-state rejection.
- Existing production descriptor suite: 10,209 checks each ordinary and
  ASan/UBSan, /tmp/zedbsd-q191-descriptor.log.
- Explicit amd64 / PCAT / PC98 disk-image builds PASS:
  /tmp/zedbsd-q191-amd64-final.log, /tmp/zedbsd-q191-pcat.log,
  /tmp/zedbsd-q191-pc98.log. Subsequent production edit changed comments only.
- git diff --check PASS. No aggregate make check or QEMU I/O run this queue.

The first sanitized run encountered the known ptrace/LeakSanitizer restriction;
rerunning outside that restriction passed with leak checking enabled. The first
amd64 build caught that zedBSD errno lacks EPROTO (host libc provides it); this
component now reports EIO on protocol failure. Final host and target builds pass.

Full p029 remains uncleared. Next: connect this state engine to preallocated,
serialized endpoint URBs with finite deadlines and checked retirement, add task
management recovery and SCSI capacity/flush disk publication, then implement
SuperSpeed stream ownership and run native persistence/replug acceptance. Keep
all original phase acceptance requirements and QEMU-only hardware waiver.


## q192: high-speed synchronous USB endpoint transport (2026-09-10)

Added usb-uas-transport.c to amd64 and PCAT USB class builds. One class-owner-
serialized transport allocates/reserves four endpoint-specific URBs before I/O,
validates high-speed bulk endpoint directions and packet sizes, and drives the
q191 protocol with a shared monotonic deadline. Paired HCD transfer reservation
supports up to 64 KiB; without it, capacity is reduced to the existing 8 KiB
reclaim-safe contract and synchronous staging is reserved. Status reception is
bounded at 512 bytes; retained sense is bounded at 252 bytes. Oversized/truncated
IUs fail protocol validation rather than being interpreted as successful SCSI.

Command/data/status failures close admission; host drain alone never authorizes
new commands. Stop cancels and checks all four URBs, retaining all resources if
any drain fails. Successful stop frees them only after HCD retirement. The
caller must hold device/endpoints and serialize init/execute/stop. Reinitializing
a failed device still requires device-task retirement/reset by the future class
owner. No recovery success or disk registration is claimed here.

Focused production-code test: tests/run-uas-transport-host.sh, ordinary and
ASan/UBSan PASS, /tmp/zedbsd-q192-transport-final.log. Covers read/write/no-data,
early CHECK CONDITION, exact endpoint order, decreasing deadline, successful
tag wrap, short command/data, submission and wait failures at all four stages,
failed-command admission, drain failure retaining every URB then successful
teardown, and partial reservation failure at every endpoint. The fixture mocks
USB API behavior; it does not establish real HCD DMA cancellation or native I/O.
Initial restricted LeakSanitizer execution hit ptrace restrictions; authorized
unrestricted execution passed with leak checks enabled.

Explicit supported image builds: /tmp/zedbsd-q192-amd64.log,
/tmp/zedbsd-q192-pcat.log and /tmp/zedbsd-q192-pc98.log PASS.
No aggregate test run, class matching, physical access or new QEMU run.

Full p029 remains uncleared. Next bounded work must connect SCSI probe/capacity /
flush policy and disk BIO registration to this transport, with class detach and
failed-device recovery. SuperSpeed stream/HCD ownership and original native
read/write/fsync/replug acceptance remain required. The reusable transport is
implemented, so next work must use it rather than repeat protocol-only planning.


## q193: high-speed disk class and first native I/O (2026-09-10)

Added usb-uas-disk.c, linked and registered under CONFIG_DRIVER_USB_STORAGE for
amd64/PCAT. Class 08/06/62 matches only supported high-speed interfaces after
full descriptor/pipe validation. Separate owner probes direct-access LUN 0,
readiness, READ CAPACITY(10)/(16), MODE SENSE cache/write-protection and initial
SYNCHRONIZE CACHE. Existing SCSI policy helpers determine read-only / flush / FUA
behavior. READ/WRITE(16) BIOs enforce bounds and exact byte count, serialize with
flush, and complete through disk BIO contracts. Class stop checks URB retirement;
detach uses disk/partition retirement before destroying the class. Failed attach
with live DMA retains a bound owner for later stop. No BOT reset/fallback.

Corrected q191/q192's overly strict GOOD byte count: SCSI allocation-length
responses such as MODE SENSE may be shorter. Protocol still requires READY/data
for a successful data command, but returns actual length; block BIO verifies
exact count. Focused command and transport fixtures were changed to exercise
this valid case, not to suppress real block short-I/O errors.

Evidence:
- tests/run-uas-disk-qemu.py: temp/q193-native2/result.json PASS. Current amd64
  image boots on disposable BOT media with a distinct EHCI UAS 32 MiB SCSI disk.
  Kernel publishes sdb, 65,536 x 512-byte blocks, policy=SYNC_CACHE. Guest writes
  2 KiB at byte 4096, invokes sync, reads it to a file and validates SHA-256.
  After QEMU exits, host verifies exactly that range changed from A5 to zero,
  with the entire remaining backing unchanged. Original boot image unchanged.
  Guest log, pcap, argv, result and production source-manifest retained there.
  /tmp/zedbsd-q193-native2.log reports PASS.
- This is raw I/O plus guest sync and backing-byte verification, not yet a mounted
  UAS filesystem fsync/reboot/replug or timeout-recovery acceptance claim.
- First native run q193-native1 reached UAS publication but waited at the empty
  password prompt because the harness omitted that input. It was explicitly
  interrupted and cleaned up, not relabeled PASS. Fixed harness performs normal
  root + empty password. A supplemental QMP connection attempt timed out while
  the first client owned the monitor; no production change was needed.
- Production protocol and transport host tests ordinary + ASan/UBSan PASS:
  /tmp/zedbsd-q193-command.log and /tmp/zedbsd-q193-transport.log.
- Explicit amd64 / PCAT / PC98 image builds PASS:
  /tmp/zedbsd-q193-amd64.log, /tmp/zedbsd-q193-pcat.log,
  /tmp/zedbsd-q193-pc98.log. git diff --check PASS.

Full p029 remains uncleared. High-speed class and disk I/O now work. Next are
bounded device-task recovery (without replaying uncertain writes), class/media
lifetime verification under detach/replug, SuperSpeed stream allocation/HCD
ownership, and the remaining original native filesystem/fsync/recovery matrix.
READ CAPACITY(16) and FUA paths are implemented but not exercised by this 32 MiB
QEMU cell. Do not claim those paths tested from this normal-case evidence.


## q194: idle replug and checked shutdown (2026-09-10)

Extended tests/run-uas-disk-qemu.py with --lifecycle. Production source is unchanged
from q193; its source manifest was rechecked and retained with this acceptance.
No rebuild or broad test campaign was necessary.

Authoritative result: temp/q194-lifecycle3/result.json PASS, log
/tmp/zedbsd-q194-lifecycle3.log. Initial raw write/sync/readback and whole backing
range verification pass again. QMP removes the UAS function after I/O closes;
guest reports the exact bus/device/port disconnect. Recreated UAS + SCSI LUN on
the same backing is explicitly attached, freshly published as sdb and reread.
The readback SHA-256 matches before and after replug. Name reuse is expected and
does not itself prove generation isolation with an old open handle.

With the recreated disk attached, guest halt reaches host-controller quiescence;
halt-samples.json proves all four CPUs HLT=1, IF=0 for three consecutive samples.
No panic/trap/assertion/driver shutdown/controller stop failure is present.
Original production boot image is unchanged; all storage writes used clones.

First attempt q194-lifecycle1 completed detach but QEMU auto-deleted the legacy
-drive backend along with its SCSI child, so re-add could not find uasdisk.
Second q194-lifecycle2 recreated the backend but omitted the explicit UAS attach
step. QEMU hotplug creates usb-uas with attached=false until LUNs are configured,
as verified in local /usr/share/doc/qemu-system-common/system/devices/usb.html
(Hotplugging USB storage) and build/qemu-pc98/hw/usb/dev-uas.c. Final harness uses
blockdev-add, device-add UAS/SCSI, then qom-set attached=true. These were harness
failures; earlier results remain failed, not rewritten. No kernel defect was
found by this idle-lifecycle cell.

Full p029 stays uncleared: live I/O cancellation and device-task recovery,
old-handle/generation scenarios, SuperSpeed streams and mounted-filesystem fsync
acceptance remain. Idle replug and attached-device shutdown now have native
evidence and need not be repeated without a relevant production change.


## q195: task abort recovery and write uncertainty (2026-09-10)

Transport records the failed tag/LUN, closes admission, and permits one explicit
recovery attempt before a later BIO. All four URBs must cancel/drain before
management traffic; each drain is bounded at 1 s. A distinct-tag 16-byte ABORT
TASK then uses a shared finite response deadline. Exact Response IU / TMF COMPLETE
is required; bounded old-tag READY/Sense records may precede it, foreign tags and
malformed responses fail closed. Successful task retirement advances past both
old tags. Failed recovery is not retried indefinitely, does not release URBs, and
stop disables future recovery. A failed CDB is never replayed.

Disk submit attempts this recovery on a later request, not while completing the
failed BIO. An uncertain/short/failed WRITE latches flush_error. Recovery never
clears it: subsequent reads may proceed, but WRITE and FLUSH retain the failure.
This preserves the uncertainty instead of certifying it with a later sync.

Focused evidence:
- tests/run-uas-transport-host.sh ordinary + ASan/UBSan PASS,
  /tmp/zedbsd-q195-transport.log. Added actual recovery-code tests for exact abort
  bytes/tag/LUN, tag wrap, queued READY/Sense, wrong/short/failed response, failed
  command submission/wait, short management command, exhausted stale-response
  bound, failed host drain, no second recovery attempt and no recovery after stop.
- tests/run-uas-disk-host.sh compiles the complete production disk translation
  unit (discarding unused registry sections), ordinary + ASan/UBSan PASS,
  /tmp/zedbsd-q195-disk.log. Real uas_submit/uas_command verify one failed read
  followed by one new successful read, and both failed and short writes remaining
  visible to later flush/write without extra commands. USB/mutex/BIO collaborators
  are mocked; this establishes class behavior, not real DMA retirement.
  ASan global instrumentation is disabled only to allow unused class-registration
  sections to be discarded; heap/stack/UB instrumentation and leak checks remain.
- Explicit amd64 / PCAT / PC98 disk-image builds PASS:
  /tmp/zedbsd-q195-amd64.log, /tmp/zedbsd-q195-pcat.log,
  /tmp/zedbsd-q195-pc98.log. git diff --check PASS.

Full phase remains uncleared. Native timeout/abort acceptance must exercise this
new path; q194 was before this change and does not prove recovery. A concrete next
fixture can place a QEMU throttle filter on the disposable UAS backing and adjust
its throttle-group limits via QOM to delay a read beyond the transport deadline,
then release the delay and verify a new request plus management IU capture. Local
build/qemu-pc98/block/throttle.c and throttle-groups.c expose that mechanism.
Implement/validate the fixture before claiming any native abort success.

Media/capacity/Unit Attention revalidation, old-handle/live disconnect scenarios,
SuperSpeed streams and mounted filesystem/fsync acceptance remain. ABORT TASK is
not a media reset and does not itself satisfy those recovery matrix cells.


## q196: native read-timeout / ABORT TASK acceptance (2026-09-10)

Added --timeout-recovery to tests/run-uas-disk-qemu.py. A QEMU throttle filter is
inserted only on the disposable UAS backing, with QOM-configured read limits;
BOT boot storage stays unlimited. Production is unchanged from q195. No new
build or broad host rerun was needed for this harness/evidence change.

Authoritative acceptance: temp/q196-timeout2/result.json PASS; log
/tmp/zedbsd-q196-timeout2.log. Normal write/sync/readback first succeeds. A priming
read at LBA 4096 succeeds, the distinct read at LBA 8192 under 256 B/s throttling
returns Connection timed out and zero bytes. After limits are cleared, a new
explicit read succeeds and its 512 bytes match the A5 backing SHA-256.

Captured USB protocol establishes the real recovery path, not merely a successful
retry: failed READ(16) tag 13 / LBA 8192 occurs once; ABORT TASK tag 14 targets tag
13; exact TMF COMPLETE response tag 14 precedes new READ(16) tag 15 for the same
LBA, followed by READ READY and GOOD Sense IU. protocol-audit.json independently
validates that ordering and identity. pcap, guest error/readback transcripts,
QEMU argv, throttle settings and source manifest are retained. Original image
unchanged; whole target backing still differs only at the earlier intended 2 KiB
write range. The stronger protocol/timeout checks were also added to the harness
and applied directly to this captured result; no repeated guest run was needed.

Initial q196-timeout1 used 1 B/s. It reproduced the guest timeout and captured
ABORT TASK submission, but QEMU then waited for its already-scheduled throttle
timer while synchronously cancelling the SCSI backend. QOM limit changes update
configuration without rearming existing waits (local throttle-groups.c), and
scsi_req_cancel calls synchronous blk_aio_cancel (local scsi-bus.c). With the
4 KiB BIO that delay was thousands of seconds. The harness hit its finite wait
and cleaned up; this failed attempt is preserved. Final 256 B/s produces about
16 seconds of throttle spacing, enough for the guest's 5-second command timeout
while allowing QEMU's cancellation to retire finitely. No kernel fix was needed.

This proves native high-speed read timeout -> checked task abort -> new read.
It does not prove uncertain-write injection, physical hardware behavior, medium
change/live old-handle generation, SuperSpeed stream recovery or mounted UAS
filesystem fsync. Those requirements remain in full p029.


## q197: USB stream identity and xHCI ownership map (2026-09-10)

Implemented drv_usb_urb_setup_stream / drv_usb_urb_stream_id and explicit
DRV_USB_HCD_CAP_BULK_STREAMS. Nonzero IDs require SuperSpeed bulk with a stream
companion and advertised HCD support. Pending or HCD-owned URBs reject changes;
failed setup leaves prior fields unchanged. Ordinary/control setup clears stream
identity to zero. HCD enqueue must still validate the allocated endpoint/stream.
No HCD advertises this new capability yet, so no unsupported nonzero request is
sent to current hardware by this change.

[Stream integration map](stream-integration.md) records the actual xHCI ring,
request/provenance, dequeue/restart, disable/free and class transaction changes
needed next. It also records the crucial SuperSpeed difference: there is no
high-speed READY sequence, so data/status URBs and cancellation must be coordinated
rather than reusing the sequential high-speed loop.

Focused actual USB core test: tests/run-usb-stream-host.sh, ordinary and ASan/UBSan
PASS, /tmp/zedbsd-q197-stream2.log. It includes the complete current usb.c and
uses real setup/control/accessor functions with constructed idle ownership;
only unused functions are link-discarded. Tests cover unsupported HCD/speed/type /
companion, ID overflow, failed ordinary setup preserving ID, pending/HCD-owned
immutability, default/control reuse and explicit zero compatibility. As in the
class fixture, ASan global instrumentation is disabled for discardable unused
sections, while heap/stack/UB and leak checking remain. Initial compile missed
the include/uapi path; corrected runner passed, without production workarounds.

Explicit amd64 / PCAT / PC98 image builds PASS:
/tmp/zedbsd-q197-amd64.log, /tmp/zedbsd-q197-pcat.log,
/tmp/zedbsd-q197-pc98.log. git diff --check PASS. No new QEMU claim for this API.

Full p029 remains uncleared. Next implement the explicit configuration transaction
and xHCI stream rings/ownership from the integration map, then connect the UAS
SuperSpeed state machine and native acceptance. Do not advertise capability from
this setup-only test or count the API as complete SuperSpeed transport.


## q198: xHCI stream ring ownership implementation (2026-09-10)

Added explicit drv_usb_endpoint_configure_streams and HCD endpoint_streams
callback. Core retains binding/device lifetime and uses the existing selection,
empty interface I/O gate and device-control lock sequence. HCD reports whether
hardware-visible changes began; an uncertain configuration quarantines the
retained device/resources. This does not silently reconfigure every bulk device.

xHCI now owns a four-entry primary context array and three stream rings per
configured endpoint. Configuration reserves DMA, quiesces the old empty endpoint,
then replaces its endpoint context with MaxPStreams=1 / LSA / primary SCT entries.
Failed pre-publication allocation frees partial resources; uncertain hardware
configuration retains the array/rings for checked teardown. Each request records
its selected ring and stream ID; events match that ring plus existing slot/DCI /
TD bounds. Doorbells and cancelled-request Set TR Dequeue carry stream ID.
Whole-endpoint recovery/quiescence sets every configured ring dequeue; endpoint
drop/slot release free all rings only after their existing ownership barrier.
One active request per endpoint remains the initial admission policy.

No HCD advertises BULK_STREAMS yet. The new configuration path and SuperSpeed UAS
remain disconnected pending the next integration/acceptance step; these builds
and helper tests are not claimed as native stream operation.

Evidence:
- tests/run-xhci-stream-host.sh includes complete production xHCI source. Ordinary
  and ASan/UBSan PASS, /tmp/zedbsd-q198-stream3.log: default vs primary context
  encoding, ID bounds, actual event matcher accepting only the selected stream
  ring, stale/wrong-ring/slot rejection, and freeing every owned stream resource.
  DMA free is mocked; it does not simulate configure/cancel command completion.
  Host/target signal types initially collided; the fixture separates their type
  names and supplies tid_t. Production headers were not changed to suit the host.
- Actual USB core ID tests PASS ordinary/ASan/UBSan,
  /tmp/zedbsd-q198-usb-stream.log. Configuration lifetime transaction itself still
  needs the upcoming native stream exercise; unused sections are link-discarded.
- temp/q198-default1/result.json PASS: QEMU BOT boot through changed xHCI, separate
  EHCI UAS raw I/O, idle detach/replug/readback, and attached-device four-CPU halt.
  /tmp/zedbsd-q198-default1.log; source manifest retained. Default ring provenance
  is exercised here. This is regression evidence, not a streams-enabled device.
- Explicit amd64 / PCAT / PC98 image builds PASS:
  /tmp/zedbsd-q198-amd64.log, /tmp/zedbsd-q198-pcat.log,
  /tmp/zedbsd-q198-pc98.log. git diff --check PASS.

Next: connect SuperSpeed UAS with coordinated status/data URBs (no READY), bounded
normal/management tags in the configured stream IDs, and explicit configuration
before reserve/submit. Enable xHCI capability only with hardware MaxPSA support
and execute real superspeed QEMU I/O/recovery/lifecycle. Media-generation and
filesystem acceptance remain in the full phase; p029 is still uncleared.


## q199: native SuperSpeed UAS integration (2026-09-10)

Capable xHCI now advertises bulk streams. USB HCD registration accepts the new
capability only with endpoint configuration callbacks. UAS configures three
usable status/data stream IDs before reserving URBs. Commands use stream zero;
status and data use the command tag and are submitted before either is awaited.
Sense validation, early SCSI failure, peer cancellation and shared deadlines
retain caller-buffer isolation. Successful commands rotate tags 1..3.
SuperSpeed task recovery deliberately returns EOPNOTSUPP: independent old and
management status streams require checked retirement before reuse. HS abort is
preserved. This is an incomplete recovery path, not a full p029 clear.

Evidence:
- temp/q199-super3/result.json: native SS disk sdb probe, 2 KiB raw write at
  offset 4096, sync/readback, exact persisted backing bytes, idle removal/replug
  and readback, then four CPUs halted with IF clear in three samples all PASS.
  Original boot image unchanged. guest.log, argv.json, uas.pcap and post-run
  source-manifest.json retained. /tmp/zedbsd-q199-super3.log records completion.
- run-uas-super-host.sh ordinary and ASan/UBSan PASS:
  /tmp/zedbsd-q199-super-host-final.log. Actual transport covers coordinated
  admission, read/write/no-data, early SCSI failure, wrong tag, endpoint errors,
  timeout cancellation, submission failures and tag wrap. USB collaborators are
  mocked; this does not prove native timeout recovery.
- Existing HS transport/task-abort host checks ordinary and ASan/UBSan PASS:
  /tmp/zedbsd-q199-hs-host.log.
- amd64 / PCAT / PC98 disk-image builds PASS:
  /tmp/zedbsd-q199-amd64-2.log, /tmp/zedbsd-q199-pcat.log,
  /tmp/zedbsd-q199-pc98.log.

Failed attempts are retained. super1 exposed the missing HCD registration
capability mask; fixed before the final build. super2 published UAS sda first,
then VFS failed (6) before boot BOT sdb appeared. super3 places boot BOT at xHCI
port 1 and UAS at port 2; it proves that topology, not arbitrary boot enumeration
order. The boot-wait observation is recorded alongside BUG-017 for triage without
assuming it has the same root cause as the earlier enumeration failure.

Full p029 remains uncleared. Resume with distinct old/management stream retirement
and native timeout/abort/new-I/O, then remaining media-generation and filesystem
acceptance. No physical UAS dependency is introduced.


## q200: native SuperSpeed timeout baseline and recovery decision

`temp/q200-timeout1/result.json` and guest.log reproduce the missing recovery:
normal raw I/O succeeds, the cold read at LBA 8192 times out with zero bytes,
and a new read after removal of throttle fails with Operation not supported.
The harness exits 1 (`/tmp/zedbsd-q200-timeout1.log`); this is an expected baseline
failure, not acceptance. Source boot image is unchanged. QEMU test process has
terminated. No production code changed and no rebuild was required.

Read-only examination of local QEMU dev-uas.c shows queued old Sense survives
ABORT TASK and host packet cancellation; only device reset clears the entire
queued result list. The management stream therefore cannot prove retirement of
old queued status. The proposed SS recovery uses checked USB reset, reconstructed
streams and validated media/geometry before a new BIO. Existing core reset has
physical-generation and DMA barriers and preserves binding objects, but is
root-port-only. Probe currently mutates published owner fields and must be
refactored before reuse for recovery. Full ordering and failure contracts are in
[stream integration](stream-integration.md#q200-recovery-decision-reset-rather-than-infer-an-empty-old-stream).

p029 remains uncleared. Next implement the class reset/reprobe path and host
ownership/failure checks, then require this native baseline to recover using
reset evidence rather than the HS-only ABORT-IU assertion. Media identity and
sticky uncertain writes remain mandatory; a timeout cannot prove an empty stream.


## q201: probe result publication prerequisite

`usb-uas-disk.c` now returns a complete `uas_media` candidate from `uas_probe`.
Intermediate capacity/cache changes do not modify the class owner. Initial attach
copies the completed description before disk publication. The sticky flush error
is untouched. This allows future reset revalidation to compare a candidate to
published state without corrupting the latter on failed probing.

Actual-source class host fixture passes ordinary and ASan/UBSan builds
(`/tmp/zedbsd-q201-host.log`). It injects transport failure at each of five commands
in the READ CAPACITY(10) probe path and verifies the output sentinel, preexisting
owner geometry/policy/write protection and sticky write error remain unchanged.
Successful probing returns the new geometry without mutating the owner. Existing
failed-BIO/new-read and uncertain-write tests still pass. CAPACITY(16), media
identity and reset ownership are not claimed covered by these new cases.

Explicit disk-image builds for amd64, PCAT and PC98 pass:
`/tmp/zedbsd-q201-amd64.log`, `/tmp/zedbsd-q201-pcat.log`,
`/tmp/zedbsd-q201-pc98.log`. No new native runtime claim for this refactoring.
Full p029 remains uncleared; next connect checked reset, reconstructed transport
and media validation, then run the q200 native failure cell to recovery.


## q202: checked SuperSpeed reset recovery

Class owner now retains device/endpoints and standard inquiry identity. On a new
BIO after SS transport failure, one attempt stops/drains the old transport, invokes
checked USB device reset, reconstructs stream configuration/reserved URBs, and
probes into a candidate. It admits the new BIO only with unchanged fixed-LUN
inquiry, geometry and cache/write-protect policy. Reset UNIT ATTENTION is the only
retryable recovery readiness sense. Failed steps stay closed; failed write/flush
uncertainty is preserved. Stopped teardown owners cannot restart. High-speed still
uses its existing ABORT TASK. No failed BIO is automatically replayed.

Actual-source host fixture ordinary and ASan/UBSan PASS
(`/tmp/zedbsd-q202-host2.log`): successful ordering, stop/reset/init/probe failure,
geometry mismatch, refusal of second failed recovery, sticky write error, and
existing HS/probe-publication regression. USB reset/transport are mocked there.
Native evidence is separate:
`temp/q202-timeout1/result.json` PASS. Cold LBA 8192 read times out with zero bytes;
new read causes checked xHCI port 2 reset, reprobe, then 512-byte readback with
SHA256 2ea16988ca9a3b973ff11693e6de4bd078775655cd6715c5a06a120f71b3e827.
QEMU reset trace increases after the captured pre-failure baseline; pcap has two
reads of that LBA with INQUIRY between them and no HS task-abort IU. Original boot
image and other backing bytes unchanged. Guest/trace/pcap/source manifest retained;
`/tmp/zedbsd-q202-timeout1.log` records completion. Earlier q200 failure remains.

Explicit amd64 / PCAT / PC98 disk-image builds PASS:
`/tmp/zedbsd-q202-amd64.log`, `/tmp/zedbsd-q202-pcat.log`,
`/tmp/zedbsd-q202-pc98.log`. git diff --check PASS.

Limits: reset API is root-port-only. SCSI removable media intentionally cannot
inherit this fixed-medium identity proof; disk-generation retirement/admission is
still required. Policy or identity mismatch closes admission, without pretending
it has implemented replacement-media publication. Native uncertain-write injection,
filesystem fsync and live media lifetime acceptance remain. Full p029 uncleared.


## q203: UAS UFS file fsync and remount acceptance

Native cells `temp/q203-ufs2/result.json` (SuperSpeed xHCI) and
`temp/q203-ufs-high1/result.json` (high-speed EHCI) PASS. Each formats only the
32 MiB disposable UAS disk through mkfs's exact registration confirmation,
verifies mount output identifies `/dev/sdb` at `/run/uas`, writes a 64 KiB file,
executes `sync /run/uas/payload && echo uasfilesynced`, then unmounts/remounts and
verifies SHA256 de2f256064a0af797747c2b97505dc0b9f3df0de4f489eac731c23ae9ca9cc31.
The success-only marker establishes zero exit status; current `sync FILE` calls
fsync(fd) and propagates errors, so this cell covers explicit file fsync.
Mount absence after unmount and UAS mount identity after remount are checked.
Boot source hash is unchanged; UAS backing is intentionally reformatted.
Guest/argv/pcap and source manifests retained. Logs:
`/tmp/zedbsd-q203-ufs2.log`, `/tmp/zedbsd-q203-ufs-high1.log`.

Initial `q203-ufs1` also passed but used global sync; retained without claiming
explicit per-file fsync. No production changes or rebuild were needed for q203.
The harness adds filesystem mode and exact confirmation/conditional-marker key
entry; git diff --check passes. These are clean sync/unmount tests, not crash,
uncertain-write injection or replacement-media acceptance.

Full p029 remains uncleared. Filesystem read/write/fsync and clean remount now
have native evidence at both supported speeds. Next address live media generation,
in-flight detach and remaining applicable lifetime/error cases; do not repeat
this normal filesystem cell without a relevant change.


## q204: pending-read disconnect reveals retained teardown

New `--inflight-disconnect` mode primes a 256 B/s throttle, issues a distinct
cold read and records QEMU command admission without SCSI completion or a returned
guest prompt. `pending-before-removal.txt` preserves that snapshot. The test
requires failed read termination, disconnect, replug/readback and checked halt;
none of these runs is a full pass.

- `temp/q204-super1`: QMP device_del during pending SS read aborts the QEMU process
  in usb_uas_cancel_io (canceled usb packet not found). This is a host emulator
  assertion, not a guest kernel panic. Trace and qemu.log retained.
- `temp/q204-super2`: setting UAS `attached=false` before SCSI deletion avoids
  that host assertion. Guest read returns No such device and zero bytes, but
  driver detach remains pending (17); the disconnect deadline expires.
- Production UAS detach now calls disk_media_revoke when the USB device is
  already tearing down, so removal uses partition_retire_media rather than trying
  to flush an absent medium through disk_gone_if_idle. Outstanding owners are
  still retained until idle; no forced free or reference-count bypass.
- `temp/q204-super3` after this correction: read terminates with an error and zero
  bytes, but disconnect still fails the 60-second deadline. Therefore media
  revocation alone does not resolve the retained owner. Replug/halt not reached.
  Logs: /tmp/zedbsd-q204-super{1,2,3}.log. All processes terminal; original boot
  images unchanged according to each result.json.

Focused actual-class detach fixture passes ordinary and ASan/UBSan:
`/tmp/zedbsd-q204-detach-host2.log`. It checks physical teardown revokes media,
busy retirement retains owner, a subsequent successful retirement destroys disk
before transport release, and ordinary detach uses the idle path. Disk/USB
collaborators are mocks; this does not explain the remaining native reference.
The initial fixture incorrectly expected driver-data clearing after free; corrected
the test to require clearing before free, without changing production order.
Explicit amd64 / PCAT / PC98 builds pass: /tmp/zedbsd-q204-amd64.log,
/tmp/zedbsd-q204-pcat.log, /tmp/zedbsd-q204-pc98.log. diff check passes.

p029 remains uncleared. Next observe the exact retirement rejection (backing
claim, open/inflight/cache owner, or unmatched disk reference) on this reproducible
cell, then fix that owner and rerun. The USB root scan already retries disconnecting
objects periodically; do not assume a missing retry or remove lifetime checks.


## q205: fix xHCI retained-detach retry and accept pending read removal

Temporary bounded diagnostic q205-debug1 recorded one detach snapshot:
refs=7, open=1, inflight=1, cache=1, buffers=3, revoked=1. Keeping the owner then
was correct. No subsequent class snapshot appeared. Inspecting the actual xHCI
port worker found it waited indefinitely for another hardware event. Correction
to q204's conclusion: the USB core *can* retry retained objects on a scan, but
xHCI did not periodically call it. EHCI/UHCI already did. This was missing retry,
not evidence of a leaked disk reference. Diagnostic run timed out as expected.

xHCI now scans ready root ports at 100 ms intervals, retaining event wakeups and
checking pending/stop around sleep. An event in the check-to-sleep window is
bounded by that interval. Stop publication uses release/acquire. This enables
retirement after outstanding disk users leave without a new connection edge.
Temporary class diagnostics were removed. The q204 physical-media revocation
remains necessary to retire revoked buffers without flushing an absent device.

Native `temp/q205-super3/result.json` PASS: trace-proven pending cold read,
USB attached=false, zero-byte failed read, complete guest disconnect, same emulated
USB device attached=true, fresh class publication, persisted 2 KiB readback, and
four CPUs halted with IF clear in three samples. Boot source and all backing bytes
outside the original intended write unchanged. Guest/trace/pending snapshot/argv/
source manifest retained. /tmp/zedbsd-q205-super3.log records completion.

q205-super1 and super2 already completed guest disconnect but failed re-creating
SCSI on the retained explicit QEMU backend (unnamed backend write permission
conflict), including after QOM confirmed old object removal. The accepted test
reconnects the same emulated physical device using attached=true; it does not
claim backend replacement. Earlier QEMU assertion and host failures remain.

Actual-class detach ownership fixture ordinary and ASan/UBSan PASS:
/tmp/zedbsd-q205-host.log. Explicit amd64 / PCAT / PC98 builds PASS:
/tmp/zedbsd-q205-amd64.log, /tmp/zedbsd-q205-pcat.log,
/tmp/zedbsd-q205-pc98.log. diff check PASS.

Full p029 remains uncleared: high-speed pending-read removal, uncertain writes,
and held-reference/replacement-media generations still need applicable acceptance.
Do not count same-device reconnect as replacement-media identity validation.


## q206: high-speed pending-read disconnect and fallback barrier fix

Native q206-high1 failed at disconnect after zero-byte read error. EHCI already
periodically scans, so q205's xHCI fix was not the missing piece. Bounded class
diagnostics q206-debug2 again saw only the initial genuinely busy disk (refs=7,
open/inflight/cache=1, buffers=3). Source inspection found the USB fallback for
HCDs without device_quiesce permanently refused any quarantined device, even
after its class detach and transfer retirement subsequently succeeded.

The fallback now requires an endpoint_disable callback, zero HCD-owned URBs,
successful checked shutdown of all active endpoints, and a second zero-owner
check before clearing quarantine for terminal teardown. Failed proof retains
quarantine. No memory is freed by this helper. The existing final endpoint/USB
lifetime barriers still apply. Temporary class diagnostics were removed.

Actual USB-core host fixture ordinary and ASan/UBSan PASS:
/tmp/zedbsd-q206-host.log. It verifies missing callback, remaining HCD owner,
endpoint shutdown error, owner appearing across shutdown, and successful final
retirement. Only endpoint hardware callback is mocked for these cases.

`temp/q206-high2/result.json` PASS: EHCI high-speed pending read established by
trace, attached=false, zero-byte read failure, complete disconnect, same-device
reattachment and fresh UAS binding, persisted 2 KiB readback, four-CPU checked
halt. Original boot/backing outside intended write unchanged. Trace, pending
snapshot, guest/argv/source manifest retained. /tmp/zedbsd-q206-high2.log.
q206-debug1 stopped before unplug because concurrent console output interleaved
the bind line. Harness now also accepts the unique QEMU UAS product's complete
port-configuration identity line; it does not guess a bus/address.

Explicit amd64 / PCAT / PC98 builds PASS: /tmp/zedbsd-q206-amd64.log,
/tmp/zedbsd-q206-pcat.log, /tmp/zedbsd-q206-pc98.log. diff check PASS.
Both supported UAS speeds now have pending-read disconnect/reconnect acceptance.
Full p029 remains uncleared for held-reference/replacement-media generations and
remaining uncertain-write/error acceptance. No replacement backing claim here.


## q207: published-media revocation on SCSI attention

Actual UAS command handling now always decodes Sense, including when the caller
omits the sense output. The shared SCSI classifier invalidates a published medium
on media absence/change, mode change, unowned reset or unknown/deferred attention.
Only reset attention from TUR inside this class's checked reset probe is exempt.
Unpublished initial probing remains separate. Both queued BIO entry and each
SCSI command check media status, so admission before revocation cannot issue a
later command onto a changed medium. Reset reprobe failure/mismatch also revokes
the published disk identity rather than only stopping transport. Write uncertainty
is not cleared. Replacement-media publication is not implemented by this change.

Actual-source class host tests ordinary and ASan/UBSan PASS:
/tmp/zedbsd-q207-host2.log. Cases cover media-change, absent, mode-change, unowned
reset, expected reset TUR, reset attention on unrelated inquiry, non-attention
command error, and queued BIO rejection with no extra transport command. Existing
probe-publication, failed-BIO/new-read and sticky-write tests remain passing.

Native SS reset regression `temp/q207-reset1/result.json` PASS: timed-out zero-byte
read, device reset, reprobe and correct new readback. Current attention restrictions
do not break the authorized reset path. Log /tmp/zedbsd-q207-reset1.log; source
manifest and QEMU trace/pcap retained. This is not a native media-exchange test.
Explicit amd64 / PCAT / PC98 builds PASS: /tmp/zedbsd-q207-amd64.log,
/tmp/zedbsd-q207-pcat.log, /tmp/zedbsd-q207-pc98.log. diff check PASS.

Full p029 remains uncleared. Next verify old held descriptors across physical
replacement and implement/verify new-medium publication after old references
retire; native uncertain-write/error cases also remain. Do not equate revoked
old admission with complete automatic media recovery.


## q208: held old descriptor across physical replacement, both speeds

Native `temp/q208-held1/result.json` (SuperSpeed xHCI) and
`temp/q208-held-high1/result.json` (high-speed EHCI) PASS. Test-only guest helper
is cross-compiled against the current amd64 sysroot and injected into a disposable
boot FAT partition (located from GPT/rootfs.img, partition 2 in these runs).
Guest copies it to /run; production root/userland remains unchanged.

The helper opens UAS O_RDWR, reads the old sector into cache and successfully
fsyncs before READY. Host disconnects/deletes the old physical UAS function and
adds a distinct backing, filled entirely with 0x5a, on the same USB port. While
holding the old fd, pread of the cached sector, pwrite at 1 MiB and fsync all
fail, yielding OLDREJECTED. The helper then waits with fd still held. Closing it
allows complete guest disconnect and fresh class publication. New fd readback
matches SHA256 219325ec03e898e5510ad21c78a41cbf80fca74c50f064bd872fb728d85704ef
for 2 KiB of 0x5a. Host verifies the entire new medium is still 0x5a, so the old-fd
write did not reach it. Both runs finish with checked four-CPU halt.

Boot source unchanged; old backing retains only the original intended raw write.
Guest/argv/helper binary/source manifest retained. Logs:
/tmp/zedbsd-q208-held1.log, /tmp/zedbsd-q208-held-high1.log.
No production changes or full rebuild needed. diff check PASS.

This proves physical USB replacement, cached old-read rejection and lifetime
retirement with a held open descriptor. It does not prove in-place removable
SCSI media exchange without USB detach, nor uncertain-write failure semantics.
Those remain in full p029. Observation for follow-up: xHCI's periodic retry logs
'driver detach pending' repeatedly while a legitimate fd is held because its
successful HCD barrier clears quarantine each pass; bound that diagnostic without
suppressing retries or weakening lifetime guards.


## q209: detach diagnostic independent of quarantine

USB device retains the last reported detach error under topology ownership.
The first error and a changed error are printed; unchanged class EBUSY no longer
prints each time the checked HCD barrier temporarily clears DMA quarantine.
Every periodic retry, ownership barrier and later disconnect report is retained.

`temp/q209-held1/result.json` PASS: SS old-fd physical replacement with two extra
seconds holding the old fd after OLDREJECTED reports exactly one pending notice.
Closing then permits complete disconnect, fresh backing readback and checked
four-CPU halt. Original/new backing invariants and boot source hash preserved.
/tmp/zedbsd-q209-held1.log; guest/argv/helper/source manifest retained.
Explicit amd64 / PCAT / PC98 builds PASS: /tmp/zedbsd-q209-amd64.log,
/tmp/zedbsd-q209-pcat.log, /tmp/zedbsd-q209-pc98.log. diff check PASS.

This resolves the repeated diagnostic observed q208. Full p029 remains uncleared
for in-place removable-media publication and remaining uncertain-write/error
acceptance; no additional physical-replacement coverage is claimed beyond q208.


## q210: native backend write EIO and persistent error, both speeds

`temp/q210-super2/result.json` and `temp/q210-high1/result.json` PASS. QEMU
raw->blkdebug->file injects one write-only EIO at sector 16384 (8 MiB). Test helper
pwrite returns -1 and fsync returns -1. A cold read at 12 MiB then succeeds with
unchanged 0xa5 bytes, but subsequent fsync and write at 16 MiB still fail.
Host backing remains exactly the original 0xa5 except the earlier intended 2 KiB
setup write. Source boot image unchanged; helper only in disposable image.

Reusable `audit-uas-write-error.py` passes both pcaps and writes write-protocol.json:
one WRITE(16) at LBA 16384, matching-tag CHECK CONDITION, and no WRITE(16) for the
later refused write at LBA 32768. This distinguishes actual write failure from
pre-write RMW failure and proves no automatic failed-CDB replay in this cell.
Guest/argv/config/helper/pcap/source manifests retained. Logs:
/tmp/zedbsd-q210-super2.log, /tmp/zedbsd-q210-high1.log.

Initial q210-super1 omitted blkdebug iotype, so its active rule also hit reads.
The helper's pwrite failed while fsync succeeded and the sticky test refused to
pass. Corrected injection uses iotype=write; no production fix was needed.
No full rebuild required; helpers compile against the current sysroot. diff check
passes. This is backend EIO/CHECK CONDITION, not partial DMA completion, write
transport timeout/reset, or crash durability. Full p029 retains those applicable
error cases and in-place removable-media recovery.


## q211: native timed-out write recovery without clearing write uncertainty

`temp/q211-super1/result.json` and `temp/q211-high1/result.json` PASS. Write-only
blkdebug delays the single EIO at sector 16384 by seven seconds, exceeding the
five-second transport deadline. Guest helper requires pwrite=-1 and ETIMEDOUT
(target errno 42), then fsync=-1. Cold read succeeds after recovery; subsequent
write and fsync remain errors. Both original backing and boot-source invariants
pass. This exercises transport timeout, not merely q210's CHECK CONDITION.

`audit-uas-write-error.py --timeout` passes both captures: failed WRITE(16) appears
once and later write is never sent. SS guest/trace proves port reset and pcap has
reprobe after failed command. HS capture has one ABORT TASK targeting the failed
write with a distinct management tag and matching TMF COMPLETE response. Thus
successful task/device recovery does not erase the earlier uncertain-write state.
write-protocol.json, trace/pcap, guest/helper/config/source manifests retained.
Logs: /tmp/zedbsd-q211-super1.log, /tmp/zedbsd-q211-high1.log.

No production changes/rebuild required. Helpers cross-compiled with current
sysroot. diff check PASS. Injected write ultimately does not modify backing;
this is not a claim of partial-media write or crash-durability testing. Full p029
still retains in-place removable-media publication and any acceptance gaps found
in the final requirement/evidence review.


## q212: reusable serialized medium publication

Extracted `uas_publish_media` from initial attach. It requires no existing disk
and validated nonzero geometry within transport capacity. Allocation/name/registry
failure releases the unpublished disk and leaves owner geometry/error state
untouched. Under the class disk mutex it registers candidate disk geometry, then
installs owner identity/policy and clears write uncertainty only for that new
identity, before newly admitted BIOs can acquire the mutex. Initial attach uses
the same helper; no existing disk is silently overwritten.

Actual-source host fixture ordinary and ASan/UBSan PASS:
/tmp/zedbsd-q212-host.log. It exercises allocation/name/registration failure,
old-owner preservation, successful new identity publication, and rejection of a
second publication over a live disk. Existing command/media/probe/error tests pass.
Native `temp/q212-publish1/result.json` PASS: SS publication, raw write/sync/readback
and persisted-byte invariants. /tmp/zedbsd-q212-publish1.log; source manifest kept.
Explicit amd64 / PCAT / PC98 builds PASS: /tmp/zedbsd-q212-amd64-final.log,
/tmp/zedbsd-q212-pcat.log, /tmp/zedbsd-q212-pc98.log. diff check PASS.

Full p029 remains uncleared. Next connect a class-owned, joined readiness worker
for removable SCSI media. Serialize readiness/revocation/old-disk retirement under
control and disk ownership, use this publication helper only after old references
retire, and preserve unavailable/failed media as closed. Attach readiness and
worker stop/join must be established before releasing any class resources. The
existing BOT control worker provides local lifecycle precedent; this queue does
not claim that monitoring or native in-place exchange is implemented.


## q213 — removable-medium control owner (2026-09-10)

Implemented an attach-ready worker for SCSI removable LUNs. Readiness sense
revokes the old disk; absent/change events permit retirement and reprobe only
after old partition/disk references are gone. New publication alone clears sticky
write error. Initially absent removable LUNs retain the class without publishing
invalid geometry. Attention during cache/persistence probing aborts publication.
Detach/quiesce first stop and join the worker without holding its mutexes; failed
join retains resources. Stopped transport remains closed, pending further recovery
work rather than unchecked reuse.

Evidence:
- Actual-source disk host fixture: medium attention, busy partition retirement,
  busy disk destruction, failed new allocation preserving sticky error, successful
  new publication. Actual detach fixture: live-worker timeout and failed join
  retain resources, successful join precedes destruction/free. Both runners pass
  normally and under ASan/UBSan with leak detection (outside ptrace sandbox).
- `temp/q213-removable2/result.json`: QEMU SuperSpeed `scsi-hd,removable=true`,
  initial publication/write/sync/readback and persisted backing PASS. Pcap contains
  13 TEST UNIT READY commands versus one INQUIRY, confirming periodic monitoring.
- `temp/q213-removable-lifecycle/result.json`: removable initial device removal,
  replacement publication/readback and four-CPU halt PASS. Replacement fixture is
  the existing fixed SCSI disk; this is not in-place removable-medium exchange.
- amd64, PCAT and PC98 disk-image builds PASS; logs
  `/tmp/zedbsd-q213-{amd64,pcat,pc98}.log`.
- First native launch failed before QEMU on a harness argument-name typo; corrected
  to `options.removable`, then used new disposable cells. No production workaround.

Full p029 remains uncleared. Resume with native in-place SCSI eject/change-medium,
initially empty LUN acceptance, partition rediscovery after replacement and bounded
recovery of a stopped removable transport. Existing physical replacement evidence
must not be substituted for these checks.


## q214 — native in-place removable medium exchange (2026-09-10)

Extended the native fixture with `--media-exchange`: QMP eject and
blockdev-change-medium on the existing SCSI backend, with USB continuously
attached. Read the previous cached offset after new publication and require the
new all-5A pattern. Initial backing retains only its intended setup write, new
backing remains unchanged, and boot source hash is preserved.

Both `temp/q214-super1/result.json` and `temp/q214-high1/result.json` PASS.
`audit-uas-media-exchange.py` also passes on both captures: exactly one USB class
binding, two disk publications, current NOT READY / medium absent sense followed
by UNIT ATTENTION / medium changed. Detailed sense sequences are retained in
`media-protocol.json` in each cell. This demonstrates in-place media exchange,
not physical USB removal. No production change or rebuild was needed after q213.

Full p029 remains uncleared. Next: initially empty removable LUN acceptance,
partition rediscovery after replacement, and bounded stopped-removable-transport
recovery. This test closes descriptors before exchange; held-reference rejection
has physical replacement evidence from q208, not yet in-place replacement proof.


## q215 — initially empty removable LUN (2026-09-10)

Added `--initially-empty` to the native UAS fixture. QEMU starts with an empty
removable SCSI backend. At the login prompt the fixture requires one UAS binding
and zero UAS disk publications, then inserts the disposable backing through
blockdev-change-medium. It requires first publication and the existing raw
write/sync/readback/persisted-byte checks.

`temp/q215-super1/result.json` and `temp/q215-high1/result.json` both PASS with
`empty_at_login=true` and unchanged source hash. Guest logs confirm exactly one
USB binding and one disk publication, after the initial login prompt. Existing
q213 kernel implementation passed unchanged; no additional build was needed.

Full phase remains uncleared. Next implementation: partition rediscovery outside
the command mutex after publication, and bounded recovery of stopped removable
transports while preserving medium identity/write uncertainty. Initial-empty
acceptance is now satisfied at both speeds.


## q216 — replacement partition discovery (2026-09-10)

New removable-medium publication now schedules partition discovery. The control
worker retains class lifetime, releases its command mutex, acquires the disk's
administrative open, invokes partition_reload, and closes that open on every
reload result. Temporary errors retain pending work; absent/unsupported partition
tables leave whole-disk I/O usable. Added synthetic geometry ioctl for existing
partition consumers.

The first native cell failed because partition_reload requires exactly one
administrative open (`disk_reload_idle`), which the initial implementation lacked.
A temporary diagnostic cell confirmed EBUSY before any table read. Added the open/
close pair and removed diagnostic output; no core admission rules were weakened.

Evidence:
- Actual-source disk host tests PASS normally and under ASan/UBSan: reload outside
  command lock, balanced administrative open, open failure and reload EBUSY retry,
  terminal no-table/unsupported handling, and geometry saturation.
- `temp/q216-super3/result.json` and `temp/q216-high1/result.json` PASS: replace
  whole medium with MBR medium (partition LBA 2048, length 32768), read 2048 bytes
  from `/dev/sdb1`, require distinct 5A hash and unchanged replacement bytes.
  Both protocol audits prove one USB binding, absent/change sense and two disk
  publications. Result text initially reused “cached-offset” from whole-disk mode;
  the authoritative `replacement_partition=sdb1` and guest command establish the
  actual partition test. Future result labels now distinguish the two modes.
- amd64, PCAT and PC98 disk-image commands exit 0, logs
  `/tmp/zedbsd-q216-amd64-final.log`, `/tmp/zedbsd-q216-pcat.log`,
  `/tmp/zedbsd-q216-pc98.log`. PC98 log includes a chmod warning for the staging
  installer launcher; final image target completes. Installer runtime was not
  retested by this UAS change.
- `git diff --check` PASS.

Full p029 remains uncleared for bounded stopped-removable-transport recovery and
final residual acceptance audit. A related static finding is that BOT's
storage_refresh_partitions also calls partition_reload without an administrative
open; that separate driver path needs follow-up, not a claim of native BOT proof.


## q217 — stopped removable transport recovery (2026-09-10)

Transport-stopping errors now revoke a removable LUN's old disk and schedule
retirement. The worker waits for old references to retire, then performs checked
stop, USB reset and transport initialization before probing/publishing a new disk.
Each failed barrier prevents later steps; failed recovery is not repeated by the
poll loop. Existing uncertain-write state remains until new identity publication.
No failed BIO is replayed and capacity equality does not certify removable identity.

Evidence:
- Actual-source host tests PASS normally and under ASan/UBSan: live-disk recovery
  refusal, stop/reset/init failure ordering, suppression of repeated failed
  recovery, retained write uncertainty, and timeout-triggered media revocation.
- `temp/q217-super3/result.json` and `temp/q217-high1/result.json` PASS: throttle a
  cold READ, require zero returned bytes, then checked USB reset, fresh disk
  publication and successful explicitly issued READ. Protocol audit in runner
  requires reset count increase, INQUIRY between failed/new READ, exactly those
  two READs at the test LBA and no ABORT in this generation-retiring reset path.
  Boot source and backing bytes are preserved. SS failed-read wait was 5.21 s.
- The removable path may surface `Unknown error` from media revocation instead
  of the transport timeout string. For that case the fixture requires at least
  4.5 s post-command wait in addition to zero transfer and the reset/readback
  evidence; it does not accept any generic immediate error as a timeout.
- amd64, PCAT, PC98 disk-image commands exit 0, logs
  `/tmp/zedbsd-q217-{amd64,pcat,pc98}.log`. Diff whitespace check PASS.

Earlier cells retained: super1 expired waiting for Password after login input,
without reaching fault injection. No production change was made for this input
failure; super2 reached injection but failed the old exact error-string oracle.
Added failure-register capture for future stalled cells, then strengthened the
removable error oracle as above. These failures are not silently marked PASS.

Full p029 remains uncleared pending final requirement/evidence audit, including
which asynchronous/SG/recovery cells apply to the depth-one class and any missing
write-uncertainty/held-reference evidence. Do not infer full phase completion from
the successful read-recovery cells alone.


## q218 — audit and mounted media exchange (2026-09-10)

Inspected current class/transport source, actual-source host assertions and retained
native result.json files listed in acceptance-audit.md. Added --mounted-exchange
using the existing UFS fixture, without kernel changes or blanket test execution.

`temp/q218-mounted-super1/result.json` FAIL: normal UFS write/fsync/remount/hash
succeeded; after in-place eject/change-medium, the old cached file read correctly
fails ENXIO and no replacement disk is published. `umount /run/uas` also fails
ENXIO and mount listing retains the old UFS. The fixture correctly stops there.
No successful replacement readback is claimed. Source image remained unchanged.

Current unmount performs writeback/VM sync and filesystem sync/prepare-unmount;
UFS prepare attempts a clean-superblock write. Therefore merely ignoring ENXIO
would weaken data-loss reporting and owner retention. This is BUG-021 and the
next design boundary, not a justification to weaken the test or force completion.
Queue finished uncleared. No production patch or build was needed for this audit.


## q219 — lost-medium design and BOT prerequisite (2026-09-10)

Saved [explicit revoked-medium teardown design](lost-media-teardown.md). Ordinary
unmount retains durability errors; a future explicit force operation is restricted
to irrevocably revoked disk-backed mounts and requires quiescence, no external
owners, failure-atomic cache/dirty ownership disposal, and local filesystem teardown.
No force-unmount implementation or BUG-021 resolution is claimed yet.

Fixed the independent BOT storage_refresh_partitions omission identified in q216:
acquire administrative disk_open outside command mutex, reload, always close;
failed open retains pending work without reload. Strengthened existing shared BOT
host fixture to enforce the real open count; storage-reserve-host now checks failed
open and balanced close alongside media replacement/retry.

Validation: storage-reserve-host actual-source fixture PASS ordinary and
ASan/UBSan/leak detection (manual cc command using -std=c11, include/uapi and
src/kern/io.c). Three supported disk-image builds exit 0, logs
`/tmp/zedbsd-q219-{amd64,pcat,pc98}.log`. `temp/q219-boot2/result.json` PASS for BOT
boot and separate SS UAS raw I/O/persisted backing, unchanged source. This does not
claim native BOT media replacement. Diff whitespace check PASS.

Earlier q219-boot1 reached login but the fixture immediately rejected absence of a
UAS publication line. Since enumeration can proceed asynchronously, changed that
oracle to a bounded 45-second wait; boot2 passes. Do not infer the first device
would have eventually attached, or that BUG-017 is fixed, from this later pass.

Full p029 remains uncleared. Next: implement design stage A (revocation eligibility
and reversible no-write worker boundary), then staged cache/filesystem commit.


## q220 — reversible revoked-media writeback pause (2026-09-10)

Added writeback_unmount_begin_revoked, separate from ordinary begin. It requires a
non-null retained disk with revoked media-chain admission, joins/pauses the worker,
rechecks eligibility, and publishes the existing reversible token without issuing
sync. The ordinary path still synchronizes and propagates errors. Dirty credits,
mount references, worker errors and rollback behavior remain owned as before.
Header documents that a future caller must dispose dirty owners before commit;
there is no public force-unmount caller yet.

Validation: `temp/q220-writeback3` actual-source concurrent worker test PASS ordinary
and ASan/UBSan/leak detection. Added live/null refusal, no-sync dirty retention,
shared-worker exclusion, unrelated-worker progress and rollback/admission checks.
Ordinary unmount sync-failure and lifecycle cases remain in the same fixture.
Build/source/commands and variant logs are preserved in that cell. All three
supported disk-image commands exit 0: `/tmp/zedbsd-q220-{amd64,pcat,pc98}.log`.
Diff whitespace check PASS. No native force-unmount claim is made.

The first two host attempts exposed stale test wiring: removed writeback-policy.c
and io-scratch.c source names, then a duplicate writeback_domain_acquire stub.
Updated runner to merged writeback.c/io.c and stubbed disk_cache_acquire below the
actual domain resolver. Source implementation was not reverted to old structure.

Stage A's writeback boundary is implemented; mount admission, VM/cache discard,
filesystem preparation and syscall/command integration remain. BUG-021 and p029
stay uncleared. Resume with design stage B's failure-atomic cache/dirty ownership,
then connect the public operation only after complete commit/rollback exists.


## q221 — revoked buffer discard preflight (2026-09-10)

Before the first eviction, buf_discard_media checks every buffer on the revoked
physical disk for retained references, busy state and inflight I/O using existing
cache/buffer/dirty-index lock order. A busy buffer now refuses without first
throwing away other idle dirty buffers. The caller must still exclude new external
users for the whole operation, as required by the preexisting API. This is not
an atomic cross-layer mount transaction or a new concurrency reservation.

Extended existing buffer-media-host with two dirty buffers, one pinned and one
releasable. EBUSY preserves both dirty flags, buffer count and dirty-byte count,
and performs no backend writes; releasing the pin permits complete discard,
zero dirty accounting, and idempotent repeat. `temp/q221-buffer1` passes ordinary
and ASan/UBSan/leak detection, including inherited buffer count/prefix/pressure/
concurrent-writer checks. Updated the old runner's io-stats.c reference to merged
io.c; no source structure was restored to satisfy a stale test.

All three disk-image builds exit 0 (`/tmp/zedbsd-q221-{amd64,pcat,pc98}.log`);
diff whitespace check PASS. No native forced-unmount claim is made. Full phase
and BUG-021 remain uncleared: VM object eligibility/dirty disposal, mount admission,
filesystem finalization and the explicit public operation remain to implement.


## q222 — revoked-mount VM ownership preflight (2026-09-10)

Added vm_object_discard_mount_check: only a non-null revoked disk-backed mount is
eligible. Registry/object locks protect inspection of every associated object;
retained cache/writeback ownership must be sole ownership, with no mapping,
operation, waiter, resize/content/detach transaction. Both live and orphan pages
must be free of hold/pin/mapping/busy/writeback owners. The function never changes
dirty indexes, writeback error, refs or registry membership. It is a preflight, not
an admission gate: the future mount transaction must exclude new users through
commit and coordinate buffer/filesystem checks.

Actual vm.c focused fixture `tests/vm-discard-host.c` PASS ordinary and
ASan/UBSan/leak detection; it creates registry/object/page descriptions and uses
real preflight code with mocked locking/media status. Checks include eligibility,
external object refs, mapping/active/waiter ownership, dirty/error preservation,
orphan pin refusal and unrelated mount isolation. It does not claim real mmap
concurrency or end-to-end forced unmount. Three disk-image builds exit 0:
`/tmp/zedbsd-q222-{amd64,pcat,pc98}.log`; diff whitespace check PASS.

Initial attempts to use the older full file-cache runner did not link: first tid_t,
then duplicate merged VM/readahead/writeback stubs and missing HAL/swap collaborators.
Logs retained in `/tmp/zedbsd-q222-host{,2}.log`. Restored this turn's edits to that
older fixture/runner and used the focused actual-source check instead; no production
symbol was weakened for the old test. Broader legacy harness repair belongs to
WS026 rather than a claim of full file-cache regression success here.

Full p029/BUG-021 remain uncleared. Next: staged reservation/commit of eligible VM
objects with dirty-budget disposal, coordinated under closed mount admission with
buffer and filesystem preparation; then syscall/umount integration and native
mounted-medium acceptance.


## q223 — revoked VM discard commit (2026-09-10)

Implemented vm_object_discard_mount after caller-owned admission closure and
cross-layer preflight. It rechecks every matching VM object under the registry
lock before mutating any, discharges live/orphan dirty credits using the existing
index/accounting helper, retires shared/cache registry identity, and destroys
frames, descriptors and file references outside the registry lock. It returns
discarded dirty bytes for the eventual explicit-loss diagnostic. A busy later
object leaves earlier objects untouched. This internal API is not yet called by
public unmount and does not itself close mount admission.

Focused actual-vm.c fixture passed ordinary execution in the preceding q223 work
and ASan/UBSan with leak detection in this continuation (exit 0). The commit test
checks whole-set refusal, dirty byte/credit accounting, two page releases plus
metadata slab release, object/file release outside registry lock, unrelated
registry identity retention and repeated empty discard. Locking, HAL storage and
file finalizers are mocked: this is not concurrent mmap or native forced-unmount
acceptance. The final test-only edit clarifies its PASS label.

PCAT and PC98 disk-image builds exited 0, logs
`/tmp/zedbsd-q223-pcat.log` and `/tmp/zedbsd-q223-pc98.log`.
The earlier amd64 process was confirmed absent and its completed image/log found;
because the original exit status was unavailable, a subsequent incremental build
confirmed exit 0 (`/tmp/zedbsd-q223-amd64-confirm.log`).
`git diff --check` passed. No aggregate tests or new native scenario were run.

Full p029 and BUG-021 remain uncleared. Resume with closed mount admission and
external-versus-internal mount/inode ownership checks, filesystem revoked teardown,
then public umount integration and the existing mounted-medium exchange scenario.
No dirty discard may be exposed before all failure-capable cross-layer checks.


## q224 — UFS revoked-unmount preparation (2026-09-10)

Added optional filesystem_type.prepare_unmount_revoked with a non-mutating,
no-backend-I/O contract. NULL leaves other filesystems unsupported. UFS checks
initialized mount/disk state and explicit revocation; retained snapshot devices
and journal image readers refuse with EBUSY. No clean-superblock write occurs in
this entry. Ordinary prepare_unmount and unmount finalization are unchanged.
The callback is registered but not yet invoked from public unmount.

`sh plan/ws025/tests/run-ufs-revoked-host.sh` passes ordinary and
ASan/UBSan/leak checks (exit 0). The fixture includes actual ufs.c and retains only
the preflight dependency graph via section GC; disk status is mocked. It checks
null/uninitialized/live refusal, writable/read-only revoked eligibility,
snapshot and open/closed journal-reader refusal, closed-without-readers acceptance,
and refusal nonmutation. Initial compile lacked POSIX host feature exposure;
adding _POSIX_C_SOURCE=200809L to the runner fixed host clock/stat declarations.
No production symbols were changed for the fixture. This is eligibility evidence,
not native forced-unmount or ordinary durability regression acceptance.

Integration review identified retained UFS cg_view buffer/disk refs and retained
VM file/inode refs that require explicit internal-owner accounting before discard.
Recorded in lost-media-teardown.md. Full p029/BUG-021 remains uncleared pending
closed admission, all-owner preflight, disposal ordering and public/native testing.

Final amd64, PCAT and PC98 disk-image builds all exited 0 after journal-reader
refusal was added: `/tmp/zedbsd-q224-{amd64,pcat,pc98}-final.log`.
Diff whitespace check passed. No aggregate test or native QEMU run in this queue.


## q225 — retained VM file references (2026-09-10)

VM discard preflight now checks file-description refs, not only VM object refs.
An external FD sharing a retained read/write description now causes EBUSY before
any dirty/error disposal. Description mount/inode identity must match the object;
unproven cross-object or cross-mount sharing is conservatively refused. The two
slots may share one file with f_refs==2, or hold distinct private files with one
ref each. New vm_object_discard_mount_refs reports one path for the aliased case,
two for distinct descriptions, and can select one inode. Its output is zero on
refusal and requires caller-owned admission closure throughout subsequent use.

Added reproducible `tests/run-vm-discard-host.sh`. Ordinary and ASan/UBSan with
leak detection pass, including external-file-owner refusal/nonmutation, alias and
distinct internal path counts, wrong mount/inode refusal, and the previous actual
VM dirty-disposal/resource-order checks. The fixture includes actual vm.c but
mocks locking/storage/file finalization; no concurrent FD or native unmount claim.

Full p029 remains uncleared. Mount/inode code must use these proven internal counts
without acquiring VM registry beneath inode-cache spinlock, then account for UFS
buffer pins before all-owner commit. Public operation/native acceptance remain.

amd64, PCAT and PC98 disk-image builds all exited 0:
`/tmp/zedbsd-q225-{amd64,pcat,pc98}.log`. Diff whitespace check passed.
No aggregate tests or native QEMU run were needed for this unconnected API stage.


## q226 — revoked inode ownership preflight (2026-09-10)

Added inode_cache_mount_revoked_check for a reserved DYING attachment. It first
checks complete VM eligibility, including empty inode-cache cases. Each matching
inode is temporarily pinned under cache lock; VM internal-path counting runs
outside that lock; then cache/root/namespace/VM ownership and the temporary pin
must exactly explain total refs. Refusal includes overflow and unexplained refs.
The pin is dropped directly without dead-inode reclamation, preserving dirty flags
and cache identity. Normal inode_cache_mount_busy remains unchanged. This API still
requires caller-owned closed admission and has no public unmount caller yet.

`sh plan/ws025/tests/run-inode-revoked-host.sh` passed ordinary and
ASan/UBSan/leak detection. Actual inode.c is included with controlled VM/lock
collaborators; assertions verify VM calls occur outside inode-cache lock, temporary
pins are held during VM inspection and released after helper error, internal and
external counts, root/namespace ownership, wrong state, overflow and nonmutation.
It does not claim native concurrency or combined real VM/VFS teardown coverage.

amd64, PCAT and PC98 disk-image builds exited 0:
`/tmp/zedbsd-q226-{amd64,pcat,pc98}.log`. Diff whitespace check passed.
No aggregate tests or native QEMU run in this internal preflight queue.

Full p029 remains uncleared. Integration review found UFS zero-link reclaim must
be suppressed at explicit irreversible commit before file/inode release. Whole-leaf
buffer disposal can remain with existing disk_media_retire after all attachments
release ownership, rather than introducing a failing shared-disk discard inside
mount commit. Details and revised ordering are in lost-media-teardown.md. Next:
filesystem no-I/O commit plus dirty inode disposal, then namespace/public operation
and mounted-medium native acceptance.


## q227 — no-I/O filesystem/inode commit (2026-09-10)

Added paired filesystem commit_unmount_revoked. UFS requires successful revoked
preflight and DYING admission, then closes journal views and disables local
writable state. This suppresses the existing ufs_reclaim zero-link backend path
before any final file/inode release; normal unmount and reclaim are unchanged.
Added inode_cache_discard_mount_dirty for the admitted revoked mount: it clears
only INODE_DIRTY and returns an affected-inode count, preserving error state,
other flags, references and unrelated mounts. Invalid commit invariants are fatal,
not partial-success errors. These internal operations have no public caller yet.

The UFS and inode focused actual-source runners pass ordinary and ASan/UBSan/leak
detection. UFS checks only writable and journal admission state change, with
idempotent repeated local commit. Inode checks dirty count, preservation of other
flags/state/references, unrelated-mount isolation and repeated empty discard.
Existing refusal and ownership tests remain in those runners. This proves the
local operations; native final file/inode reclaim is still to be exercised through
public mount integration. The ufs_reclaim writable guard was inspected directly.

Full p029/BUG-021 remain uncleared. Next queue can combine the completed internal
operations with closed mount admission, reversible preflight/rollback and the
explicit unmount flag; native mounted-medium exchange remains the acceptance gate.

Three disk-image builds exited 0: `/tmp/zedbsd-q227-{amd64,pcat,pc98}.log`.
Diff whitespace check passed. No aggregate tests or native QEMU run in this queue.


## q228 — public revoked UFS unmount (2026-09-10)

Implemented shared MNT_FORCE, root-only syscall flag validation, context-aware
flagged unmount and `umount [-f] [--] directory`. Ordinary zero-flag behavior remains
on the existing synchronization path. Explicit force requires revoked disk-backed
filesystem support, lifecycle/VM providers, and no root/private/bind/child mount.
It pauses readahead/writeback without sync, reserves DYING namespace admission,
purges namecache, validates exact mount/inode/VM ownership and UFS preflight, then
commits local filesystem/inode/VM disposal. Every precommit refusal restores LIVE
and worker policy. One loss diagnostic reports VM dirty bytes and dirty inodes;
success means disposal, never durability. Existing disk retirement handles physical
buffer discard and replacement publication after old users release references.
The boot/storage guide describes the operation and restrictions.

Integration review found global registry-driven VM reclaim could add an owner after
namespace preflight. Whole-object eviction, clean/dirty page reclaim and unscoped
sync now skip DYING mount objects. Scoped sync remains available for ordinary
teardown. `run-vm-discard-host.sh` passes ordinary and ASan/UBSan/leak detection,
including reserved clean-page/whole-object reclaim refusal and all prior tests.

Native evidence:
- `temp/q228-mounted-super1`: PASS ordinary UFS write/fsync/remount/hash, old cached
  read ENXIO after in-place exchange, ordinary unmount refusal, force success,
  old attachment removal, replacement publication/readback and unchanged full
  replacement backing. Dirty VM/inode counts are zero in this case.
- `temp/q228-mounted-super2`: additionally PASS live-medium force refusal and
  retained-cwd EBUSY. After `cd /root`, retry succeeds and replacement readback
  passes. This run precedes the global VM reclaim admission refinement.
- `temp/q228-mounted-high1`: FAIL before media exchange/force, at mkfs confirmation
  completion wait (90 seconds). Retained register snapshot maps CPU0 RIP to
  drv_usb_urb_wait. UAS capture ends at WRITE(16) tag 0xfa data-out completion with
  no subsequent status request. Root cause is unproven; BUG-022 records bounded
  EHCI completion/URB deadline follow-up. Do not call this force-path acceptance.

Final builds after VM admission refinement exit 0:
`/tmp/zedbsd-q228-amd64-reserved.log`, `/tmp/zedbsd-q228-pcat-final.log`,
`/tmp/zedbsd-q228-pc98.log`. Diff whitespace check passes; no aggregate tests.
Full p029 remains uncleared pending deliberately retained dirty-state acceptance,
remaining ownership cases and High-Speed completion recovery/acceptance.

Final `temp/q228-mounted-super3` after the VM reclaim admission refinement also
PASS: normal UFS fsync/remount/readback, live force refusal, revoked old read
rejection, ordinary unmount refusal, cwd EBUSY and rollback, release/retry success,
new publication/readback and unchanged replacement/source image. This is the final
native evidence for the q228 binary. Dirty VM/inode diagnostic counts remain zero.


## q229 — cooperative synchronous USB wait (2026-09-10)

q228's saved RIP is the pre-deadline status polling loop in drv_usb_urb_wait.
The loop had no scheduler handoff until timeout, whereas EHCI terminal publication
uses a retirement worker. Pending waits now yield, leaving status decoding,
checked cancellation and HCD ownership intact. This removes a concrete scheduling
hazard; a single prior register snapshot does not prove every source of stalled
clock/completion delivery.

Added actual-usb.c `usb-wait-host.c` / `run-usb-wait-host.sh`: finite/no-timeout
completion requiring a scheduler handoff, already-complete return, terminal STALL,
and bounded EBUSY cancellation retaining PENDING/HCD ownership. A read-count guard
makes the old non-yielding loop fail instead of silently hanging this fixture.
Normal and ASan/UBSan/leak checks pass after isolating the fixture's sched_yield
symbol as test_usb_sched_yield. Initial sanitizer execution with the unrenamed
mock stalled in futex wait; its exact owned process was terminated (exit 143),
then the corrected fixture passed. This was a test/runtime symbol collision, not
production completion evidence.


`temp/q229-mounted-high1` PASS with the production wait change: High-Speed UFS
format/write/fsync/remount/hash, live force refusal, revoked cached-read and ordinary
unmount refusal, retained cwd EBUSY, successful force after cwd release, new-medium
publication/readback and unchanged replacement/source images. This reaches beyond
the q228 formatter stall. VM/inode dirty-loss counts are zero; deliberately retained
dirty-state acceptance remains separate.


`temp/q229-mounted-high2` independently repeats the same complete High-Speed
scenario and PASS, including final replacement hash and unchanged source image.
The observed formatter-stall path is treated as fixed for this bounded evidence;
retain q228's failure rather than claiming exhaustive EHCI scheduling proof.
Three builds exit 0: `/tmp/zedbsd-q229-{amd64,pcat,pc98}.log`.
Diff whitespace check passes. No aggregate tests were run.
Full p029 remains uncleared for dirty-retained and remaining ownership acceptance;
High-Speed clean mounted-medium recovery is now native-accepted alongside SS.


## q230 — retained dirty acceptance and p029 completion (2026-09-10)

Added `uas-dirty-guest.c` and --dirty-exchange. Using the existing public writeback
control/report interface, the helper overwrites one preallocated page without
fsync and reports actual dirty credits before host-side eject. It keeps an FD
across exchange, checks old read/fsync rejection and force EBUSY, closes it and
requires the 4096 dirty bytes to remain. The shell then tests ordinary refusal,
cwd refusal/retry, explicit force, positive dirty-disposal diagnostic, replacement
publication/hash and unchanged entire replacement backing.

`temp/q230-dirty-super1` and `temp/q230-dirty-high1` both PASS:
dirty_before_loss=4096, dirty_after_close=4096, dirty_discarded=4096, source_unchanged.
The High-Speed case additionally queries post-disposal public accounting:
mounts/workers/busy/dirty/reserved/tickets/memory all zero. Historical writeback
errors remain observable (errors=10), as expected after loss; disposal does not
rewrite them into successful persistence. SuperSpeed did not include that added
post-disposal report assertion; its positive dirty-disposal/republication evidence
and the shared accounting code's focused checks are retained without overstating it.

No production code changed in q230, so the q229 three-platform build gates remain
applicable. The guest helper compiled for the current amd64 sysroot; runner syntax
and diff whitespace checks pass. No aggregate tests were run.

Completion audit: re-read applicable ASYNC02–07, SG02–04 compatibility, REC01–06
and FLUSH04 criteria and the depth-one scope. Inspected the saved result.json files
for q203 UFS fsync/remount (HS/SS), q205/q206 inflight detach/halt, q208/q209 old-FD
rejection, q210 failed writes, q211 timed-out writes, q214 in-place media exchange,
q215 empty-LUN insertion, q216 partition rediscovery, q217 reset/read recovery,
q228/q229 clean force/cwd recovery and the two q230 dirty cases. All named positive
records report PASS and unchanged source. Re-read q210/q211 write-protocol.json
(no replay) and q214/q216 media-protocol.json (absent/change sense, two publications,
one USB binding); q216 records identify replacement sdb1. Current transport bounds,
reserved-buffer use, wrong/late-tag/cancel/drain fixture assertions and the
revoked-unmount ownership checks were also inspected.

The missing mounted-dirty recovery identified in q218 is now implemented and
native-accepted. p029 is completed/cleared under the user's QEMU-only decision.
The implementation remains depth one, LUN 0, HS READY / SS coordinated streams,
with staged buffers. No direct-SG performance, multi-command queueing, arbitrary
hub topology or real hardware acceptance is claimed or required by this selected
phase. Preserve prior failed cells as history. Next Priority work: p028 and p030;
physical PC98 p032 remains separate.
