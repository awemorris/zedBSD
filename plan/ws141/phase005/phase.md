<!-- awesome-plan project=zedbsd record=ws141-p005 -->

# ws141-p005: GPU統合とdisplay/render共有

Status: in-progress
Disposition: normal
Parent: [WS141](../ws.md)
Queue: [完成までの承認と有限実行scope](../execution-20261009.md#完成までの継続承認2026-10-09)

## scope/criteriaと依存

二deviceのresource/scanout共有、既存GPU APIを介したnative job/display統合。Keiland描画の依存実行器はp006。

依存: p003とp004の必要な実source/host出力。software実装の依存と実機のwhole acceptanceを区別する。実機はユーザーが後で行うと回答済み。

## 規則と確認

C全文 `plan/coding-style.md`、Guardrailのsource/ownership/HAL/GPL/scanout規則を適用。固定Linux/Mesa一次sourceを照合し、GPL資料/改名表はignored tempのみ。named rpi4 build（warning/error0）と必要な短いhost検証、formatter/style補助+全文manualを実施。QEMUはQ1/T1、Master/共有記録は担当から更新しない。

## 設計変更の出典（2026-10-09）

ユーザー「完成まで自走してください。」と回答「WS141に実行器・compilerも含め、Keiland表示まで進める」。p006の別WS判断を本WS内実装に確定し、p005はこの出力をdesktop描画の依存とする。実機関門を削除しない。[全体変更/実行範囲](../execution-20261009.md#完成までの継続承認2026-10-09)。


## native出力との所有契約（2026-10-09、i12）

[p004](../phase004/phase.md#i12のnative-job診断と後続interface2026-10-09)のprivate runnerは同時実行を一つに制限し、launched failureではretired=falseを返す。p005のworker/resource ownerは全allocationとVAを保つ独立referenceを持ち、timeout/faultのsession close/destroyでDMA storageを返さない。common recoveryによる全owner退去とnative reset/表flush成功の後にだけ解放/VA再利用する。runnerが公開clientの裏でresetしない点を保持する。i12のsoftware統合確認後にi13へ進む、実機whole acceptanceは別に保持。[全体結果](../execution-20261009.md#i12-native-v3dv1v10のsoftware結果2026-10-09)。

## i13開始（2026-10-09）

必要なnative出力はmain `30350c8cba31875a01d58f804a5de13a1305a7ee`へ統合され、host/buildで確認済み。共有allocation/reference、placed blob、scanout import、renderer VAとworker/recoveryのsoftwareを実装する。実機whole acceptanceは後日。任意companion_idは現在のcommon APIから取得できず0を保持し、device roleとforeign constraintsで二deviceを選択する。[実行記録](../execution-20261009.md#i12統合確認とi13開始2026-10-09)。

## i13 checkpoint: 二device/native allocation共有（2026-10-09）

rendererを独立登録し、低1GiB placed blob・同device新VA import・foreign native scanout・independent DMA hold・failed MMU flush quarantine/common reset回復を実装。actual source hostとrpi4 y build PASS、warning0、C/style/630旧名チェック0。[exact source/commands/制限](../execution-20261009.md#i13の二deviceallocation共有の実装2026-10-09継続中)。非同期worker/common completion/job/desktopは残り、in-progressを維持。現在のallocation-only stop contractはcommand capability公開前にworker retirementへ更新する。
