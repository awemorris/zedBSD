<!-- awesome-plan project=zedbsd record=ws141-final-software-audit -->

# WS141 最終software監査（2026-10-10）

対象: [承認済みi11〜i15](execution-20261009.md#完成までの継続承認2026-10-09)、[p007](phase007/phase.md)。担当Codexの独立worktreeで実施。Master/共有Queue/同期cacheは更新しない。実機確認はユーザー担当で後日という承認を保持する。

## Sourceと規約

全driver 129 file（C/header/own SPIR-V assembly/生成word列）、WS141 host/checker、限定承認されたmailbox3 path・Normal NC4 path・arm64 source列を確認した。正確な対象と最終hashは[台帳](final-source-sha256.tsv)。新codeはZlib、既存共有Zlib frontend/codecはread-only再利用。Gen12 backend、HAL API、user UAPIを変更していない。

C全文[規約](../coding-style.md)と[Guardrail](../guardrail.md)を適用。簡約版・C例外は無し。全sourceのpublic/static順・public目的comment・ANSI declaration・条件内call/Boolean生成・macro・loop/switch・paragraph・critical section・counter/flagの意味・allocation/publication/unwind・最後の結果return・機能用診断とtest専用switchの区別を確認した。各実装checkpointの全文function reviewと、最終状態の全scope走査・下記修正を合わせた確認であり、formatterだけの判定ではない。

- 大きなpipeline/descriptor/primaryの一時tableを既存256KiB command arenaへ移動。公開ownerはarenaを借用しない。count上限・全batch検査・partial publicationの巻戻しを維持。empty primary freeはallocation無しで有効。
- compiler状態とpipeline/meta keyをCPU所有へ移動。追加heap failureを全unwind。fragment varyingの予備parseにも、native compileと同じgraphics stage/定数深度preflightを適用。shared parserは変更しない。
- 177の複合条件の改行、76の末尾completion/refusal return、host stageの条件内call2箇所、不要なUNUSED_PARAMETER macroを修正。条件の順・括弧・short circuitは不変。permanent workerの無限loopに停止成功を宣言しない。
- clang-format19.1.7を変更function範囲へ適用し、全文規約のdefinition tabs/条件ごとの改行を復元。既存の無関係なtable/関数をformatしない。
- `python3 plan/tools/style-check.py src/drivers/gpu/bcm2711/*.c src/drivers/gpu/bcm2711/*.h plan/ws141/tests/*.c --summary` はconditional14件のみ。display3/firmware1/readout7/v3d3はいずれも同種の短い文字列2択で、§6で許可された対称choice。条件内call等の未解決指摘は0。`git diff --check` PASS。Python syntax、shell parseもPASS。

## 16KiB stack

[再現checker](tests/stack-audit.py)はfinal LTOの全命令でSP減算/preindexを集計する。遅いprologueを見落とした旧checkpointは[訂正](execution-20261009.md#i14-public-runtimeのstack修正とgraphics-admission2026-10-10)を保持。単一local frameやGCされたprivate binaryを全経路の証明にしない。

| Ordinary configured runtime | callerとkernel IRQを含む保守的見積り |
| --- | ---: |
| single worker | 12,960 byte |
| 同期COMMAND ioctl | 15,984 byte |
| 非同期SUBMIT ioctl | 12,432 byte |
| ARM64 task stack | 16,384 byte |

caller: kernel_thread_trampoline16 byte、同期側arm64_sync_lower288/arm64_sync_handler144/kernel_syscall_handler2336/file_ioctl96/cdev_ioctl_file0/gpu_ioctl176/gpu_command_ioctl96。driver frameの上に同時に残る分だけを足す。file close/user returnや他syscallの別branchは深いruntimeから戻った後であり、同時stackではない。

IRQ: SAVE_FRAME288 byteを含むarm64_irq_currentの最大5,344 byte。今回のrpi4 configではarchitectural timerとprivate BCM sourcesが対象。private FDT handlerはsource/timing/compositorへ固定登録。EL1で既に実行中のdriverへのIRQはEL0 user-return branchへ入らない。exceptionのDAIF、schedulerのirqsave/restoreとcontext switchを照合し、一つのtask stackへ同時に載るIRQは一つ。別taskへ切り替えた間のstackを同じtaskへ加算しない。

20 dispatch、worker executor/disposer、13 typed destructor、4 recording release、2 layout release、draw prepare、GPU completionのfence finalizerをsourceから固定。LLVMのBRは実命令のADR+read-only table+index+ADDとsource switchを照合した同一function内のjump。heap observerはこの非trace buildでNULL（setterはentry.cのKERN_KERNEL_HEAP_TRACE内のみ）。未分類indirect/未知SP/無限callgraph cycleを0扱いせずcheckerで拒否する。

再帰はexternal stream5 active frames、constant9、type10（depth拒否frame込み）、matrix determinant4、typed owner DAG8+null leaf。SIGCHLDの親通知はCONT/KILL処理へ再帰しないためsignal2 frame。Workgroupのcompute-only再帰は全private shared-parse入口のgraphics preflightで拒否する。typed ownerの不可能な組合せとkernel meta shaderに任意user定数を重ねた過大評価も残す。

最小余裕400 byteは、このcompiler/config/sourceに対するordinary runtime bound。terminal kernel invariant failureのhal_fatal診断、別config/observer/callbackの追加、一般の全kernel syscall、実機stack使用量を証明するものではない。入力/設定が変われば再監査する。

## license・字面・設計・BLOB

固定一次source: Linux6.19 `05f7e89ab9731565d8a62e3b5d1ec206485eeb0b`、Mesa25.3.6 `06f9e28304d5d3f109c33535c1c25b9df5769af2`。path/license/hashは[既存台帳](rpi4-gpu-license-audit.md)。GPL作業文書/改名表/oracle sourceはignored tempのみでcommitしない。

- 台帳144行中135を通常cache pathで、6をfetch alias/固定DTB pathでSHA一致確認（重複行を含む）。残る旧overlay2件と旧wiki snapshotは今回cacheに無く再取得せず、過去のhash/出典記録を維持。現実装の入力DTB、register/packet/QPU/MIT shader ABI sourceは照合済み。これら3件の新規code取り込みは無い。
- GPL cache C/header52 fileと全own C/header/word列を、comment除外のliteral24 tokenとidentifier正規化48 tokenで比較。初回literal1 pair/14windowはHVS filter係数表。修正後literal0。正規化77 pair/2773windowはinclude列、case列、同型assignment/decoder dispatch/複数memcpyというCの共通構文。全候補をcategoryとsource目的で確認し、具体的なGPL演算/構造/名前/commentの取り込みとして残す候補は無し。有限window比較は短い一致や全設計の自動証明ではないため、下記構成も確認した。
- 630改名表はhardware定数旧名の残存0。NULL/EINVALなどkept/API項目はhardware名称の一致と扱わない。各own sourceのZlib headerを確認。
- LinuxのDRM/atomic/BO/reservation/dma_fence/workqueue/power-runtime構造を移植せず、zedBSDのFDT role、bounded prepared display operation列、permanent copy scanout、private IRQ latch、one native job slot、typed Vulkan registry/command arena/immutable recording/owner DAG/single worker/checked recoveryに組み替えている。Linuxと同じhardware前後関係は保持するが、Linuxのsoftware object/control構造を取り込まない。
- native packet/texture/QPU形式は固定MIT Mesa XML/sourceのinterface事実。scalar frontend/codecは既存Zlib、backendは独立の単一ALU/liveness/semantic uniform/有限profile実装。MIT oracleはhostで独立照合するだけでkernelへlinkしない。
- filterは設計で承認された独自計算へ修正。[Mitchell–Netravali原論文](https://www.cs.utexas.edu/~fussell/courses/cs384g-fall2013/lectures/mitchell/Mitchell.pdf)式8にB=C=1/3を代入し、x=(31-2i)/16・Q8最近丸め・対称位相の最大weight補正から独自生成。全8位相和256/9bit/対称性を[有理数checker](tests/filter-coefficients-check.py)で確認、actual display generatorの11 SRAM wordもliteral照合。kernel FP無し。現scanoutはunscaledでfilterを使用しない。
- arrayは自作CL packet image、field/sample/corner table、own SPIR-V assemblyからSPIRV-Toolsで生成したmeta shader word列。own assembly→runtime compilerで独立native programを生成する。firmware machine-code BLOBの埋め込みは無く、userland/firmwareへの移動対象無し。VideoCore boot firmwareは既存外部boot配布物、driverはmailboxを使う。

## actual clientと二node

既存libvulkanのwsi-display-nodes/context/wsi-image/memoryと照合。display-only nodeをVkPhysicalDeviceにしない。companion0は実在rendererの既定pairへ解決。XML1.3.269はcopy_display=false、既存private DMABUF hint0x200/placed allocationを使用。公開OPAQUE FD pNextは別に許可された形式で、二つを混同しない。

BGRA/RGBA・stride/offset4整列・contiguous/低1GiBのdisplay制約に、rendererの実64byte pitch/Normal NC backing/immutable image identity/exportとdisplay foreign import/copy presentを合わせた。present後のsource destroy、closeのconsole復帰、timeout時の永久copy/quarantine ownerを確認。memory/kernel/user aliasは同じNormal NC、shared4 pathの既存cached/MMIO defaultは維持。

actual Keiland shader/graphics writer、QueueSubmit/sync/primary/descriptorのproduction sourceを使うhostと、actual register/open/二namespace/shared-owner fixtureを確認した。fixtureが代替するのはscheduler/MMIO/common observer/native retirementであり、kernel syscall・実GPU・Keiland画面を動かした証拠ではない。

## 最終確認と受け渡し

LLVM23.1.0 read-only、clang-format19.1.7、GCC14.2.0。named rpi4 y/n vmunixはwarning/error0・各ARM64 checker3 PASS。host全14群PASS、runtime26scope、display4、hardware2、stage4、独立Mesa packet/QPU decodeとscalar/viewport/colour結果を保持。全commands/log/hashとmain統合は[実行記録](execution-20261009.md#i15-最終software監査と検証2026-10-10)に記載。

software実装/監査/統合後もWS incomplete、p002〜p006のwhole acceptanceは実機待ち。RPi4のP0/N0/R0/P1/P2/P3/P5とV1〜V10、original HDMI復帰/console RAM寿命、native描画/flip/cache/IRQ/reset、Keiland画面と操作は未確認。Q1/T1の回帰・共有projection/GitHub公開は担当境界のまま。実機結果に応じ同WSを再開し、未確認をsoftware完了で置き換えない。
