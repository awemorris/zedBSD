<!-- awesome-plan project=zedbsd record=ws025-p005 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws025/phase005/phase.md`

親: [ws025](https://github.com/awemorris/zedBSD/issues/26)

# ws025-p005: 高位 RAM の通常公開と受け入れ

日付: 2026-09-07

Phase ID: `ws025-p005`

Status: completed; Queue q092; automatic evidence and user-accepted physical gate in results.md

Parent: [WS025](https://github.com/awemorris/zedBSD/issues/26)

依存: ws025-p004

## 目的と境界

通常 image の恒久 1 GiB 制限を撤去し、全 usable RAM の利用を証明する。

## 変更対象

- `src/hal/amd64/cmain.c`
- `src/hal/amd64/page.c`
- `src/hal/amd64/bsp-pcat/boot.c`
- `plan/ws003/tests/uefi-high-memory-usb-boot.sh`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. p003/p004 の staged publication を通常有効にし、初期 map サイズと allocator 利用上限を切り離す。test-only cap は通常 config に残さない。
2. v6 boot reservations の解放時点を確認し、不要な loader/BootServices 領域を返す。AP/ACPI/runtime 使用中の領域は返さない。
3. bounded probe で 1 GiB 超・4 GiB 超の PA を明示選択し、kernel/user map、内容検証、fork/COW、再利用を試験する。
4. SeaBIOS/OVMF の RAM 行列、SMP、USB-root、DMA32、高位 ACPI/FB を実行する。reported と managed の差は各 reservation で説明する。
5. 実機で同じ artifact を確認する。automatic と physical の状態を別に残し、単なる login 到達を全 RAM 利用の証明にしない。

[共通 I/O 契約](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/io-design.md) と [memory 設計](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/memory-design.md) を維持する。

## 受け入れ

- MEM01–MEM16 と acceptance.md の boot 行列。新 BIOS/UEFI image が 1 GiB/4 GiB で切り捨てない。
- 少なくとも両 firmware の 8/16 GiB セルで PA >4 GiB の実利用・USB I/O・4 CPU を証拠化する。
- 該当 ID の詳細は [受け入れ行列](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

実機未確認は physical pending として残す。過去の 1 GiB 制限を黙って戻して全面完了にしない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。
