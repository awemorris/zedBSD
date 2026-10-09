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

## Vulkan session/objectのsource出力（2026-10-09、継続中）

compilerはmainへ統合済み。新private Vulkan session/object ownerを追加し、typed ID/session isolation、registry/dependency/prepared workの独立reference、old identity再利用、closing namespace withdrawal、live ownerが残るcloseのEBUSY/arena保持、actual partial allocation unwindをhostで確認。caller controller mutex/worker joinを前提にしたsource部品であり、nodeのcommand/capset/runtimeへは未接続。private software gateはPASS、whole p006はin-progress。[範囲/確認/復帰点](../execution-20261009.md#i14-vulkan-sessionobjectのsoftware出力2026-10-09)。


## Vulkan transportのcheckpoint（2026-10-09）

実client wire writerでSET/SEEK/VERSION/外部streamを検証しPASS、named rpi4 y build warning/error0。明示reply capacity/retained CPU owner/最後のrelease atomic trailerと外部streamのbounded immutable copyを実装。typed command callbackは未接続、native GPU completionとは別。p006/i14はin-progress、次はinstance/device/memory/pipeline/draw/queue。詳細は[実行記録](../execution-20261009.md#i14-vulkan-streamのsoftware出力2026-10-09)。


## HOST_COHERENT memoryのmapping依存（2026-10-09）

実libvulkanのdiscoveryはhost coherent memoryを必須とする。cached V3D RAMをcoherentと偽らず、既存HALのNormal NC kernel aliasと同属性のuser translationを使う。shared GPU/VMの4 pathの具体的差分を[依存提案](../uncached-ram-mapping-proposal.md)へ用意し、未適用。担当source境界のため承認を求める。HAL API変更無し。private instance/device/runtimeは独立継続、COMMAND/CAPSETをdependency未達で公開しない。p005/p006 in-progress、Master/shared projectionはQ1。


## native root/queryとNormal NC memoryのcheckpoint（2026-10-09）

ユーザー承認のshared4 pathを適用し、private bufferのNormal NC alias lifetimeとrender/display→VM cache属性を接続。instance/physical/device/queue parent graphとexact timeline/domain所有、callbackが残るdomainのreuse拒否、native busy/faultをidle成功にしない処理、actual client codecでの有限physical queriesを追加。actual source host2 PASS、rpi4 y build warning/error0。native runtimeのmemory budget/limit enforcementは未接続、COMMAND/CAPSET/JOB未公開。次はVkDeviceMemory/nonzero BLOB、resource/pipeline/draw/queue。p006/i14 in-progress。[scope/commands/hash/制限](../execution-20261009.md#i14-vulkan-native-rootqueryとnormal-nc-owner2026-10-09)。

## i14 native memory/BLOB checkpoint（2026-10-09）

実client wireでlazy VkMemory/type0/export/importとnonzero placed BLOBを実装。actual VA/MMU sourceを含むhostでindependent references・declaration budget・OOM・failed flush quarantine/recovery PASS、rpi4 y build warning/error0。void commandのactual header requested=1を照合して先行root destructionも修正。COMMAND/CAPSET/JOBはまだ未公開、次はbuffer/image/layout/bindingとtyped rendering runtime。[正確な出力/修正/制限](../execution-20261009.md#i14-vkdevicememoryとplaced-blobのsoftware出力2026-10-09)。p006/i14はin-progress、Keiland/実機/最終適合は未達。

## i14 buffer/image/binding checkpoint（2026-10-09）

typed create/destroy/requirements/bind/linear layoutを実client codecへ接続。logical buffer extentとrounded requirementを分離、raster colour image pitchを定義し、independent binding/prepared ownerをactual VA source hostで確認。host3範囲/rpi4 y build PASS、style total0。public runtime未公開、次はview/sampler/shader moduleとdescriptor/pipeline/draw。[設計/失敗と修正/結果/制限](../execution-20261009.md#i14-bufferimagerequirementsbindingのsoftware出力2026-10-09)。p006/i14 in-progress、Keiland/実機/p007未達。

## i14 immutable input checkpoint（2026-10-09）

actual client handle/record encoderとnative sourceでcolour view・nearest/linear sampler・copied SPIR-V moduleを追加。image/root/module/jobの独立保持、registry OOM、arena/stream overwrite後のactual Keiland module byte保持を確認。host4範囲/rpi4 y build/style PASS。stage/entry/native compiler keyはpipeline createの後続、public runtimeは未公開。[結果/失敗と修正/制限/再開](../execution-20261009.md#i14-immutable-viewsamplerspir-v-moduleのsoftware出力2026-10-09)。次はdescriptor/pipeline layout、p006/i14 in-progress。
