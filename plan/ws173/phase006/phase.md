<!-- awesome-plan project=zedbsd record=ws173-p006 -->

# ws173-p006: シナリオの選択の helper（git の範囲 → paths）と suite の runner

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-202c の smoke・full、T1-232・T1-305 などで runner が使われている）（旧: test-done（2026-10-07 q834 P2: runner は T1 の AAT の実行（T1-202c の smoke・full、T1-232、T1-305 など）で target で使われ、選択（`changed:`・suite・id・pattern）は host の git だけで決まる。判定は Q1）（旧: in-progress（2026-10-05 夜、host の確かめまで）））
Disposition: normal
Parent: [WS173](../ws.md)
Queue: Q1 の指示（2026-10-05 夜「p006（paths で選ぶ helper）へ進んでよい」）
依存: [p004](../phase004/phase.md)（シナリオ・suite・runner）

## 範囲

[plan/tests.md](../../tests.md) §5「変更: 変えた file に `paths` が重なるシナリオ＋ smoke。git の範囲から選ぶ helper を作る」と、suite を流す runner。runner は p004 で作った `plan/tools/aat/run-aat.sh`（suite・id・pattern・`area:`）で、ここでは変更からの選択を足す。

## 作った物

- `plan/tools/aat/select-scenarios.py [RANGE] [--no-smoke] [--all-status] [--explain] [--gaps]`:
  - RANGE は git diff の範囲（既定 `main...HEAD` = branch の自分の変更）。commit 1 つならその commit の変更（merge の commit も `-m` で）。
  - 変えた file が、シナリオの `paths` の 1 つの下（`/` で終わる path は directory、他は file かその前置き。`…/keyboard` は `keyboard.c`・`keyboard-layout.c` を含む）にあるか、シナリオの文書そのものが変わった時に選ぶ。smoke の suite を足す（`--no-smoke` で外す）。active だけ（`--all-status` で draft も）。
  - 順は full の suite の順（起動が先、log out・shutdown が最後）。`--explain` は選んだ理由、`--gaps` はどのシナリオにも当たらない source の変更（`tests/`・`userland/tests/`・`plan/`・`docs/` 以外）を標準エラーに出す（シナリオを足す候補）。
- runner の `changed:RANGE`: `run-aat.sh TARGET OUTDIR changed:main...HEAD` で同じ選択を流す。記録の名前は `changed-<範囲>`。

## 判断（P2）

- 選択は保守的に広く: `src/` のような広い path を持つ起動のシナリオは kernel の変更でいつも選ばれる。狭める時はシナリオの `paths` を細かくする（helper は path の文字列だけを見る）。
- `plan/`・`docs/` の変更は振る舞いを変えないので何も選ばない。suite の file の変更も同じ。

## 確かめ（host）

- `sh plan/tools/aat/tests/run-host.sh` → `aat-host: PASS`（足した項目: 固定の変更の一覧で、phone の source → phone の 2 本、calendar の文書 → その 1 本だけ、smoke、`include/uapi/hidraw.h` が gap、`plan/` は何も選ばない、`--no-smoke`、順、`covers` の前置きと directory）。
- 実の commit で: `select-scenarios.py c138d72c --explain`（imageview と notes の main.c）→ imageview と notes の 4 本と smoke の 8 本。runner の `changed:c138d72c` も同じ 12 本を選ぶ。

## 未実施

- （済み、2026-10-07）target での runner の実行は T1-202c 以降。
- plan/tests.md §5 への使い方の追記（Q1 の文書）。
