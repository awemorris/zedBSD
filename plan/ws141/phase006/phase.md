<!-- awesome-plan project=zedbsd record=ws141-p006 -->

# ws141-p006: kernel Vulkan実行器とSPIR-V compiler

Status: in-progress
Disposition: normal
Parent: [WS141](../ws.md)
Queue: [完成までの承認と有限実行scope](../execution-20261009.md#完成までの継続承認2026-10-09)

## scope/criteriaと依存

既存Venus wire/libvulkanを使うkernel実行器とV3D4.2のSPIR-V compilerを追加し、Keilandの描画/表示経路を成立させる。未実装capabilityは公開しない。

依存: p004 native job/cache/MMUとp005 resource/share。software実装の依存と実機のwhole acceptanceを区別する。実機はユーザーが後で行うと回答済み。

## 規則と確認

C全文 `plan/coding-style.md`、Guardrailのsource/ownership/HAL/GPL/scanout規則を適用。固定Linux/Mesa一次sourceを照合し、GPL資料/改名表はignored tempのみ。named rpi4 build（warning/error0）と必要な短いhost検証、formatter/style補助+全文manualを実施。QEMUはQ1/T1、Master/共有記録は担当から更新しない。

## 設計変更の出典（2026-10-09）

ユーザー「完成まで自走してください。」と回答「WS141に実行器・compilerも含め、Keiland表示まで進める」。p006の別WS判断を本WS内実装に確定し、p005はこの出力をdesktop描画の依存とする。実機関門を削除しない。[全体変更/実行範囲](../execution-20261009.md#完成までの継続承認2026-10-09)。


## trusted native loweringの依存（2026-10-09、i12）

[p004](../phase004/phase.md#i12のnative-job診断と後続interface2026-10-09)でCL/TFU/CSDのkernel内部job型/runnerを追加。p006はSPIR-V出力のQPU shader/uniform/attribute/texture、CLの間接list/pool/targetをp005のownerへ紐付け、入力CPU clean、GPU completion/必要なTMU clean、CPU output invalidateの境界を守る。retired=falseのfailureはp005/common recoveryにquarantineを渡し、Vulkan完了として通知しない。まだcapabilityを公開しない。[i12結果と制限](../execution-20261009.md#i12-native-v3dv1v10のsoftware結果2026-10-09)。

## p005 resource ownerの出力契約（2026-10-09、i13）

rendererのsession-local protocol resource IDとglobal native VAは別に管理する。resource viewの独立referenceをjobが保持し、common callbackの終了とuncertain DMAのstorage retirementを分離する。CPU/cache/view編集はcontroller mutexでnative executionから排他。BLOBは現段階のstorage登録ではblob_id=0だけを受け、p006はVulkan allocation objectへのnonzero blob_id bindingを追加する。COMMAND/CAPSETは実行器・compilerの完成後、登録前にbindする。allocation-only stopからworker join/callback drainへ置換する依存を保持。[i13](../execution-20261009.md#i13の二deviceallocation共有の実装2026-10-09継続中)。


## p005 workerのscoped prerequisite（2026-10-09）

private workerとjob tableはactual source host/buildで確認済み。prepared trusted payloadが独立VA viewをretainし、FIFO executor/disposerがcontroller mutex内、common completionはlock外でFINISHING後にslotを返す。Vulkan queue作成/破棄がIRQ guard下のsession timeline ownershipを管理する。最大8supervised marker/16全slot、commit allocation0、decoder completionとactual GPU completionの区別を保つ。p005 whole clearanceを依存出力として偽称せず、[このsource出力](../execution-20261009.md#i13-checkpoint-native-workerとsupervised-reservation2026-10-09)を使用する。COMMAND/CAPSET/JOB公開はp006の全bindingが完成してから。


## i14開始とcompiler部品（2026-10-09）

[統合済みowner/worker](../execution-20261009.md#i13-worker統合とi14-compiler開始2026-10-09)をscoped prerequisiteとして開始。既存device-independent Zlib scalar SPIR-V parserをread-only source再利用し、QPU4.2 encoder、register/value管理、VPM vertex/fragment interface、uniform/TMU/出力/終了を新規実装する。未実装capabilityを公開しない。kernel Vulkan wire/object/descriptor/queue実行器とnonzero blob bindingは後続で接続。実機whole acceptanceは後日。

## compilerのsoftware checkpoint（2026-10-09）

独立QPU encoder/scalar compilerを実装し、actual Keiland quad/panelの6stage variantsと各fragmentのblend/swap組合せを確認。固定Mesa decoder/repackerで全native wordsを照合、別scalar IR interpreterとの32input差分、actual uniform consumption/TMU4result/target4channelとnative4allocation refusalのownership unwindをPASS。RPi4 named build exit0・warning/error0。scalar証拠をnative GPU実行やWS clearanceとは扱わない。unsupported operations/control effectsは拒否しcapabilityを公開しない。[正確な範囲、制限、次のVulkan runtime](../execution-20261009.md#i14-compilerのsoftware-checkpoint2026-10-09)。
