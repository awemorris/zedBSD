# ws025-p029: 条件付き UAS driver

日付: 2026-09-07

Phase ID: `ws025-p029`

## 最新の受け入れ方針（2026-09-09、ユーザー指定）

実機UAS機器がないため、QEMU `usb-uas` での動作確認をもってclearedにする。
実機試験は完了の必須条件ではない。下記q122/q125などの実機先行条件は
履歴であり、実装や完了を妨げる条件にしない。

q230でQEMU受け入れを完了した。HS READY／SS streamによるdepth-one UAS、
read/write/fsync、timeout/reset/cancel、再接続・媒体交換、旧参照の拒否、
dirtyデータを残した媒体喪失後の強制解除と再公開まで確認済み。
[最終結果と受け入れ監査](results.md#q230--retained-dirty-acceptance-and-p029-completion-2026-09-10)を参照。
再開したp032の実機固有問題は、既に通ったPC98 QEMU gateを取り消すものでは
なく、独立したUAS実装を実機待ちで停止させない。

## q144 descriptor and transport contract

q145 implementation: introduce a pure `drv_usb_uas_decode_configuration` in
`src/drivers/usb/usb-uas.c` and a small capability header. Parse only the requested
interface/alternate, preserve endpoint/pipe association, require four unique
bulk endpoints with correct direction and high/super-speed packet/companion
rules. Reject truncated lengths, duplicate selected alternates, missing/duplicate
pipes and inconsistent stream capabilities. Publish output only after complete
validation. No allocations, transport registration or endpoint activation.
Host tests compile production source against captured descriptors and malformed
variants under ordinary and ASan/UBSan builds; run explicit three-platform builds.

Result and next implementation: [captured capabilities and transport design](transport-design.md).

Boot the current amd64 image on disposable USB BOT media, and attach a separate
QEMU `usb-uas` with an explicit blank `scsi-hd` LUN 0. Capture control transfers
using QEMU's per-device pcap facility. Repeat with UAS on EHCI (high speed) and
xHCI (super speed). Decode complete device/configuration replies, preserving
endpoint/pipe-usage association and SuperSpeed companion MaxStreams. Require
zedBSD's actual enumeration log, not merely firmware enumeration or QEMU help.

Record unsupported-driver status truthfully. Design the initial depth-one
command/status/data state machine, READY-vs-stream behavior, tag retirement,
timeout/reset, and descriptor rejection before production implementation. This
bounded queue establishes the backend and design; it does not complete p029.

Status: completed / cleared (q230); QEMU HS/SS受け入れと最終証拠監査完了。

Parent: [WS025](../ws.md)

依存: ws025-p019、ws025-p024、QEMU UAS backendと実取得descriptor

追加の先行条件: [ws025-p031](../phase031-driver-layout-style/phase.md) のドライバ整理を完了してから実装する。下記の旧ソースパスは p031 の移行表で解決する。既存の採用条件は維持する。

追加の回帰 gate: [ws025-p032](../phase032-pc98-boot-regression/phase.md) の PC-98 QEMU 起動回復を完了してから実装へ進む。

## 現在の選択

ユーザーがp027〜p030を次に実施する最優先項目として選択した。
旧見送りを継続する扱いではなく、p031残件・現ソース・下記の測定/実機条件を確認してQueue化する。
今回の計画整理で実装や測定を実施済みとは扱わない。

## 目的と境界

BOT と別 owner の UAS driver を実装する。

## 変更対象

- `src/drivers (新 UAS owner)`
- `include/drivers/usb.h`
- `src/drivers/pci-xhci.c`
- `src/kern/disk.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. QEMU UASの protocol/interface/pipe/stream capability を記録し、対応対象を固定する。印字された製品名だけで決めない。
2. command/status/data の owner、tag/stream、task management、queue 上限を設計し、descriptor parser と同期 depth 1 から実装する。
3. USB/disk の既存 generation/async/SG 契約に接続し、reset/cancel/timeout/out-of-order を受け入れてから多重化する。
4. 選択失敗時の BOT fallback が device の interface 切替え契約内で可能か確認し、live DMA を残して切替えない。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- 専用 descriptor/protocol fixture、ASYNC/SG/REC/FLUSH の該当セル、QEMU UAS read/write/fsync/再接続。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

QEMUでtransportを実装・検証する。実機未入手を停止条件にせず、BOT並列化で代用しない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。

## q122 adoption decision

Not adopted in the mandatory WS025 implementation. Obtain target UAS descriptors and stream/pipe capabilities with a concrete hardware acceptance path.
See [effective policy and evidence basis](../phase026-integration-defaults/effective-policy.md).
This records the conditional decision, not completion of this optional phase.

## q125 現ソースに合わせた詳細化

ユーザーのPriority全件自走指示により着手条件を確認。完了できないPhaseはunclearedとし他WSへ進む。

対象: src/drivers/usb/（新UAS owner）、include/drivers/usb.h、src/drivers/pci/pci-xhci.c、src/kern/disk.c。

現状: 直近実機はWLANとホストUSB Ethernetであり、UAS storageのdescriptor/stream能力は取得していない。現在テスト機SSHがtimeout。

設計手順: (1) interface protocol=UASとcommand/status/data pipe usage、MaxStreamsをdescriptorから固定。(2) parser→depth1 command/status/data owner→cancel/reset/task-managementの順で実装。(3) tag/streamはdrain確認まで再利用せず、old generationのstatusを拒否。(4) 実機read/write/fsync/切断再接続が通ってから並行化し、BOTへのlive切替えは行わない。

今回の未クリア理由: 対象UAS機器とdescriptorが未確認。現Phaseの実機先行条件を満たさず、架空descriptorに合わせたdriverは実装しない。

再開条件: UAS対象のdescriptorと利用可能な実機試験経路。

旧停止節のplanned維持はq122時点の判断。今回の実行結果はunclearedとして扱う。

## 現在の実行条件（q140中の読み取り確認、2026-09-09）

上記q122/q125の実機先行条件は履歴であり、後続のユーザー指示により
実装の停止条件にはしない。ローカルQEMUの公式同梱文書
`/usr/share/doc/qemu-system-common/system/devices/usb.html` は `usb-uas` と
明示的な `scsi-hd,bus=uas.0,scsi-id=0,lun=0,drive=...` の構成を説明している。
usb-uas単体ではディスクを作らない。使い捨てbackingとSCSI LUNを明示して
descriptor/pipe/stream能力を取得するところから次の有限Queueを始める。

現ソースにはSuperSpeed companionのdecodeはあるが、USB API/HCDにUASの
stream/tag所有者は未実装。descriptorを取得せずストリーム不要と決めない。
QEMUが検証に使えるかを実構成で確認してから、depth 1の別transport owner、
command/status/dataとcancel/quiesceの順に設計する。実機がないだけでFutureに
移さず、QEMUでの検証が利用不能と確認された場合のみユーザー指定どおり移す。

## q191: high-speed protocol engine

Implement command IU encoding for LUN 0..255 and CDBs up to 16 bytes, with
nonzero tags and explicit expected transfer direction/length. Decode READY and
Sense IUs against one command state: only matching-tag, correctly directed READY
permits data; only one data phase is allowed. Validate complete status framing,
sense length and qualifier before publishing completion. Short data may precede
CHECK CONDITION, but GOOD requires the expected byte count. Any framing/state
error poisons the command until the future owner retires host and device work.
Use byte access, not packed native structs. QEMU dev-uas.c is layout evidence,
not imported source. Focused tests exercise wire bytes and invalid transitions.
This does not yet register a device or discharge the full p029 acceptance.

## q192: USB endpoint transport

Use four endpoint-specific reserved synchronous URBs, allocated before I/O.
One serialized owner passes endpoints in validated Pipe Usage order; require
high speed and bulk direction/packet constraints. Each command shares a finite
monotonic deadline across command, status and data. Return SCSI status and bounded
sense bytes separately from transport errors. Any USB/protocol failure closes
admission permanently until device-task recovery is implemented; never reset
state merely because host URBs are drained. Stop closes admission and checks all
URB retirement before freeing any resource, retaining the entire transport on
failure. Test sequence, short command, errors at each endpoint stage, exhausted
deadline, failure admission and failed drain. This does not yet register a disk.

## q193: high-speed disk class

Connect validated descriptor pipes to the synchronous owner. Probe LUN 0 direct
access, readiness, capacity and MODE SENSE/cache policy, SYNCHRONIZE CACHE before
publication. Share SCSI policy codecs with BOT but keep class ownership distinct.
Serialize BIO/flush and class stop. Preserve removable/generation contracts and
retain failed-stop owners. Begin high-speed matching; SuperSpeed remains rejected
until stream support exists. QEMU high-speed attachment is the first native test.

Correction to q191: allocation-length SCSI responses may be shorter than requested
with GOOD status. Require READY/data before GOOD for a data command, but return
actual length and let the SCSI caller enforce block transfer exactness. Host
fixtures must reflect this protocol rule, not make MODE SENSE always exact-size.

## q194: native idle detach/replug and shutdown

Extend the accepted raw-I/O cell with QMP removal of the UAS function after
commands finish and handles close. Require the matching guest bus/address/port
disconnect marker, recreate UAS plus its SCSI LUN on the retained backing, require
fresh disk publication, then reread and hash the persisted bytes. Halt with UAS
still attached: require all four CPUs HLT=1 and IF=0 in three consecutive samples,
with no shutdown failure/panic. This covers idle lifetime, not in-flight or held
open descriptors. Original full phase remains unchanged.

## q195: high-speed task abort recovery

Record the failed command tag/LUN and retain preallocated URBs. Before a new BIO,
try recovery once: cancel and drain all host URBs, then send ABORT TASK on a tag
different from the failed command. Require exact Response IU and TMF COMPLETE.
At most eight queued old-tag READY/Sense IUs may precede the response; reject
foreign tags/malformed lengths. Failed recovery stays closed; stop disables it.
Never reuse the management tag after failure. On successful task retirement,
advance tags and allow a new command, never replay the failed BIO. Partial or
uncertain writes and flush errors remain sticky across recovery. Host tests
inject drain/response errors, stale tags, queued status and timeout.

## q196: native timeout/abort

Place a throttle block filter only on the UAS backing. After normal I/O, set low
read bandwidth via QOM. Prime its token accounting then request a distinct cold
sector, require guest read failure, restore unlimited bandwidth and issue a new
read. Capture command/response IUs to establish ABORT TASK uses a distinct tag,
retires the timed-out tag, and precedes successful new-command readback. Preserve
original image and untouched backing bytes. This is read-timeout recovery, not
an uncertain-write or media-generation acceptance substitute.

## q197: explicit URB stream identity

Add nonzero stream setup with explicit HCD capability and SuperSpeed bulk /
companion checks. Failed setup must leave the old URB configuration unchanged;
ordinary setup resets stream ID to zero. HCD validates configured stream/ring
identity again at enqueue, while USB core retains its existing lifetime barrier.
No current HCD advertises capability until its stream context, completion and
cancel paths are implemented. Test actual USB core setup on idle/pending/HCD-owned
URBs, unsupported HCD/speed/type, invalid IDs, and normal/control reuse.

Stream実装の接続箇所と所有権: [stream integration](stream-integration.md).

## q198: primary stream rings

Implement four-entry primary context arrays (IDs 1..3, zero invalid), explicitly
requested with interface I/O closed under selection/control/binding lifetime.
Each accepted request retains its selected ring and stream ID. Match transfer
pointers against that ring, encode doorbell/Set TR Dequeue stream ID, restart the
selected ring after cancellation, and update all streams for whole-endpoint
recovery. Keep all stream DMA through uncertain configure/stop; free after checked
drop/slot disable. No endpoint multiplexing or SuperSpeed UAS claim in this step.

## q199: SuperSpeed class/transport

Enable xHCI capability only with MaxPSA support. Before any class URB, explicitly
configure status/data endpoints for IDs 1..3. Send commands on stream zero; tag
selects status/data stream. Submit status and data before waiting for either;
validate Sense IU, handle early SCSI failure by checked data cancellation, and
retire/isolate both caller buffers on any error. Preserve finite deadline and
one active command. Wrap tags within configured IDs only after completion.

Initially reject SuperSpeed task recovery after transport failure. High-speed
abort drains one status FIFO; SuperSpeed has independent old/management queues
and needs a separate checked retirement procedure before tag reuse. Do not claim
q195/q196 recovery for the new profile. Verify native SS normal I/O and lifecycle;
full phase still requires stream recovery and remaining acceptance.


## q200: SuperSpeed recovery baseline and reset ownership

Run the existing native throttled-read recovery cell at SuperSpeed, retaining
its failure output. Inspect task abort, queued Sense and USB reset implementation.
A management response on a distinct stream does not consume old-stream status.
Do not infer queue emptiness from a timeout. Define a checked device-reset path
with drained URBs, reconstructed streams, capacity/identity checks and preserved
write uncertainty before enabling recovery. This diagnostic queue does not
complete the remaining recovery implementation.


## q201: atomic probe-result publication

Return probe geometry/cache policy through a candidate populated only on success.
Do not modify the class's published geometry, policy or sticky flush error during
probe. Initial attach publishes the complete result before disk_create. Exercise
failure at every probe command and success with a preexisting owner description;
verify failure leaves both output and published owner unchanged. This prepares
reset revalidation without enabling unsafe recovery or claiming media identity.


## q202: checked SuperSpeed reset recovery

Retain device/endpoints under class lifetime. On a new BIO after transport error,
make one checked stop/reset/reinit/reprobe attempt. Require unchanged fixed-LUN
inquiry identity, capacity and cache/write-protect policy; consume only reset
UNIT ATTENTION during recovery, not arbitrary media change. Never replay failed
BIO or clear sticky write uncertainty. Errors close admission and preserve owned
resources for teardown. Removable SCSI media require the pending disk-generation
path and stay closed; do not treat matching capacity as medium identity.


## q203: UAS filesystem persistence

Extend the disposable native cell with explicit UFS format confirmation, verified
UAS mount, file write, sync, unmount/remount and hash readback. Preserve boot image;
the UAS backing is intentionally reformatted. Require mount listing evidence
before file creation so failed mount cannot silently test the boot filesystem.
The strengthened cell uses `sync FILE`, whose current implementation invokes
fsync(fd), and checks a success-only shell marker. Do not claim crash recovery. Fix only observed integration issues within this bounded cell.


## q204: native pending-read disconnect

Throttle a cold read and require QEMU trace command admission without matching
SCSI completion, plus no returned guest prompt, before device_del. Bound QMP
removal wait to 45 seconds because QEMU synchronously cancels backend timers.
Require failed zero-byte read to terminate, matching disconnect, new disk binding,
persisted readback and checked halt. This is read lifetime, not uncertain write
or held-open descriptor replacement. Preserve every failure and trace snapshot.


## q205: retained-detach owner diagnosis

Repeat q204 with at most three temporary class detach snapshots of disk references,
open/closing/inflight/cache/buffer owners and revoked state. These are diagnostic
samples, not lock-coherent correctness proofs. Use them to select the responsible
owner and inspect its acquire/release contract. Remove diagnostics before final
builds; never bypass a retirement guard to force the native test green.


## q206: high-speed pending-read removal

Apply q204/q205's trace-proven pending-read disconnect cell to EHCI high speed.
Require zero-byte failed read, complete disconnect, fresh binding on reconnect,
persisted readback and checked four-CPU halt. This closes the other supported
speed's pending-read cell; replacement-media/held-descriptor and uncertain-write
acceptance remain independent.


q206 follow-up: initial EHCI cell exposed permanent quarantine in the no-device-
quiesce fallback. Require zero HCD owners and successful endpoint shutdown before
clearing that terminal quarantine; verify every refusal branch in actual-core
host fixture, and require native pending-read disconnect/reconnect to complete.


## q207: SCSI attention invalidates published media

Classify current Sense with the shared BOT/SCSI helper. For a published disk,
media absent/change, mode change and unowned reset/unknown attention revoke the
old medium. Only reset UNIT ATTENTION on TUR during this class's checked reset
probe is authorized for bounded retry. Initial unpublished probe remains separate.
Check media status under class serialization before every queued BIO so a BIO
admitted before revocation cannot issue another CDB afterwards. Reset reprobe
failure or identity mismatch must revoke published cache identity as well as close
transport. Preserve write uncertainty. Test sense classification and queued BIO
refusal in actual class source; replacement publication is still outstanding.


## q208: held open descriptor and physical replacement

Guest helper opens UAS read/write, primes a cached sector and successfully fsyncs,
then waits. Host unplugs the physical device and installs a new UAS/backing on the
same port while the fd remains open. Old fd read/write/fsync must all fail; close
then permits old object retirement and new disk publication. Read the new backing's
distinct pattern and verify it was not modified. Use a disposable injected helper,
not a production command. This tests physical replacement, not an in-place SCSI
removable-media exchange without USB disconnect.


## q209: retained-detach diagnostics

Track last reported class-detach error per USB device under topology ownership,
independently of DMA quarantine. Print the first error and changes in error;
do not print the same EBUSY on each periodic pass. Preserve every retry and stop
barrier. Extend the held-reference native test with a two-second hold after
old-fd rejection and require exactly one unchanged-error notification, followed
by successful close/replacement/readback/halt.


## q210: native write failure and sticky error

Use raw->blkdebug->file on only disposable UAS backing. Inject one write_aio EIO
at sector 16384 (8 MiB), leaving setup I/O untouched. A guest helper attempts a
4 KiB write there, requires error at write/fsync, reads a cold unchanged location,
then requires further write and fsync errors. Preserve host backing outside prior
intended setup write. This proves write-error propagation/latching, not a partial
DMA timeout, reset-after-write, or power-loss durability.


## q211: write timeout recovery

Extend write-only blkdebug cell with a seven-second delayed error, exceeding the
five-second transport deadline. Require pwrite ETIMEDOUT specifically, successful
cold read after transport recovery, retained write/fsync errors, and no replay of
the failed WRITE. Capture QEMU reset/ABORT traces. Do not count a quick backend
CHECK CONDITION as timeout or this noncommitting injection as a partial write.


## q212: serialized new-medium publication

Extract attach's disk publication into a helper taking a completed probe
candidate under the disk mutex. Require no existing disk. Build new disk geometry
from the candidate, register it, then install owner fields before releasing the
mutex; newly admitted BIOs cannot observe half-published state. Allocation, name
or registration failure must leave old owner fields/error state untouched and
release the unpublished disk. Clear write uncertainty only on successful new
identity publication. Initial attach uses this helper; the future media worker
must retire old references before calling it.


## q213: removable-media control ownership

Start a worker for removable SCSI LUNs before initial disk publication, with an
attach-ready handshake. Poll readiness under control and command serialization.
Only absent/media-change sense permits automatic identity replacement; other
attention remains revoked. Retire all old partition/disk references before
reprobe and new publication. Keep write uncertainty until publication succeeds.
Support initially absent media without publishing incomplete geometry. Stop and
join outside command/control mutexes before teardown; retain owner on timeout or
join failure. Host-check lifetime refusal and retirement ordering, then build.
Native in-place medium exchange and partition discovery remain acceptance work.


## q214: in-place removable SCSI exchange

Extend the native fixture with QMP eject/change-medium on the same SCSI device,
without USB device deletion or reattachment. Require fresh disk publication and
read a distinct replacement pattern from the same cached offset used before
ejection. Confirm initial and replacement backing bytes, unchanged boot source,
and repeat at high/super speed. Partition discovery and empty initial LUN are
separate remaining acceptance work.


## q215: initially empty removable LUN

Configure QEMU with a removable SCSI backend having no file at boot. Require
successful OS login and UAS binding without any UAS disk publication. Insert the
disposable medium using QMP change-medium without USB reconnect, then require
publication, write/sync/readback and persisted backing checks at HS and SS.


## q216: replacement partition discovery

Mark partition discovery pending on successful new-medium publication. The control
worker holds class lifetime/control ownership but releases the command mutex
before partition_reload, whose reads submit back to the class. Retry temporary
errors; consider no-table/unsupported scheme terminal without disabling whole-disk
I/O. Provide disk geometry ioctl for existing partition consumers. Verify unlocked
reload and pending retry semantics in the actual-source host fixture and build all
three supported x86 targets. Native partition-bearing media remain acceptance work.

q216 native follow-up: extend the exchange fixture with one MBR partition at LBA
2048, length 32768 sectors, on the inserted medium. Require new `sdX1` readback of
the distinct pattern, unchanged replacement bytes, and repeat HS/SS after build.


## q217: stopped removable transport

A transport-stopping failure on a removable LUN revokes its published identity and
schedules retirement. No reset or fresh media probe is allowed until old references
retire. Under control/command ownership, attempt checked transport stop, physical
USB reset and transport initialization once. Retain ownership on failed stop and
leave failed reset/init closed, without periodic reset storms. Probe and publish a
new disk generation only on success; old failed BIOs are never replayed and old
write uncertainty is retained until that new publication. Host-check ordering,
failed-step suppression, and live-disk rejection, then build supported targets.

q217 native follow-up: HS/SS removable LUN cold-read throttling, old generation
retirement, checked USB reset, fresh publication and explicitly issued new read.
Preserve the zero-transfer/time boundary and inspect reset/INQUIRY/READ records.


## q218: evidence audit and mounted identity

Map applicable ASYNC/SG/REC/FLUSH requirements to implementation and retained
actual-source/native evidence, distinguishing synchronous depth-one/bounce mode
from unimplemented multiplexed/direct-SG claims. Fill REC05/REC06 mounted-media
evidence: mount UFS, fsync and remount/read a file to prime cache; exchange medium
without USB reconnect while mount remains. Old file checksum must fail, and new
disk publication must wait for unmount. After unmount, require fresh new-pattern
raw readback and unchanged replacement backing. Preserve prior failures.


## q219: teardown design and shared BOT prerequisite

Document explicit `umount -f` restricted to irrevocably revoked disk-backed mounts,
with ordinary unmount unchanged, owner quiescence, external-reference refusal,
dirty-accounting disposition and filesystem-specific local teardown. Implement
that multi-layer path in subsequent bounded queues. First fix the independently
confirmed BOT partition_reload caller: acquire its required administrative disk
open outside the command mutex, reload, close on every result; retain pending work
on open/reload failure. Strengthen the existing BOT host fixture to enforce the
real open-count contract and verify no leak/reload after failed open. Build x86.


## q220: reversible writeback boundary for revoked media

Add a separate internal begin entry requiring a non-null disk whose retained
media chain is revoked (`disk_media_status` currently tests only revocation after
its null guard). Share pause/serialization/token ownership with ordinary unmount,
but skip synchronization only on the explicit revoked entry. Recheck eligibility
after joining the worker. Preserve dirty credits, error state and mount refs;
rollback restores admission. Ordinary begin must still propagate sync errors.
Test live/null refusal, no-sync dirty preservation, sibling/admission exclusion and
rollback through the existing actual writeback worker fixture. Commit/discard and
syscall plumbing remain later stages; this is not BUG-021 completion.


## q221: buffer discard preflight

Current buf_discard_media can discard one dirty buffer before discovering another
pinned buffer and returning EBUSY. Under its existing caller contract excluding
external disk/cache users, preflight all matching buffers for refs/busy/inflight
before the first eviction. Do not claim a stand-alone reservation against callers
that violate that contract. Test a pinned dirty buffer plus a separate releasable
dirty buffer: refusal must retain both and all dirty accounting, with no writes;
after release, discard succeeds and accounting returns. VM/mount transaction
preflight and commit remain separate implementation steps.


## q222: VM ownership preflight

Add a read-only revoked-mount preflight covering every associated shared object.
Require retained cache/writeback ownership only, one object reference, no mapping,
active operation, registry waiter, detach/resize/content owner or anonymous object.
Check live and orphan pages for busy/writeback/hold/pin/mapping ownership. Dirty
and failed writeback state may be eligible for later explicit discard but is never
changed by this check. Reject live/null disk. Caller must separately close mount
admission and retain exclusion through commit. Exercise real file-cache object
mapping and pin lifetimes, dirty retention and failure nonmutation.


## q223: VM discard commit

Under caller-owned closed mount admission and paused workers, recheck all target
VM objects under registry lock before mutating any. Remove only eligible revoked
mount objects; clear dirty ownership through the existing credit/index helpers,
retire registry/cache counts, and destroy detached pages/files outside registry
lock. Report discarded dirty bytes for explicit-loss diagnostics. Refusal must
leave every object untouched. Test busy second-object nonmutation and successful
live/orphan dirty credit + slab/frame/file release using actual VM code. No public
force-unmount until namespace/filesystem preparation can satisfy the caller contract.


## q224: UFS revoked-media preparation

Add an optional filesystem callback for explicit revoked-media teardown. It must
be non-mutating, perform no backend I/O, and refuse unsupported or still-owned
filesystem state before any cache discard. UFS requires an initialized revoked
disk-backed mount and refuses a retained snapshot device. Ordinary prepare_unmount
continues its existing clean-superblock write and errors. Existing local unmount
finalizer is shared after the eventual cross-layer commit. Check eligibility,
snapshot refusal and unchanged state with actual source; build all three targets.
Public invocation remains deferred until mount admission and ownership are ready.


## q225: retained VM file ownership

VM objects can retain the same read/write file description, including a former
mmap user's description. Sole object ownership is not proof that its file has no
external owner. Require each retained description's refs to match its read/write
slots, with mount/inode identity matching the object. Conservatively refuse
cross-object or cross-mount descriptions whose exclusive ownership is unproven.
Expose deduplicated path-reference counts for mount or selected inode to the future
closed-admission transaction. Never count two path refs for two slots holding one
file description. All checks are non-mutating; external FD refusal precedes any
dirty/error discard. Callers retain closed admission and must respect VM registry
lock ordering; the counting helper is not a reservation. Test alias, distinct
reader/writer, external file owner, wrong identity and refusal nonmutation.


## q226: inode ownership preflight under closed admission

Add an internal revoked-mount inode check requiring the reserved DYING attachment
and the VM ownership helper. Validate VM even for an empty inode cache. For each
matching inode, take a temporary pin under the cache lock, inspect proven VM path
owners outside that lock, then compare total refs under the cache lock against
cache/root/namespace/VM plus the temporary pin. Refuse unexplained references and
overflow. Drop the temporary pin without reclaiming or clearing any inode; leave
dirty/error state untouched on success and refusal. The caller prevents new mount
users and retains exclusion through final commit. Focused actual-inode tests cover
external/cwd-equivalent refs, namespace/root/VM refs, wrong admission, helper errors,
lock separation and cleanup. Public unmount/dirty inode disposal remain subsequent.


## q227: no-I/O filesystem and inode commit

Add an optional no-fail filesystem revoked commit callback paired with preflight.
After closed admission and every cross-layer preflight, UFS closes journal views
and makes its local mount unwritable before VM file-close/inode release can enter
zero-link reclaim. No disk metadata or clean flag is written. Ordinary teardown
keeps existing behavior. Add inode dirty-flag disposal for the same irrevocably
revoked DYING attachment, returning a diagnostic count and preserving error history
and all other flags/owners until final destruction. Invalid commit preconditions
are invariant failures, not partial-success error returns. Verify local mutation
scope and count with focused actual-source tests; full public integration follows.


## q228: namespace and public integration

Define shared MNT_FORCE and parse umount [-f] [--] directory. Preserve the existing
root-only syscall gate and reject unknown flags. Keep ordinary context unmount as
a wrapper with flags=0. The explicit revoked path requires paired filesystem
callbacks, non-null revoked disk and all lifecycle/VM providers; reject root,
bind/private/child attachments. Pause reads/writeback without syncing, reserve
DYING under namespace transaction, purge namecache, and count known VM mount paths
before exact namespace refs and inode/UFS preflight. Roll back to LIVE plus worker
policy on every precommit refusal. Once preflight passes, commit UFS/no-I/O inode
and VM disposal, log loss counts, finalize and detach using existing lifetime
order. No ordinary sync-error suppression. Existing disk retirement owns physical
buffer discard after all old attachment refs drop. Build three targets and extend
mounted-exchange QEMU acceptance to assert ordinary failure, then explicit force,
old attachment removal, replacement publication/readback. Record any native failure
with evidence and resume conditions; do not claim full p029 on partial acceptance.


q228 integration review also requires global VM eviction/reclaim and unscoped
sync scans to skip objects whose mount is DYING. Otherwise a new reclamation owner
can appear after mount/inode reference preflight. Targeted unmount-owned sync remains
available for ordinary teardown. Verify reserved-mount clean reclaim/eviction refusal
in the actual VM fixture, then rebuild and repeat the public native path.


## q229: synchronous USB wait scheduling

q228 High-Speed trace completed data-out in QEMU while CPU0 remained in the
pre-deadline drv_usb_urb_wait spin. EHCI completion publication depends on the
retirement worker. The current pending loop offers no scheduling opportunity until
its deadline expires. Yield while pending, preserving terminal-status mapping,
checked cancel and HCD ownership semantics. Verify completion requiring scheduling
and timeout progression with a focused actual-source fixture. Repeat High-Speed
mounted-medium acceptance; a passing attempt alone does not prove every cause of
BUG-022, so preserve failed evidence and report the exact coverage.


## q230: retained dirty native acceptance

Add a guest helper using existing writeback control/report UAPI. Enable delayed
writeback for the preallocated UFS payload, dirty an existing page without fsync,
and require positive device dirty accounting before signaling the host to eject.
Keep an FD open across exchange; old read/fsync must fail and force must refuse
EBUSY. Close the FD, require dirty state still retained, then let the shell force
unmount. Require positive VM dirty-byte disposal diagnostic, replacement publication
and unchanged replacement data. Do not label a clean timing outcome dirty coverage.
Run SS then HS when the first path is established. No production knobs or artificial
writeback age changes solely to force a passing test.
