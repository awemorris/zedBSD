<!-- awesome-plan project=zedbsd record=ws025-p009 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws025/phase009/phase.md`

親: [ws025](https://github.com/awemorris/zedBSD/issues/26)

# ws025-p009: USB 64 KiB reservation

日付: 2026-09-07

Phase ID: `ws025-p009`

Status: completed; Queue q096; results.md に受け入れ証拠を記録

Parent: [WS025](https://github.com/awemorris/zedBSD/issues/26)

依存: ws025-p001。高位 RAM との統合は p005 の結果を利用

## 目的と境界

core staging と HCD DMA/request を再利用し、normal storage の要求上限を 64 KiB にする。

## 変更対象

- `include/drivers/usb.h`
- `src/drivers/usb.c`
- `src/drivers/usb-storage.c`
- `src/drivers/pci-xhci.c`
- `src/drivers/dma.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. 現行 USB core と各 HCD の reservation owner を確認し、normal bulk 2 本分の core/HCD 両方を attach 時に確保する。非対応 HCD は 8 KiB と報告する。
2. swap/reclaim-safe 8 KiB reserve を別に維持し、normal 負荷の枯渇で奪わない。attach 途中失敗を全段階で巻き戻す。
3. URB/request/DMA の貸出し世代を持ち、timeout/取消し未確定時は保持する。隔離中に同じ backing を再利用せず、代替の無制限確保を禁止する。
4. storage の max transfer、BOT data length、HCD/TRB boundary と実効上限を一致させる。BOT command は直列。
5. p005 後に DMA32/高位 normal memory と統合し、WLAN/HID 並行時の IRQ/latency/timeout を測る。

[共通 I/O 契約](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/io-design.md) と [memory 設計](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/memory-design.md) を維持する。

## 受け入れ

- IO11–IO12、ASYNC04–ASYNC06、MEM12–MEM16。warm steady I/O で normal request/DMA の動的確保を除く。
- 64 KiB 対応 HCD の data command 一回、8 KiB fallback、複数 storage、reserve 枯渇、cancel/recovery が通る。
- 該当 ID の詳細は [受け入れ行列](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

retirement を証明できない資源は retained と報告する。IMOD を同時変更して原因を混ぜない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。
