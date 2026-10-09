<!-- awesome-plan project=zedbsd record=ws141-p007 -->

# ws141-p007: 全規約・license/類似・最終確認

Status: in-progress
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
