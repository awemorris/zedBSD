<!-- awesome-plan project=zedbsd record=ws141-p007 -->

# ws141-p007: 全規約・license/類似・最終確認

Status: cleared
Disposition: normal
Parent: [WS141](../ws.md)
Queue: [完成までの承認と有限実行scope](../execution-20261009.md#完成までの継続承認2026-10-09)

## scope/criteriaと依存

全WSのchanged sourceを全文規約/ライセンス/GPL字面/設計類似/BLOBで確認、最終buildとhostを確認して最新mainへ統合。実機未実施の関門は保持。

依存: p003〜p006の最終changed source。software実装の依存と実機のwhole acceptanceを区別する。実機はユーザーが後で行うと回答済み。

## 規則と確認

C全文 `plan/coding-style.md`、Guardrailのsource/ownership/HAL/GPL/scanout規則を適用。固定Linux/Mesa一次sourceを照合し、GPL資料/改名表はignored tempのみ。named rpi4 build（warning/error0）と必要な短いhost検証、formatter/style補助+全文manualを実施。QEMUはQ1/T1、Master/共有記録は担当から更新しない。

## 設計変更の出典（2026-10-09）

ユーザー「完成まで自走してください。」と回答「WS141に実行器・compilerも含め、Keiland表示まで進める」。p006の別WS判断を本WS内実装に確定し、p005はこの出力をdesktop描画の依存とする。実機関門を削除しない。[全体変更/実行範囲](../execution-20261009.md#完成までの継続承認2026-10-09)。

## i15開始（2026-10-10）

public runtimeを含む最終software sourceの監査を開始。全文C/Guardrailを適用し、最終LTOのcaller/IRQ/間接callback・再帰上限、全WS規約/license/GPL字面・設計/BLOB、actual clientの二node/scanout契約を確認する。実機のwhole acceptanceは保持する。詳細・結果は[実行記録](../execution-20261009.md)。

## 全source software確認結果（2026-10-10）

[全文audit](../p007-software-audit.md)・[183path final hash](../final-source-sha256.tsv)・[16KiB ordinary stack](../final-stack-report.json)を保存。規約のguard/return/macro、予備fragment preflight、stack一時table、独自filter生成を修正。全14host/y-n build/Python・shell/diff PASS。final source監査に未修正code残件無し。main統合後にi15/p007を判定する。physical全関門は他Phase/WSに保持し、hostをKeiland画面成功としない。

## clearance（2026-10-10）

i15の最終source監査・build/host・main統合criteriaを満たしcleared。source0f56e5200、main merge5eb8867f3、183path hash一致、ordinary16KiB stack/y-n build/全14host PASS。[統合と終了記録](../execution-20261009.md#i15-main統合software実行の終了2026-10-10)。実機/KeilandとWS acceptanceは未達のまま保持し、remote closure/projectionはQ1へ残す。

## source変更による再開（2026-10-10）

ユーザーの共通shader refactor指示によりi15の最終source適用は失効、unclearedとして記録して今回i18へ再開。旧clearanceの結果/証拠は保持。新共通frontend・vendor caller/build依存を全文規約/境界/build/host/ARM64 stackで再確認する。[今回実行](../compiler-refactor-20261010.md)。

## i18最終source検証（2026-10-10）

共通compilerとi915/VC4 callerを再確認し、全文C/境界/Zlib origin、RPi4 y/n・amd64 i915 y build、対象host、final普通経路stack15680/margin704を確認。[今回commands/results/hash](../compiler-refactor-20261010.md#i17-software-clearancei18最終検証)。main統合/readbackまではin-progress、実機のwhole条件は保持。


## i18 clearance・main統合（2026-10-10）

今回最終sourceの全文規約/境界/license、対象build/host/ordinary stackとmain統合/readbackのcriteriaを満たしi18/p007をcleared。source `fca9cd37f`、main親 `0206f4f88`、merge `ab268b857`。対象212/212 source hash一致、build/host入力差分0、main/own/integration cleanを確認した。[今回終了と残件](../compiler-refactor-20261010.md#i18-main統合今回software実行の終了2026-10-10)。旧i15 clearance・source変更による失効/再開の履歴は保持する。RPi4/Keilandの実機条件は他Phase/WSに未達のまま保持し、remote closure/projectionはQ1へ保留する。
