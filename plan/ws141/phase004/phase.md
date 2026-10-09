<!-- awesome-plan project=zedbsd record=ws141-p004 -->

# ws141-p004: V3D 4.2の電源・MMU・job・回復

Status: in-progress（2026-10-09、V5のsoftwareページ表とV7のnoop CL生成済み。hardware待ち）
Disposition: normal
Parent: [WS141](../ws.md)
Queue: [独立セッションの実行記録 i04](../execution-20261009.md)
実行者: 独立Codexセッション。Q1はパッチの統合・共有記録の投影を担当。

## 範囲と目標

[承認済みdesign §3.3](../rpi4-gpu-design.md#33-v3dp004)のV1〜V10を成立させる。V0はp002。V1の電源・clock/reset、V2の最大rate確認、V3の識別、V4のcache範囲/IRQ mask、V5のMMU、V6のIRQ、V7のnoop bin/render、V8のclear/store、V9のTFU、V10のtimeout/reset後のV7再実行がwhole Phaseの範囲。CSDとshader/drawing実行器はp006の判断後。

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
