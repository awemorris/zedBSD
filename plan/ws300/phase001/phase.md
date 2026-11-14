<!-- awesome-plan project=zedbsd record=ws300p001 -->

# ws300-p001: 調査と設計（VMD・root port・NVMe の S0 idle、APST・ASPM・LTR）

Phase ID: `ws300-p001`
Parent: [WS300](../ws.md)
Status: cleared（2026-11-15 Q1）
Phase disposition: normal
Queue: q1683-i01（P6）

## 範囲

- 5330 の調べ（[survey-5330.md](../survey-5330.md)、P5）と公開の資料（[survey-fable.md](../survey-fable.md)、P5）から、p002〜p005 の設計を書く。design-reviewer を 1 回。
- code は書かない。

## 結果

- 2026-10-29 P5（q1666-i03・i04）: 調べの 2 つ、design.md の下書き §0〜§3。
- 2026-10-29 P6（q1683-i01）: design.md 第 1 版（§4〜§14: 各 Phase の設計、順、lock、失敗、UAPI、試験、Phase と受け入れ、出典、推しで決めた事 Q300-1〜5、未確認）。
- 2026-10-29 P6: design-reviewer、第 2 版（§15 に 23 件の扱い。code の反映は d503c39ea）。
- 2026-11-15 P5（q2114-i01）: merge されなかった branch `agent/p6-ws300` から design.md 第 2 版・design-review.md・p001〜p005 の phase.md を main に移し、design.md を第 3 版にした
  （今の main 5202274642 に合わせた: p002 は BUG-424、build は top の Makefile と `mk/`、計算は header の inline。差は design §16）。design-reviewer は第 3 版にはかけていない
  （第 3 版は第 2 版の設計の範囲の中の置き場所の変更と、BUG-424 の事実の反映）。p001 の cleared は branch の記録（P6）で、Q1 の確認は未。

