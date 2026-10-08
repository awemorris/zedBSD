<!-- awesome-plan project=zedbsd record=ws050p001 -->

# ws050-p001: 調査と設計（UCSI）

Phase ID: `ws050-p001`
Parent: [WS050](../ws.md)
Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc §2））（旧: in-progress → Q1 の判定待ち（2026-10-08 q902 P1 の照合: §10 の判断は 2026-10-04 ユーザーが決定（WS051 の ws.md の記録）、p002〜p005 がこの設計で実装済み）（旧: in-progress（2026-10-04。design.md の第 2 版にレビューを反映した。§10 の人間の判断待ち）））
Phase disposition: normal
Queue: q679 / q679-i01（P1）

## 経過（2026-10-04、P1 generation15）

- 5330 の `ssdt8.dat`（`UsbCTabl`）と DSDT を `iasl -d` で読み、UCSI の device `\_SB.UBTC`（USBC000/PNP0CA0）の `_STA`・`_CRS`（GNVS の `UBCB`）、
  mailbox の region（0x38 byte、UCSI 1.x の配置）、`_DSM`（UUID `6f8398c2-…`、1 = write、2 = read）、EC の `_Q79` → `Notify (UBTC, 0x80)`、
  connector `CR01`〜`CR0A` を確かめ、[design.md](../design.md) を書いた（構成、初期化、誤りと競合、試験、他の WS との境界、Phase の案、判断の点）。
- design-reviewer（読みのみ）を起動した。結果は generation15 の終わりまでに届かなかった（届けば Q1 か generation16 が design.md に反映する）。
- 発見: Linux の i915 は DP-alt を UCSI を使わず TCSS の register で扱う → WS051 の画面の出力は WS050 に必須の依存を持たない見込み（design §8、
  [ws051-p001](../../ws051/phase001/phase.md)）。

## 再開点

- design-reviewer の指摘を反映し、§10 の判断（公開の形 `/dev/typec`、role の切り替えを範囲外、UCSI 1.x だけ）を Q1 経由でユーザーに確かめる。
- design.md の §2 の bit の配置は UCSI の仕様書で確かめてから p002 の実装に入る（Linux の driver（GPL）は写さない）。

## レビューの反映（2026-10-04、P1 generation16）

- generation15 が起動した design-reviewer の結果を Q1 が受領し、[design-review-2026-10-04.md](../design-review-2026-10-04.md) に複写した。
- [design.md](../design.md) を第 2 版に書き直した（§11 に A1〜E の各項目の扱いの表）。主な変更: mailbox の読み書きを `\ECMU` を取った 1 つの
  interpreter entry の中で（`_Q79` との競合、A1。5330 の `_DSM` 1・2 と `_Q79` が全部 root の `Mutex (ECMU, 0)` の下で mailbox を書くことを
  逆アセンブルで確かめた）、`_STA` の後に VERSION を読み attach は `_REG`・EC の後（A2）、`UBCB` は pointer で memory の型を UAT で（A3）、
  driver は別に uncached で写像（A4）、lock の順（A5・A6）、function 0 の Buffer の bit を必須に（A7）、Notify の後は `_DSM` 2 を呼ばない（A8）、
  配置は AML の region で決める（B2）、ACK の規則（B3）、一覧の index・offset（B4）、向きは 1.x に無い（B1）、DP の pin・HPD は i915（B5）、
  通知の有効化の順（B6）、Phase を p001〜p004 に分け直した（E）。
- WS049 に足す公開の口（mutex を取った entry での callback、package の生成、notify の除去と直列化、`_CRS` の共通の解析、memory map の型の要否）を
  design §12 にまとめ、WS049 の新しい Phase として Q1 に提案した。
- §10 の人間の判断（公開の形、role の切り替え・SET_NEW_CAM を範囲外、1.x の配置だけ、向きの受け入れの変更、ws.md の HPD・pin の記述）を Q1 に返した。

## 再開点（第 2 版の後）

- §10 の判断を受けて ws.md（目標・受け入れ）を直す（ws.md の変更は判断の後）。
- p002 の前に: 仕様書で §2 を確かめる、UAT で 5330 の VERSION・`UBCB` と memory map の型・mailbox の記録・`CR0n` の対応・`_DSM` の所要を得る。
