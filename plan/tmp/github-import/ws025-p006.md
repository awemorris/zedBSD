<!-- awesome-plan project=zedbsd record=ws025-p006 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws025/phase006/phase.md`

親: [ws025](https://github.com/awemorris/zedBSD/issues/26)

# ws025-p006: I/O pool と exec chunk

日付: 2026-09-07

Phase ID: `ws025-p006`

Status: completed; Queue q093; results.md に受け入れ証拠を記録

Parent: [WS025](https://github.com/awemorris/zedBSD/issues/26)

依存: ws025-p001

## 目的と境界

syscall/exec の毎回の大きな物理連続確保と PAGE_SIZE 分割を減らす。

## 変更対象

- `src/kern/syscall.c`
- `src/kern/elf.c`
- `src/kern/entry.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. 共有大小 pool を起動時構築する。64 KiB の希望本数、RAM/64・4 MiB 上限、4 KiB reserve、header/page rounding を io-design.md に従って実装する。
2. syscall は既存 file/content 契約内で try-borrow し、大小の枯渇時のみ既存 stack fallback を使う。pool 待機・per-call fallback allocation は追加しない。
3. exec の copy_segment_snapshot を最大 64 KiB へ変え、content lease、部分 segment、BSS と失敗 cleanup を維持する。
4. 全 exit、signal、EFAULT、EOF、short result、vector/PIPE_BUF で返却を検証する。vmap 前は pool 構築時の物理連続確保を明示する。

[共通 I/O 契約](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/io-design.md) と [memory 設計](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/memory-design.md) を維持する。

## 受け入れ

- IO01–IO06。warm pool で 64 KiB scalar/単一 iovec は file transfer 一回、scratch 用の新規物理確保ゼロ。
- q087 の六 entry path と exec fixture、並行枯渇・低 RAM が通る。通常負荷 fallback 率は調整目標として測る。
- 該当 ID の詳細は [受け入れ行列](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

枯渇率が高い場合は workload/実 bytes を記録して予算内で調整する。無制限増設や lease 内待機に逃げない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。
