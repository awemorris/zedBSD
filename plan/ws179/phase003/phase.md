<!-- awesome-plan project=zedbsd record=ws179-p003 -->
# ws179-p003: 規約の全文の見直しと accent の AAT のシナリオ

Parent: [WS179](../ws.md)
Status: in-progress（2026-10-08 P1 q875: AAT のシナリオと helper を足した、QEMU は T1。規約の全文の見直しはユーザーの決定でベータ3）
Disposition: normal
Queue: q875（P1、2026-10-08 Q1 の承認「5330 なしで進められる小さい物を順に」の 6 番。規約の部分は除く）
依存: p001・p002

## 範囲

1. AAT のシナリオ `tests/scenarios/desktop/appearance/accent.md`（新）と helper `desktop.appearance.accent`（`plan/tools/aat/scenarios/helpers_desktop.py`）。
2. 規約の全文の見直し（WS179 の変えた C の全部）: **ベータ3**（2026-10-08 ユーザー「コーディング規約による整形はベータ3でやります。」）。この Queue では扱わない。

## 実装（2026-10-08 P1）

- シナリオ: Files を App Home から開き、Settings を Appearance の頁に。Accent の 2 番目の丸（Purple、control 91）→ `ZSETTINGS ACCENT index=1`・`KWL THEME appearance=N accent=1`、撮影 `purple`。1 番目（Blue、control 90）→ `index=0`・`accent=0`、撮影 `blue`（最後は既定の青に戻す）。Files の `ZFILES ACCENT index=N` は記録だけ（app の log が session の log に入らない時に fail にしない）。見えは needs-person。
- suite: `tests/suites/full.suite` の `desktop.appearance.*` に入る。
- 確認: `python3 plan/tools/aat/check-scenarios.py`（scenarios 108、PASS）、`python3 -m py_compile`、`helpers_desktop.py --list` に `desktop.appearance.accent`。host の settings の renderer で Appearance の頁の control 90〜97 が y=235（窓の中、scroll なし）にあることを確かめた。
- QEMU（T1）: AAT `--only 'desktop\.appearance\.accent'`。未実施。

## 積み残し

- 準正常系 3 件は WS177 の backlog（p001 の記録のとおり）。
