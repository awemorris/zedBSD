<!-- awesome-plan project=zedbsd record=ws310 -->

# WS310: ACPI の _OSC を呼び、システム全体の挙動を厳重に確かめる

Status: planning
Primary Milestone: MG004
Resume point: p001 の設計を終えた（[design.md](design.md)、review は [design-review.md](design-review.md)）。次は Q1 の確かめと [user-questions.md](user-questions.md) の Q310-1〜6 の答え、その後 p002（aml-host の option）と p004（codec）を並べて。

## ユーザーの決定（原文、2026-10-29）

WS301 の Q301-3（ACPI の _OSC を呼ぶか）への回答:「最終的には呼ぶが、別なWSにして、システム全体の挙動が問題ないかを厳重にチェックする。今は呼ばない。」

## ユーザーの決定（要点）

- 2026-10-31 Q1 から、ユーザーが不在の間（昼頃まで）設計だけ進められる WS は設計を進める、という趣旨の指示を受けた。WS301 は WS333 に統合され、WS333 の CPU の枠組みと熱が main に入ったので、この WS の設計の時期になった。
- 2026-10-31 Q1 から、ACPI 6.5 の仕様を手で置く件は cockpit の 01（WS333 の同じ件）にまとめてユーザーに聞く、Q310 の問いには「01 と同じ件」と書く、という趣旨の指示を受けた（Q310-6）。

## 目標

zedBSD が ACPI の _OSC で OS の能力を firmware に申告し、firmware が出す情報（_CPC など）を使えるようにする。呼ぶ前と後で、電源・PCIe・熱・CPU の性能などシステム全体の挙動に問題が無いことを、実機（まず 5330）で厳重に確かめる。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 調べと設計（呼ぶ _OSC と bit、確かめの方法と項目） | in-progress（設計と review を終えた、Q1 の確かめ待ち） | WS333 p006・p011（CPU の枠組み）・熱 |
| p002 | aml-host の option（`--set`・`--memory-file`・`--trace`・`--diff`・`--call-every-method`）と `_OSC` の ASL の試験 | planning | p001 |
| p003 | 5330 の入力の読み（`/dev/acpi`・Debian の dmesg、読むだけ）と host の評価、`osc-effects-5330.md` | planning | p002、5330 の割当 |
| p004 | `acpi-osc-codec`（純粋な関数）と host 試験 | planning | p001 |
| p005 | `acpi-osc.c` の統合（3 段・MADT・host bridge・PCI の口・log）、`acpi.osc=` の語（既定 off）、QEMU の試験 | planning | p004 |
| p006 | 5330 の前後の比べ（段ごと、1 本の script） | planning | p003、p005、Q310-2 |
| p007 | 既定の変更（Q310-1）・docs・耐久（Q310-5） | planning | p006 |
| p008 | 規約の全文の見直し | planning | p007 |
