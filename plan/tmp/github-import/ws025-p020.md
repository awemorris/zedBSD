<!-- awesome-plan project=zedbsd record=ws025-p020 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws025/phase020/phase.md`

親: [ws025](https://github.com/awemorris/zedBSD/issues/26)

# ws025-p020: bounded 先読み

日付: 2026-09-07

Phase ID: `ws025-p020`

Status: completed (q110); bounded automatic readahead, cache-read ownership correction, focused/native/build acceptance complete; see results.md.

Parent: [WS025](https://github.com/awemorris/zedBSD/issues/26)

依存: ws025-p016、ws025-p019

## 目的と境界

需要 read を優先しながら順次アクセスの次の範囲を先読みする。

## 変更対象

- `src/kern/file.c`
- `src/kern/vm-object.c`
- `src/kern/disk.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. open file description ごとの next offset/window を導入する。需要 read の run 合成と要求外の先読みを別 counter にする。
2. window は 64 KiB から最大 128 KiB を候補とし、各 BIO は 64 KiB 以下。EOF、queue slot、global/device budget で制限する。
3. seek/random/pressure で縮小し、需要 I/O を優先する。高位 cache page に populate できることを確認する。
4. content generation が変わった completion は破棄する。先読みの失敗を、既に成功した需要 read のエラーにしない。

[共通 I/O 契約](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/io-design.md) と [memory 設計](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/memory-design.md) を維持する。

## 受け入れ

