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

## i14 canonical layout checkpoint（2026-10-09）

actual client recordでdescriptor/pipeline layoutを追加。canonical bindings・same-device immutable sampler・finite set/push interfaceとindependent graph retentionをhostで確認。host5範囲/rpi4 y build/style PASS、次は512-set pool/set/updateとimmutable draw snapshot。public runtime未公開、p006/i14 in-progress。[結果/制限/再開](../execution-20261009.md#i14-canonical-descriptorpipeline-layoutのsoftware出力2026-10-09)。

## i14 pool/set checkpoint（2026-10-09）

actual client recordでfinite pool/reset/destroyとall-or-nothing set batch allocate/freeを追加。old prepared setのindependent graph/charge保持とcapacity reuse、2nd registry OOMの全rollbackをnative hostで確認。host6範囲/rpi4 y build/style PASS、next ordered descriptor update/copyとdraw snapshot。public runtime未公開、p006/i14 in-progress。[結果/限界/復帰点](../execution-20261009.md#i14-descriptor-poolset所有のsoftware出力2026-10-09)。

## i14 descriptor更新 checkpoint（2026-10-09）

actual client selected-field framing/generated copy recordでordered writes/copiesを接続。complete staged validation/rollback、destination immutable override、exact logical uniform interval、independent draw snapshotsのmutable set/reset/public retirement非依存をhostで確認。host7範囲/RPi4 y build/style PASS、public runtime未公開、p006/i14 in-progress。[結果/修正/限界/復帰点](../execution-20261009.md#i14-ordered-descriptor更新とdraw-snapshotのsoftware出力2026-10-09)。next render pass/framebuffer/compiled pipeline、native command/queue/common binding。

## i14 target checkpoint（2026-10-09）

single-colour render pass/framebufferをactual client recordへ接続。clear/load/present mapping/backdrop dependencyを保持、bounded native target compatibility/ownershipとprepared graphを確認。host8範囲/RPi4 y build/style PASS、public runtime未公開、p006/i14 in-progress。[結果/限界/復帰点](../execution-20261009.md#i14-render-passframebufferのsoftware出力2026-10-09)。next compiled graphics pipeline/recorded native draw/queue/common binding。

## i14 compiled pipeline checkpoint（2026-10-09）

same-device module/layout/passとactual native compilerを接続。唯一main entry/stage・canonical varying/FIFO・descriptor/push permission・attribute interfaceを確認しpartial compile/OOMを全退役。host9範囲/RPi4 y build/style PASS。wire create/destroy/public runtimeは未接続、native codeのGPU uploadはprepared draw ownerの後続責務。p006/i14 in-progress。[結果/限界/復帰点](../execution-20261009.md#i14-compiled-pipeline-graphのsoftware出力2026-10-09)。

## i14 pipeline wire checkpoint（2026-10-09）

actual private client selected-state encoderをreadonly wrapperで実行、independent native finite decoder/complete batch/partial member resultsとcompiler/typed ownerへ接続。host9範囲/RPi4 y build/style PASS、single compiled stack framesを確認、total public runtime call pathは後続。p006/i14 in-progress、COMMAND/CAPSET/JOB未公開。[結果/限界/復帰点](../execution-20261009.md#i14-graphics-pipeline-wire-batchのsoftware出力2026-10-09)。next recorded command/native prepared draw/queue/common binding。

## i14 primary/graphics recording checkpoint（2026-10-09）

actual client primary pool/buffer codecsとreal public9vkCmdのfinite native recordingを接続。whole batch rollback、pending mutation拒否、pool非cycle/registry退役とindependent old graph、selected colour clear/raw state/typed interfaces、first node OOM→End failure/clean re-recordを確認。ordinary descriptor update-after-recordの想定をVulkan 1.0仕様へ修正し、set generation/current validationとpending update拒否を追加。host11範囲/RPi4 y build/style PASS、actual native CL/GPU completionの証拠にはしない。p005/p006/i13/i14 in-progress、COMMAND/CAPSET/JOB未公開、Keiland/実機/p007未達。next immutable current draw preparation/native VA/code/uniform/TMU/CL、transfer/barrier/queue/common/public runtime。Master変更無し。[正確な結果/想定訂正/失敗と修正/限界/復帰点](../execution-20261009.md#i14-primary-command所有とactual-vkcmd記録のsoftware出力2026-10-09)。

## i14 ordered draw state checkpoint（2026-10-09）

exact push ranges/canonical set prefix compatibilityとdescriptor disturbance、stage別push/partial vertex/dynamic stateの2回walkを実装。logical fetch/compiled uniform/whole target/sample backing/alias feedbackをnative準備前に確認。actual host12範囲/RPi4 y build/style PASS。callbackはborrowed state観察のみ、独立native prepared owner/CL/DMA/queue/public bindingは後続、p006/i14 in-progress、Keiland/実機/p007未達。[結果/fixture correction/限界/再開](../execution-20261009.md#i14-ordered-draw-stateのsoftware出力2026-10-09)。

## i14 prepared CPU graph checkpoint（2026-10-09）

独立primary graph/consumed descriptor snapshots、distinct ordinary set/command pending charges、全OOM/counter rollback、pending free/reset/destroy guardを追加。actual host13範囲/RPi4 y build/style PASS。native code/VA/CL/DMA/queueは未接続。workerはfalse-retirement payloadを自動再disposeしないため、actual native disposerで明示quarantine transfer/checked reset後のreleaseを接続する残条件を記録。p006/i14 in-progress、Keiland/実機/p007未達。[正確な契約/結果/限界/再開](../execution-20261009.md#i14-immutable-prepared-cpu-graphのsoftware出力2026-10-09)。

## i14 native recordとtexture配置のcheckpoint（2026-10-09）

[実行記録](../execution-20261009.md#i14-native-shaderfetchtexture-recordとuif変換のsoftware出力2026-10-09): private 4.2 shader/attribute/texture/sampler serializerとstrict non-XOR UIF pixel変換を追加。callerはactual VPM capacity、uploaded code/uniform/default/fetch/scratchの独立owned intervalsを提供し、VCMを2 batchesとする。texture scratchの作成は先行image writeのnative completionとCPU visibility後のFIFO execution時。final fragment switchでscoreboardを取得し、real centre WをRF0へ供給するshader flagを有効にする。

確認: [native-state-host-test.sh](../tests/native-state-host-test.sh) / [XMLと逆pixel oracle](../tests/native-state-check.py) で8 full record・4847 pixels/padding/raster・atomic refusal PASS、rpi4 y build warning/error0、style total0。GPU upload/CL/queue/public bindingと実機は未達。near-final full-standard/license/similarityはp007で再確認する。Phase in-progress、変更は同Phase内、foreign interface/dependency変更無し。

## i14 native upload owner checkpoint（2026-10-09）

[詳細](../execution-20261009.md#i14-native-upload-storageのsoftware出力2026-10-09): private cached code/scratch allocationとnative VAの独立ownerを追加。actual PA bitsで配置し、compiled codeのlittle-endian upload/full padded cache cleanを実施。retired=falseは全保持、trueはNULL消費し、failed translation retirementをnative space quarantineが保持。whole prepared job quarantineとclosing session lifetimeは後続接続のまま。

[hardware host](../tests/v3d-hardware-host-test.sh) のactual mapping/refcount/cache/reset failure boundaryとcode bytes/padding確認がPASS、rpi4 y build warning/error0/style total0。fixtureの旧typed BLOB参照/linkとreset error期待を追従し、actual Vulkan source成功をmockで代替していない。public runtime/GPU launch/Keilandは未接続、Phase/WSをcleared/completedにしない。変更は同Phase内でforeign interface無し。

## i14 integer viewport uniforms checkpoint（2026-10-09）

[詳細](../execution-20261009.md#i14-integer-viewport-uniformのsoftware出力2026-10-09): copied IEEE viewportからXY shader scale/depth range/offsetをkernel integerのみでatomic生成。guard/round/stickyとnearest-evenを使用し、subnormal/逆depth/±zeroを保持。native-state hostのindependent host FP oracleで256境界＋1024固定sampleが全bits一致、既存XML/pixelもPASS、y build warning/error0/style total0。

次にuniform streamとFIFO時点のUBO read、owned descriptor pointers/native storage/quarantineを接続する。general softfloat/public API/HAL変更は無し。同Phaseのinternal loweringで、foreign dependencyを変更しない。Phase in-progress、actual GPU/Keiland acceptance/p007未達。

## i14 FIFO scalar uniforms checkpoint（2026-10-09）

[詳細](../execution-20261009.md#i14-fifo-scalar-uniform-streamのsoftware出力2026-10-09): actual compiled順のconstant/push/viewport/UBO/current coherent read/native descriptor addressをindependent CPU streamへ完成。whole prefix OOM/late refusal解放、typed logical bounds、stage別copied stateとcanonical bindingを確認。actual Vulkan-device host14範囲とy build warning/error0/style0 PASS。GPU mapping ownership/queue launchは未接続。次はowned code/uniform/TMU/fetch preparation、whole-job/session quarantine/CL/queue/runtime、実機とp007。Phase in-progress、foreign commitment/HAL/UAPI変更無し。

## i14 whole native draw preparation checkpoint（2026-10-09）

[詳細](../execution-20261009.md#i14-whole-native-draw-preparationのsoftware出力2026-10-09): FIFOのactual sampled UIF/owned descriptors/code/uniform/default/packed fetch/shader recordsをnative mapping rootへ保持、job-wide256MiBの全page padding予算、firstVertex rebase・float default・late OOM全解放・uncertain保持を実装。actual Vulkan/compiler/MMU source host15とy build warning/error0/style0 PASS。既存pipelineのmissing-component未実装限定を解消し、実pipeline R32→vec2 compile/releaseも確認。native CL/GPU launchは未接続。whole prepared/native job quarantineとsession/queue/runtime、実機とp007は残る。Phase内部、foreign scope/HAL/UAPI変更無し、in-progress保持。

## i14 native BCL/clipper checkpoint（2026-10-09）

[詳細](../execution-20261009.md#i14-native-draw-bclとclipperのsoftware出力2026-10-09): 116byte complete native draw BCL stateをwhole ownerへcopied、4.2 integer fine/coarse/guardband depth/viewport edge、drawable-area-scissor restriction、facing/provoking/interpolation/disabled inherited state/owned shader/rebased drawを実装。zero-input dummy readも追加。fixed XML全byte、800 new arithmetic cases、actual compiler/MMU root15範囲、named y warning/error0/style0 PASS。source/license fixed hash確認。pass list/実GPU launch/runtime/whole job+session quarantine/実機/p007は未達、Phase in-progress。同Phase内部loweringでforeign interface/依存変更無し。


## i14 whole native pass checkpoint（2026-10-09）

Complete BCL/RCL/generic tile listと、independent output/linked draw/9pass storage ownersを追加。fixed XML全7stream、actual compiler/prepared/MMU host16範囲、named y build warning/error0/style0を確認。256supertile上限に合わせ最大64×64tilesは4×4groupへまとめる。prepare/OOM中のtarget mutation無し、false whole retain/true complete teardown、全padded budget/zero draws/late rollbackを確認。CLEARはraw stateコピーだけでexecution前rectangle clear/native launchは後続、whole job/controller quarantine/closing sessionを公開前に接続する。native execution/Keiland/実機の証拠ではない。p006/WS未達を保持。[詳細/訂正/復帰点](../execution-20261009.md#i14-whole-native-pass-clとgpu-ownerのsoftware出力2026-10-09)。

## i14 private native executor checkpoint（2026-10-09）

整数UNORM clear＋whole passのpreflight/exact rectangle clear→existing native CL runner→retirement/output visibilityを接続。1037 independent IEEE colour cases/actual native graph＋explicit runner fixtureのhost17範囲/named y build warning/error0/style0を確認。single-use replay refusal、outside pixels保持、fault前CPU mutation無し、syntheticuncertain全root保持。native runnerはこのhostで明示mock、physical GPU/IRQ/cache/resetの証拠ではない。公開前にwhole prepared/session/controller quarantineを実装する。WS/Phase acceptance保持。[詳細/復帰点](../execution-20261009.md#i14-deferred-clearとnative-pass-executorのsoftware出力2026-10-09)。


## i14 whole native jobとclosed-session retirement checkpoint（2026-10-09）

whole pending primary/current passをpersistent controller quarantineへallocation無しでtransferし、閉じたrenderer/namespaceは最後のtyped ownerまで保持する。actual checked reset→payload→closed session→translation recoveryの順を接続。actual Vulkan host18範囲/actual renderer close・provider reset failure・retained destructor host/named y warning/error0/style0 PASS。runner/recoveryのhostモデルはphysical DMA proofではない。transfer/barrier/submit/public dispatch/common bindingとKeiland/実機/p007は後続、Phase in-progress保持。内部lifetime契約、外国scope/HAL/UAPI変更無し。[詳細/再開点](../execution-20261009.md#i14-whole-pending-native-jobとclosed-rendererのsoftware出力2026-10-09)。


## i14 explicit dependencies/implicit attachment layout checkpoint（2026-10-09）

same-device coherent dependency nodeをwhole pending primaryに保持し、FIFO barrierは全imageのold layout/backing preflight後にvisibilityとnew layoutをpublishする。native passのinitial/final layout lifecycleもnative retirementへ接続。actual public barrier encoder/runtime host19範囲、actual close/reset host、final named y warning/error0/style0 PASS。UNDEFINED discardと末尾mismatchのatomic refusalを区別して確認。native transfer/primary queue submit/public/common binding・physical Keiland・p007は未達、Phase in-progress保持。内部runtime出力、foreign scope/HAL/UAPI変更無し。[詳細/次](../execution-20261009.md#i14-explicit-barrierとimplicit-pass-layoutのsoftware出力2026-10-09)。


## i14 external-sharing dependency admission checkpoint（2026-10-09）

actual WSIのfamily0↔external pairを、exact bound memoryのexternal declarationがある場合に限定してprivate barrierへ接続。typed export/private allocationに基づく数値resource-description fixtureで双方のadmission/refusalを確認し、actual public barrier/whole native graph host19範囲・named y warning/error0/style0 PASS。external provider/GPU実動作とpublic queue/fence publicationは未達。shared Vulkan headerの欠けたcore tokenはprivate標準値で表現、共有source/HAL/UAPI変更無し。native GPU meta transfer/runtime/physical/p007は後続、Phase in-progress保持。[証拠/再開点](../execution-20261009.md#i14-external-family共有メモリのadmission-checkpoint2026-10-09)。


## i14 full-image native GPU clear checkpoint（2026-10-10）

actual public vkCmdClearColorImage→typed immutable primary→prepared pending graph→zero-draw native tile clear/storeを接続。TRANSFER_DST用途・bound same-device・remaining ranges・FIFO current layout、独立output/9storage、OOM/budget rollback、zero-draw quarantine/reset lifetimeを確認。actual encoder/native owner＋explicit runner host20範囲、final named RPi4 y warning/error0/checks3/style0 PASS。CPU target write無し。mock runnerはGPU pixelを書かず、実機clear/Keiland成功は未確認。copy/blit/readback・primary queue submit/fence/semaphore/public/common binding・final runtime stack/p007が残り、Phase in-progress/WS incompleteを維持。Master/shared source/HAL/UAPI変更無し。[証拠/失敗と修正/復帰点](../execution-20261009.md#i14-full-image-gpu-clearのsoftware出力2026-10-10)。


## i14 native image copy/blit checkpoint（2026-10-10）

actual public copy/blit→完全immutable primary/pending graph→内部kernel-compiled texture quad/native passを接続。raw copy、nearest/linear拡縮・両axis反転・RGBA/BGRA conversion、source全sample footprint/physical alias、multi-region FIFO/whole-pass quarantineを確認。temporary数値metaのみで公開仮object無し、CPU destination pixel write無し。actual client/kernel-source＋明示runner host21範囲、final RPi4 y warning/error0/checks3/style0とown SPIR-V validator PASS。実GPU pixelはmockしないため実機/Keiland成功は未確認。buffer readback/transfer・QueueSubmit/fence/semaphore/public/common binding・final stack/p007が残り、Phase in-progress/WS incompleteを維持。Master変更無し。[正確な範囲/codec誤りの修正/確認/復帰点](../execution-20261009.md#i14-native-image-copyblitのsoftware出力2026-10-10)。
