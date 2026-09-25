<!-- awesome-plan project=zedbsd record=queue-q316 -->

# Queue q316: refactorの対応表と、WS034の版・依存グラフ

<!-- awesome-plan-current:start -->
Status: finished
Active Queue: none（q316 finished）
Executor: root（このエージェント）。Phaseはサブエージェントで並行実行
Last Queue: q314 finished（履歴 `plan/history/queue-q314.md`）。WS032のq315 finished（`plan/ws032/queue-ws032.md`）
<!-- awesome-plan-current:end -->

Approval: current user「では、commitしてから、まず1つqueueを実行してみてください。」（2026-09-23、q316案の提示後）
Start UTC: 2026-09-22T18:13:35+00:00
Prepared: 2026-09-23
運用方針: `plan/master.md`「実行体制とQueue運用方針」（最大2 Phase、サブエージェントで並行、root だけが `git commit -m WIP`、pushしない）

## 選んだPhase

| Order | Attempt | Phase | Status | Scope | 実行 |
| --- | --- | --- | --- | --- | --- |
| 1 | q316-i01 | [ws035-p001](../ws035/phase001/phase.md) | cleared | refactorの移動対応表・参照一覧・HAL配下の変更行・kernel include経路の保証案・refactor前のbuild基準・VFIOホストの読み取り確認 | phase-runner（Opus 5.5 High） |
| 2 | q316-i02 | [ws034-p001](../ws034/phase001/phase.md) | cleared | WS034全パッケージの版・入手元・SHA-256・ライセンス・build系・置き場所、依存グラフ、CA bundleとmesonの案 | phase-runner（Opus 5.5 High） |

## 選んだ理由

- 優先順位の先頭はrefactorで、その前提がws035-p001の対応表である。
- ws034-p001は、WS034の全パッケージPhaseの前提である。
- どちらも文書だけを作り、ソースを変更しない。ファイル範囲（`plan/ws035/` と `plan/ws034/`、`build/distfiles`）が
  重ならないので並行できる。人間の判断を途中で要しない。

## 依存

ws035-p001、ws034-p001ともに前提なし（計画済み）。i01とi02の間に依存は無い。

## 時間と上限

各180分の見積、Queue全体で約180分（並行）。build 1回1800秒、取得1件600秒、ssh 1回60秒。
同条件の変更なしretryは3回まで。aggregate `make check` は使わない。

## 権限の範囲

- ホスト（Latitude 5330）へのsshは読み取りだけ。状態を変える操作はしない。
- パッケージの取得（`build/distfiles`）はネットワークを使う。
- HAL・UAPI・ソースは変更しない。GitHubへの公開はしない（同期キャッシュ未構築のため）。

## 終了後にrootが行うこと

結果の確認と記録、Phaseの状態更新、WS034・WS035の計画の修正（依存の食い違い、HAL承認の依頼材料）、
`git commit -m WIP`、次のQueue案の提示。ws035-p001で出たHAL配下の変更行は、refactor開始前にユーザーへ承認を求める。

## 結果（Finish UTC: 2026-09-22T18:47:44+00:00）

| Attempt | Phase | 結果 | 主な成果 |
| --- | --- | --- | --- |
| q316-i01 | ws035-p001 | cleared（受け入れ6条件すべて） | 移動対応表（248ファイル）、参照一覧のscript、HAL書換え6行（p004）、kernelのlibcヘッダ読込みの監査script、refactor前build基準（amd64・i915-amd64のみ成功）、5330のHDAはIOMMU group 15で単独パススルー不可 |
| q316-i02 | ws034-p001 | cleared（受け入れ4条件すべて） | package-inventory（tarball 53件、size・SHA-256実測）、依存グラフ、Phase表の修正案20行（rootが反映）、CA bundleとmesonの案 |

実行: 2つのphase-runner（Opus 5.5 High）を並行。ソース・HAL・UAPIの変更なし。
詳細は各Phaseの `results.md`。判断が要る点はWS035・WS034の `ws.md` の未決事項に記録した。
