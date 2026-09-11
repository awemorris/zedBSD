# ws025-p025: storage sense と媒体世代

日付: 2026-09-07

Phase ID: `ws025-p025`

Status: completed in q121; see [acceptance evidence](results.md).

Parent: [WS025](../ws.md)

依存: ws025-p001、ws025-p014

## 目的と境界

同一媒体で回復可能な失敗を有限に復旧し、実媒体変更は旧 cache/mount から切り離す。

## 変更対象

- `src/drivers/usb-storage.c`
- `src/kern/disk.c`
- `src/kern/buf.c`
- `src/drivers/loop.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. ONLINE/RECONFIGURE/REVALIDATE/ABSENT/FAILED と last sense、reset owner、media generation を明示する。一般の永続 error bit clear を廃止する。
2. 自前 reset に対応する 06/29/00 は一回 retry、06/28/00 は revalidate、2A は ASCQ ごとに mode/capacity を分類する。未分類は無条件 retry にしない。
3. 保留 BIO と control worker を分け、worker が自分の request の drain を待たない構造にする。上位へ最終 error を返す前に同一媒体の有限 recovery を試みる。
4. 旧世代の submission/cache hit/partition/claim/mount admission を閉じ、idle の場合だけ新 physical disk object/generation を登録する。mounted root を別媒体へ再接続しない。
5. reset 後 06/28 の device quirk は実機再現/firmware/媒体構造の根拠がある場合に限る。INQUIRY/capacity 一致だけを根拠にしない。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- REC01–REC06、FLUSH04–FLUSH06、ASYNC04–ASYNC08。mode change、capacity change、absent、unknown UA、self-reset、保留 BIO と drain を検証する。
- 同一媒体での回復成功と最終 FS error state が一致し、媒体交換で古い cache の内容を返さない。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

driver 復旧だけで mount readonly を解除しない。実媒体交換後の旧 mount 継続は実装しない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。

## p018 native shutdown lifecycle observation

`../temp/p018-shutdown-native/guest.log` records successful storage drain and real
halt, but USB class shutdown reports error 17 for both mounted storage devices
and retains the host controller. The storage contents were independently verified
after stopping QEMU. Review the existing mounted-device lifecycle and controller
quiesce semantics during p025/p026: do not interpret a retained HCD as confirmed
hardware shutdown, and do not discard mounted/cache ownership merely to silence
the message. This is a recorded finding, not additional executed p025 scope.
