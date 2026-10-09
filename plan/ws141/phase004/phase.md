<!-- awesome-plan project=zedbsd record=ws141-p004 -->

# ws141-p004: V3D 4.2の電源・MMU・job・回復

Status: in-progress（2026-10-09、i12でnative V1〜V10/job/cache/MMU/IRQを接続しhost/build確認。実機whole acceptanceは未確認）
Disposition: normal
Parent: [WS141](../ws.md)
Queue: [独立セッションの実行記録 i04](../execution-20261009.md)
実行者: 独立Codexセッション。Q1はパッチの統合・共有記録の投影を担当。

現行software attempt: [i12](../execution-20261009.md#i12-native-v3dv1v10のsoftware結果2026-10-09)。main統合はユーザーが本担当へ承認済み、共有記録/GitHub/T1の投影はQ1。

## 範囲と目標

[承認済みdesign §3.3](../rpi4-gpu-design.md#33-v3dp004)のV1〜V10を成立させる。V0はp002。V1の電源・clock/reset、V2の最大rate確認、V3の識別、V4のcache範囲/IRQ mask、V5のMMU、V6のIRQ、V7のnoop bin/render、V8のclear/store、V9のTFU、V10のtimeout/reset後のV7再実行がwhole Phaseの範囲。CSDとshader/drawing実行器はp006へ接続する（2026-10-09ユーザーが本WS内実装を確定）。

## 依存と今回の部分範囲

- Whole Phase: p002のV0実機確認とp001の設計・license判断。Q1のclearance・実機観測は未達で保持する。
- i04: 現行のV0骨格がbuild可能なことと、回収済み改名資料のPTE形式だけを依存出力として使う。GPU registerに一切触れず、p003のN1の実機確認とも独立。
- 4 KiB PTEの生成と解除を`src/drivers/gpu/bcm2711/mmu.c`へ追加。VAは4 GiB、物理PFNは24 bit、PTEはVALID bit28/WRITE bit29。page 0は予約し、範囲・alignment・既存mappingを全て確認してから更新する。
- 呼び手は4 MiBの表の確保・寿命・排他、jobが参照しない時刻、cache clean/barrierとTLB flushを担当する。このattemptではその呼び手やMMIO設定を起動経路に追加しない。
- 当該driverのbuild source列へmmu.cだけを追加。HAL API・GPU公開APIの変更なし。

## 規則・参照・検証

- `plan/coding-style.md`全文とGuardrailのHAL/GPL/scanout/buildの規則。clang-format-19、style-check、全文の目視review、短いhost試験、rpi4のdriver y/n build（warning/error 0）。集約make check/QEMU起動/toolchain変更は行わない。
- 形式の出典: 回収済み`temp/rename/renamed/v3d-init-and-submit.md`のV5と監査済み固定Linux source。値はhardwareの事実として使い、名称・配置・処理・commentは独立して記述する。GPL原文・対応表はignored tempだけに置く。最終license/類似監査はp007。
- Whole Phaseの受け入れ: V1〜V10の段ごとの実機の印と値、V7 bin→render完了、V8の出力、V9のTFU、V10の回復後V7。QEMUはQ1経由T1の起動回帰だけで、V3D実機を代替しない。
- 今回の試験はページ表のword列・境界・既存mappingの保持を確認する。hardware MMU/cache/TLBが動く証拠にはしない。

## i06: V7のsoftware生成

統合済みの状態からユーザーの継続指示で実行。1×1のBCL・RCL・generic tile sub-listを独立したbyte serializerで組み、アドレスをlittle-endianで入れる。caller-owned bufferとmapped tile poolが入力で、容量不足・GPU VAのoverflow・使用領域の重なりを拒否し、完全な生成後にused lengthを公開する。

出典は監査済み固定MesaのMIT noop/prologのpacket使用とXMLの4.2形式。XML自体の既定MIT扱いは既存のユーザー決定を保持。hostは4.2のXMLのfield/default/minus-oneから独立に作る期待列と照合し、拒否時のbuffer保持も確認。GPL旧名照合、全文C/format/style-check、rpi4 build warning 0を行う。GPUへの実投入、MMU/cache/IRQの統合と実機観測はこの部分attemptには含めず、whole Phaseの段順/受け入れは変更しない。

## 再開条件

i04のhost/build結果を記録した後、実機V0を確認しV1へ進む。電源domainとresetの実装は既存HAL/FDT/firmwareの責務を照合して進める。HAL APIが必要なら適用前に具体的差分をplanへ置く。

## 結果（2026-10-09、i04）

mmu.cのmap/unmapとprivate宣言・buildへの追加を実装。mmu-host-test.cのliteral PTE/最終VA/PA/衝突・hole/不正span/larger-page拒否がPASS。stage/list/list-copyの既存確認もPASS。rpi4 y/n build exit 0・warning/error 0、全文C reviewとstyle-check total 0。詳細は[実行記録 i03/i04の結果](../execution-20261009.md#i03i04の結果2026-10-09)。

この部分attemptだけをclearedとしwhole Phaseはin-progress。電源・clock・MMUのregister・cache/TLB flush、IRQ、CL、TFU、reset/jobの実機動作は未実施。元のstage順・whole Phase条件は変更していない。[WS](../ws.md)のPhase表/再開点を同時に更新。次は実機V0の観測とV1の電源処理、V5の表確保・寿命管理・hardware設定の統合。

## 結果（2026-10-09、i06）

V7のBCL/RCL/generic tile sub-listをcl.cで生成する処理を実装。caller-owned bufferとpoolの全予約範囲を検証し、全検査後に14/56/19 byteを生成・公開する。CPU storageとGPU mapping/cache/job投入は後続の所有者の責務として明記。NONE store、shader無しで起動から呼ばない。

noop-host-testは成功・容量/VA/予約等の拒否とbyte保持を確認しPASS。固定XMLから独立にopcode/field/default/minus-oneを解釈するnoop-packet-checkで3列全体が一致しPASS。既存4試験PASS、rpi4 y/n build warning/error 0、全文C review/format/補助style-check/構文/diff確認済み。詳細は[実行記録 i06](../execution-20261009.md#i06の結果2026-10-09)。このsoftware部分だけcleared、whole Phaseはin-progress。V7がGPU上で完了したとの主張はしない。WSのPhase表/再開点を同時に更新。V8生成を次のsoftware段とし、実投入は元のV0〜V6の確認/実装後。

V7生成は最新mainへmerge `16024f1b9`で統合済み（i07）。統合版でもhost/oracleとy/n build PASS。次のV8の固定MIT手順には、clear値を設定した後に2回のdummy tileを通し、最初のtileでCLEAR、最後にVCD cache flushする初期化が含まれる。noopのNONE storeをcolor storeに替えるだけではその条件を供給できない。実装時はこの初期化もpacket oracleと照合し、hardwareのclear/store完了は実機のbuffer観測で確認する。


## p006の実装範囲確定に伴う依存（2026-10-09）

ユーザー回答「WS141に実行器・compilerも含め、Keiland表示まで進める」でp006を本WS内の実装Phaseへ変更。p004はnative bin/render/TFU/CSDのjob/cache/MMU/reset出力をp006へ渡す。p004側のV1〜V10の受け入れは保持し、CSD/shaderの命令生成・Vulkan object/stream実行は[p006](../phase006/phase.md)。実機は後日、software実装と実機whole acceptanceを区別する。[起点/全体変更](../ws.md#完成までの自走p006の実装範囲確定2026-10-09)。


## i12のnative電源処理（2026-10-09、継続中）

p006の本WS内実装が承認済み、i12をin-progressへ。固定firmware treeのnative PM domain1/reset0/firmware clock5を検証し、V1/V2とnative resetの部品を実装/boot接続。actual FDT/provider codeの短いhost PASS、kernel build warning/error0。ASB停止timeoutはreset未達を保持し、READY falseでengine readを拒否する。V3〜V10/MMU/cache/IRQ/jobは継続、実機未確認とwhole Phase in-progressを保持。[source/commands/制限](../execution-20261009.md#i11のmain統合とi12の再開2026-10-09)。


## i12のnative job/診断と後続interface（2026-10-09）

V1〜V10をbootへ接続し、cache/MMU/IRQ/retained storageとtrusted bin/render/TFU/CSD runnerを実装。clearの全packetを固定XMLと照合し、hostはactual sourceでtimeout/両bank IRQ/同時fault/overflow/forced-reset/出力sentinelを確認、rpi4 y/n warning/error0。物理GPUの受け入れとは扱わずwhole Phaseはin-progress。[exact evidence](../execution-20261009.md#i12-native-v3dv1v10のsoftware結果2026-10-09)。

p005へ渡す契約はsingle worker、callerによる全buffer/VA保持、失敗launched jobのretired=false、公開clientがある場合のresetはcommon recoveryによる全owner処理後だけ。p006はこのtrusted job型でCL/TFU/CSDをlowerし、compiler output/indirect span/inputsを保持する。新public HAL/GPU API無し。foreign Phaseへ同契約を記録、ソフトウェアの統合後にi13を開始する。

## 最終software監査（2026-10-10）

native電源/MMU/IRQ/job/resetの最終sourceを[p007 audit](../p007-software-audit.md)で確認。対応hostとnamed rpi4 y/n build PASS、code修正と制限は[実行記録i15](../execution-20261009.md#i15-最終software監査と検証2026-10-10)。software結果で実機のwhole acceptanceをclearせず、Statusはin-progressを保持する。Master/共有投影はQ1。
