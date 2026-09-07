# ws025-p022: exec の cache snapshot 共有

日付: 2026-09-07

Phase ID: `ws025-p022`

Status: completed in q118; see [results](results.md) and [snapshot design](snapshot-design.md).

Parent: [WS025](../ws.md)

依存: ws025-p015、ws025-p016

## 目的と境界

実行ファイル内容の安定性を保ちながら、page cache を exec に利用する。

## 変更対象

- `src/kern/elf.c`
- `src/kern/vm-object.c`
- `src/kern/vmspace.c`
- `src/kern/file.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. p006 の pool copy を基準にし、ELF header/PT_LOAD/interpreter の内容 lease と page 世代を一つの snapshot として扱う。
2. 初期の直接共有対象は実行中に内容が変わらない inode/backing に限定する。read-only mount の可変化も pin の lifetime で制御する。通常の writable file は既存 copy snapshot を維持する。
3. text は読み取り専用、data は private/COW とし、非整列先頭/末尾、BSS、異なる segment 権限で必要な private page を作る。
4. exec pin、cache reference、process exit/fork の寿命と圧力時の回収を結び付ける。共有対象の内容更新と writable mapping を admission で制御する。
5. 変更可能 file の snapshot 共有へ拡張する場合は内容世代別 object と writer COW/reclaim の契約を別の有効化項目にし、MAP_PRIVATE だけで保証したことにしない。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- EXEC01–EXEC05、CACHE01–CACHE09。immutable/readonly の二回目 exec は対象 data の device read を抑え、text page を実際に共有する。
- writable file の既存 snapshot、interpreter、BSS/partial page、fork/COW、同時更新、exec 失敗 cleanup とメモリ圧力を確認する。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

通常の writable file に一律 ETXTBSY を追加して旧挙動を変えない。共有対象と copy fallback の実効結果を報告する。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。
