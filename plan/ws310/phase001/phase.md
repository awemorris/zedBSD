<!-- awesome-plan project=zedbsd record=ws310-p001 -->

# ws310-p001: 調べと設計（呼ぶ _OSC と bit、確かめの方法と項目）

Status: in-progress（設計と review を終えた、Q1 の確かめ待ち）

- Queue: q1779-i01（P6、worktree `p6-ws310`、branch `agent/p6-ws310`、基点 main e2f816e14）。
- 範囲: `plan/ws310/` だけ。source は変えない。

## 成果

- [design.md](../design.md): 呼ぶ `_OSC`（PCIe の host bridge・platform-wide・processor）と bit、時期と順、内部の口、起動の switch `acpi.osc=`、失敗と回復、他の WS との関係、host・QEMU・5330 の前後の確かめ、WS の受け入れ、Phase の表。
- [design-review.md](../design-review.md): design-reviewer の敵対的レビューと反映。
- [user-questions.md](../user-questions.md): Q310-1〜6。

## 確かめた事

- 5330 の DSDT・SSDT を iasl で逆 assemble し、`_OSC` の 4 か所（`\_SB`・`\_SB.PRxx`・`\_SB.PC00`・`\_SB.IETM`）の振る舞いと、その状態を読む所を grep で洗い出した（design §3）。
- aml-host（worktree の build、2026-10-31）で 5330 の表に `\_SB._OSC`・`\_SB.PR00._OSC`・`\_SB.PC00._OSC` を評価した。`\_SB.PC00._OSC` の query で `\OSCC` が 0 → 0x18 に変わる（query でも状態を変える firmware）事を確かめた。memory は 0 の模型なので値は実機の物ではない。

## 未実施

- ACPI 6.5・PCI Firmware Specification・Intel の processor の `_OSC` の文書の取得（uefi.org が bot の確かめの page を返した。回避していない。Q310-6）。
- 5330 の実機の値の読み（p003）。