- READ01–READ04、CACHE03–CACHE09、ASYNC01–ASYNC08。順次 read で useful hit が増え、random workload の不要 bytes と p95 を記録する。
- EOF、truncate、copy-up、並行 write、seek、error、queue/メモリ不足で demand read が前進する。
- 該当 ID の詳細は [受け入れ行列](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

先読みが需要 I/O を遅らせる場合は budget/window を縮める。無制限 window や同期の要求外 read に戻さない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。

## q110 selected implementation plan

p016 cache budgets/reclaim, p019 owned asynchronous BIO and p018 coherent file
I/O/lifecycle are complete. Inspection confirms `vm_object_read_coherent` already
coalesces demand fills and owns an active object/cache admission. It can wait for
an object transition, so calling it synchronously for bytes beyond a read would
violate this phase. The asynchronous BIO provider owns device payloads but does
not know filesystem extent/hole mapping; a filesystem prefetch worker must stay
above it rather than block its own lower transport worker.

1. Add per-open-description sequential state with explicitly sampled content
   identity/generation and completed offset. Update it at outer read completion,
   after dropping ordinary file/content leases. A seek/random transition resets
   the stream; separate descriptions remain independent. Start at a bounded
   64 KiB window and permit 128 KiB only with demonstrated sequential usefulness.
2. Use bounded asynchronous filesystem work, independently owned from BIO slots.
   A job retains its read owner, final content inode and physical/cache lifecycle
   admissions. Queue/resource refusal skips optional work; it cannot change an
   already successful demand result. At most one bounded fill is in flight per
   physical worker; never create unbounded threads or allocate per-page work.
   Reserve/charge actual scratch/control storage through shared cache accounting.
3. Refactor the existing VM population boundary to support optional private fills
   and conditional publication. Recheck current content identity, EOF, generation
   and existing page state before adopting completed bytes. Do not overwrite a
   dirty/newer page or publish a partially initialized page. A stale/error result
   frees its private ownership. Do not retain an inode/content lease while waiting
   for optional queue admission. Lower I/O continues through explicit context and
   established backend run mapping, with each request bounded to 64 KiB.
4. Demand activity wins admission over queued prefetch; pressure/random activity
   shrinks or cancels the optional window. A running device transfer is not falsely
   called cancellable. Bound any unavoidable non-preemptible interference and
   measure it. Close/seek/truncate/copy-up/media change invalidate queued work and
   retain necessary owners until actual completion. Integrate unmount/shutdown
   ownership so optional work cannot survive a retired mount/device unnoticed.
5. Add separate requested/completed/useful/discarded/error bytes and queue-pressure
   observations; demand run counters remain distinct. Test READ01–READ04 and the
   referenced CACHE/ASYNC rows, with deterministic delayed completion and memory,
   queue, stale-generation and backend failures. Record sequential usefulness and
   random unnecessary bytes/p95, then supported builds and native storage/cache
   regressions. Keep default behavior coherent when prefetch resources are absent.

Implement state/accounting and ownership first, then conditional VM publication,
then scheduling and measured tuning. Revisit local implementation choices against
actual VM/lock contracts while retaining these boundaries. No new human decision
or physical access is required; the existing autonomous WS025 authorization owns
this finite queue. Review evidence every 90 active minutes without inventing a
phase completion merely because a checkpoint timebox expires.

### Scheduler ownership refinement after stream/VM primitives

Do not retain and later dereference a raw originating `struct file *` from a
worker after close. Prefer the already independent VM cache read handle/object
owned by the private-fill token. A queued job may use the originating file only
as an identity under the queue guard, with close/seek/reset hooks invalidating
that identity before pool reuse; it must not read freed per-description state.
File descriptor teardown is in `src/kern/filedesc.c`. Distinguish descriptor close
from the routine temporary `file_close` at every syscall end, otherwise every
newly queued read would cancel itself. Final file retirement must also invalidate
any origin identity used by kernel-only descriptions.

A bounded worker must explicitly own prepared frames and cache admissions while
queued/running. Queue refusal aborts them before returning from the optional
completion hook. Cancellation of accepted device I/O only suppresses adoption;
resources stay owned until real completion. Define the terminal adoption claim
under the queue guard, then release that guard before VM publication/free, which
may retire physical storage or file references. Mount/shutdown drain must join
this completion phase as well as the device read. Demand priority cannot be
implemented by holding the registry guard across backend or VM cleanup.

State primitives alone do not satisfy READ01–READ04. Wire observations, invalidation,
useful-byte feedback and counters together with scheduler ownership, then measure
sequential and random workloads before marking this phase complete.

### Asynchronous service contract

`readahead-worker.c` now owns four reusable physical workers, each with two total
preparing/queued/running/cleanup slots and one 64 KiB scratch payload. HAL scratch
plus a control-page allowance is charged by its actual size as optional shared
worker memory. Preparation publishes a slot before allocating VM frames, so a
close or teardown can cancel/join preparation as well as device I/O. Every job
owns the independent VM fill and a physical cache-domain token; the origin file
is compared only as an identity after admission. Submission try-locks control
and the origin file, checks the predictor generation, and registers the slot
before dropping the file lock. The upcoming reset/close hooks must reset and
cancel under that same file lock; callers submit only after dropping file/content
leases. This closes reset-before-admission without retaining a user description.

Demand admission currently gates new speculative reads globally, conservatively
across physical devices. Already running transfers are not preempted; canceled
queued fills bypass this gate to release ownership promptly. Workers independently
execute backend reads through `FILE_IO_VM_OBJECT` and conditionally publish private
fills. The terminal adoption claim clears the origin under the queue guard; a
later seek cannot undo an already claimed adoption, but VM generation validation
still protects file content and a mount boundary joins its completion/cleanup.

A caller-owned boundary token closes one mount or all admission, marks matching
jobs canceled and joins every state through final cache-token release. Interrupted
begin removes its admission gate and leaves accepted cancellation safe to finish.
Nested boundaries compose. Mount identity must remain retained by the caller until
boundary finish. Idle trimming releases scratch under control serialization; a
HAL release failure retains a charged reusable owner. Kernel worker records are
permanent and bounded; no fresh thread is allocated for a reused record.

The APIs and mount/shutdown boundaries are implemented. Ordinary file operations
and descriptor invalidation are not connected yet. Before automatic admission, wire demand balance including
all begin-error exits, capture initial offset/EOF under coherent read ownership,
update predictor/cancel under the description lock, distinguish descriptor close
from temporary references, and use boundaries before existing VM unmount/drain
checks. Add useful-page consumption feedback and public counters, then measure
READ workloads; do not infer end-to-end acceptance from service fixtures alone.


Lifecycle integration checkpoint: `mount_io_quiesce/finish` now scopes the
independent read boundary around public/private unmount's existing writeback/VM
checks. Shutdown retains a static global read boundary after commit and restores
it on every retryable failure. Host provider-ordering checks, actual worker plus
shutdown delayed-I/O checks, and the QEMU USB/UFS writeback regression pass; see
results. Capture/observation and FD-close hooks remain the next implementation.


Automatic-hook checkpoint: file transactions capture initial offset, stream
witness and leased EOF; release demand/file/content ownership before optional
observation/submission; skip contended state with try-locks. Explicit FD teardown
and seek invalidate; temporary reference drops preserve state; final reference
retirement resets under exclusive ownership. Async worker admission excludes
idle/bootstrap callers after the QEMU boot deadlock described in results, and
backing-owner I/O does not create an independent stream inside its outer request.
The corrected automatic hooks pass the USB/UFS native regression and file/VM/FD
host tests. Next required work is useful-page feedback/public counters and actual
READ workload measurements, including physical-domain demand interference.


Usefulness checkpoint: actual coherent VM reads now report disjoint forward
speculative consumption through the outer file transaction, driving real window
growth from 64 to 128 KiB after 64 KiB of confirmed use. Attribution stops on dirty,
content/size changes, mappings and retirement. Fixed per-page frontier accounting
is exact for forward reads and conservative for backward gap revisits; public
counters and random acceptance reports must identify useful as a lower bound and
retired uncredited bytes as an unused upper bound. Focused actual VM/file tests
cover growth, repeat/partial reads and truncate. Public counters and measured
READ workload acceptance remain next, not yet achieved.


Cold-read review: public counters and actual sysctl ABI tests pass. Minimum
non-EOF refill is now 32 KiB after page-sized replenishment caused excess work.
The corrected native fixture uses offline, distinct data extents and requires
actual driver-read bytes; timing retains TSC cycles plus calibrated estimates
because the ordinary clock resolves 10 ms. Cold sequential reads reduce driver
calls but still show an excessive demand tail. Before declaring READ acceptance,
review generic inode serialization around speculative backend reads, independent
backend cursor ownership, and shared content read leases as one design. Retain
all mutation/EOF/claim protections; do not falsely assert INODE_IO_OWNED. Repeat
cold latency and remaining RAM/device/acceptance gates after fixing the contention.

Add a deterministic regression before changing read ownership: pause a speculative
backend transfer for future pages, then read an already resident earlier page
through an independent ordinary description. The cached demand should complete
without waiting for the future-page device read. Keep a simultaneous content
mutation/truncate excluded or generation-safe, and retain backend cursor lifetime
protection. The existing generic inode lock is the suspected blocking boundary;
confirm with this regression rather than inferring all delay from a benchmark.


Cached-demand correction is implemented: existing-object operation pins plus
shared visible/final content leases span the outer transaction, while actual
backend misses retain serialized inode I/O. The deterministic paused-backend
regression now passes, including mutation/truncate exclusion and pin release.
Cold USB tail improves but sequential baseline/device/RAM acceptance remains open;
see results for measured values and limitations.
