<!-- awesome-plan project=zedbsd record=ws315-p005 -->
# ws315-p005: 規約の全文の見直し

Status: cleared（2026-10-30 05:53 P3 q1694-i01: conformance-reviewer の見直しを P3 が確かめた。host 試験 PASS、amd64・pcat（driver y）の build warning 0）
Parent: [WS315](../ws.md)
Queue: q1694 / q1694-i01（P3）
依存: p002〜p004

## 対象

WS315 が足した C: `include/drivers/usb/usb-rtl815x.h`、`src/drivers/usb/usb-rtl815x-frame.c`、`src/drivers/usb/usb-rtl815x.c`、`src/kern/platform/pcat.c` の登録の行、`plan/ws315/tests/rtl815x-frame-test.c`。
`plan/coding-style.md` の全文（§14 の checklist）と guardrail の出典・開発の管理情報の規則に照らし、書き方だけを直す（動作は変えない）。

## 直した事（conformance-reviewer、P3 が diff を読んで確かめた）

- 閉じ括弧の後の空行（usb-rtl815x.c 23、試験 4）、段落の目的の comment（usb-rtl815x.c 35、試験 6、frame.c 1）。
- 条件の中の関数の呼び出しを変数に（`drv_usb_device_speed` 2 か所、`rtl_rx_head_done_locked` は同じ評価の順のまま）、引数の中の入れ子の呼び出し（`sched_ticks`・`drv_usb_rtl815x_chip_name`）を変数に。
- 条件演算子を if に（link の log の duplex の文字列は同じ、`interrupt_length`、試験の入れ子の三項は static const の配列）。
- 未使用の引数の `(void)x;` を `UNUSED_PARAMETER()` に（usb-bt.c と同じ file の中の `#ifndef` の定義）。
- `pcat.c`: 登録の呼び出しを変数に受けてから判定（log の文字列は同じ）。
- P3: RTL8157 の新しい register・bit の名前を独自の名前に直し、PHY の表の comment を自分の言葉に書き直した（[p004](../phase004/phase.md) の「出典の確かめ」）。define の列を揃えた。
- 変えていない: register の access の順と値、log の文字列、出典の comment、PHY の表の値と順（diff で 99 行一致）。

## 確かめ

- host: `OUT=build/ws315-p005-frame-3 sh plan/ws315/tests/run-rtl815x-frame-test.sh` → `rtl815x-frame: PASS` ×2（-O2、ASan・UBSan）。
- build（vmunix、warning 0）: `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/ws315-p005-amd64 build/ws315-p005-amd64/vmunix`（amd64 vmunix check PASS）、
  `make ZEDBSD_CONFIG=config/ci/config-pcat.mk CONFIG_DRIVER_USB_RTL815X=y BUILD=build/ws315-p005-pcat build/ws315-p005-pcat/vmunix`（rc 0）。`git diff --check` OK。
- 未実施: clang-format（環境に無い）。5330 の実機の回帰（p003 の残りと合わせて、5330 の SSH の判断の後）。
- 見直さずに残した物: 同じ comment の下に続く `error = rtl_write(...); if (error != 0) return error;` の組（一つの操作として読める）、pcat.c の他の WS の登録の `if (call() != 0)`（範囲の外）。

## 結果

- 規約の見直しを終えた。WS の受け入れの残りは p003 の image の試験（camcap・送受信の同時）と、p004・p005 を含む tree の 5330 での RTL8156B の回帰。
