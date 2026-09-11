# ws025-p014: flush 世代と永続化証明

日付: 2026-09-07

Phase ID: `ws025-p014`

Status: completed; Queue q099; see [results](results.md)

Parent: [WS025](../ws.md)

依存: ws025-p001

## 目的と境界

同じ永続化境界への重複 flush だけを省き、上位 dirty と leaf write の違いを表す。

## 変更対象

- `src/kern/disk.c`
- `src/drivers/loop.c`
- `src/kern/overlayfs.c`
- `src/drivers/fs/ufs1/ufs1-vfs.c`
- `src/drivers/fs/fat.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. leaf disk に accepted/completed/stable frontier と generation を定義する。最初は counter と全 flush 発行の adapter を入れる。
2. 完了 frontier は前の write に穴がある間は進めない。flush は対象 write の完了を待ち、成功した範囲だけ stable とする。
3. mount/FS/overlay は BIO 前の論理 dirty 世代を持つ。同じ範囲への flush は合流し、後続 write/別 journal commit は合流させない。
4. partial/failed/unknown write、flush failure、reset/media change で証明を無効化する。device の flush/FUA capability を実効 policy に含める。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- FLUSH01–FLUSH06。no-write repeat の省略、同時 fsync、新規 write、error、overlay の異なる commit 境界を検証する。
- 成功 fsync の保証と errno が変わらない。SYNCHRONIZE CACHE 最大一回を無条件の oracle にしない。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

下位 flush 成功だけで上位 readonly を解除しない。不明な device persistence は成功保証に含めない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。

## Pre-queue inspection after q098

The production BIO completion currently publishes COMPLETED and wakes a waiting
caller before it finishes leaf inflight/reference accounting and reads `b_done`.
A stack-owned synchronous BIO can therefore be reused by its caller before the
completion routine finishes accessing it. P014's completion-frontier bookkeeping
must finish before terminal publication; include an explicit lifetime regression
in the selected Queue. This is a source finding, not a claimed runtime reproduction.

An embedded outstanding-write list can track accepted order without retaining
completed BIO storage or allocating completion records. Remove a BIO before
terminal publication; the first outstanding sequence bounds the completed
frontier, so out-of-order completion cannot skip a hole. Flush proofs must carry
a captured target and invalidation epoch, and must not be inferred from upper
logical dirtiness. Exact locking, callback/waiter ownership and upper-owner dirty
hooks remain to be finalized before implementation authorization via Queue.

## q099 selected design

- Embed ordered outstanding-write links in BIOs; remove each link before publishing
  completion so no completed caller storage remains referenced. Completed frontier
  stops immediately before the earliest pending write, or reaches accepted when empty.
- Capture a target at flush entry. Serialize physical flush ownership and wait only
  for the captured target's completion frontier. Stable advances only to that target
  and only when the invalidation epoch still matches. New writes are not retroactively
  included. Sequence/epoch exhaustion disables reuse rather than aliasing old proof.
- Require an explicit physical persistence capability for elision. Unknown and
  stacked disks always forward flush (loop therefore always calls backing fsync).
  USB policy and recovery hooks determine eligibility; unsupported-driver contracts
  retain existing behavior instead of optimistic no-op success.
- Keep upper logical generations distinct and conservative. Existing FS/FAT/overlay
  drain always executes; a successful mount sync records only its starting target.
  Leaf proof cannot suppress upper dirty work or clear a sticky filesystem error.
- Include completion ownership in the change: all accounting and callback capture
  precede terminal publication. A callback-owned BIO cannot also have an independent
  synchronous owner freeing the same storage; preserve a clear ownership contract.

The upper hooks and driver eligibility are integration work within this Queue,
not a reason to mark a frontier-only stub complete. P017 will extend the logical
owner state with deferred dirty data and sticky error/drain policy.
