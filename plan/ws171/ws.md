<!-- awesome-plan project=zedbsd record=ws171 -->
# WS171: hal.h の全ての関数に契約の comment を書く

Status: planning（2026-10-05 追加、急がない（ユーザー「今でなくていい」）、段は未定。見積もり 2 LW（Q1 の概算: hal.h の関数は約 150、architecture の実装の調べを含む））
Master: [master](../master.md)
Primary Milestone: MG008

## 由来

ユーザー（2026-10-05、WS052 の HAL v2 の review の後）「hal.hの追加をレビューしました。OKです。ですが、今回の追加で、関数のコメントがすばらしかったです。規約もしっかり書かれていて、とてもよいと思いました。このコメント、すべての関数につけてほしいです。今でなくていいので、WSを作っておいてください。」

## 目標

`include/hal/hal.h` の全ての関数（と macro・型の契約）に、HAL v2（commit 29f954f5、`hal_cpu_idle_suspend`・`hal_irq_set_wake`・`hal_cpu_notify` など）と同じ水準の契約の comment を書く:
呼び出しの条件（文脈・割り込み・lock）、何をするか、戻る時の状態、返り値（HAL_OK と各 error の意味）、他の HAL の操作との関係、architecture が対応しない時の扱い。
「XXX: Add explanation.」を無くす。

## 制約

- comment だけの変更でも hal.h の変更なので、差分を plan に置いてユーザーの review を得てから当てる（Guardrail の HAL）。宣言・型・意味は変えない。
- 契約は今の実装（amd64・i386・arm64・sparcv9・m68k・UP）が実際に満たす物を書く。実装と合わない所は契約を勝手に決めず、bug かユーザーの判断として出す。
- 規約は plan/coding-style.md（comment の形）。

## Phase

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 関数の一覧と今の comment の状態、各 architecture の実装の調べ、書き方の雛形（HAL v2 の comment を基に） | cleared 候補（2026-10-09 P1: 130 関数、contract 20・short 62・none 42・placeholder 6、[inventory.md](phase001/inventory.md)） | — |
| p002 | 差分の案（領域ごとに分けてよい: CPU・memory・IRQ・timer・console・rtc ほか）、ユーザーの review | planning | p001 |
| p003 | 承認された差分の適用、build、実装と合わなかった点の bug 化 | planning | p002 |
