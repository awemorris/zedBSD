<!-- awesome-plan project=zedbsd record=ws141-p004 -->

# ws141-p004: V3D 4.2の電源・MMU・job・回復

Status: in-progress（2026-10-09、V5の純粋なページ表生成だけを開始）
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

## 再開条件

i04のhost/build結果を記録した後、実機V0を確認しV1へ進む。電源domainとresetの実装は既存HAL/FDT/firmwareの責務を照合して進める。HAL APIが必要なら適用前に具体的差分をplanへ置く。

## 結果（2026-10-09、i04）

mmu.cのmap/unmapとprivate宣言・buildへの追加を実装。mmu-host-test.cのliteral PTE/最終VA/PA/衝突・hole/不正span/larger-page拒否がPASS。stage/list/list-copyの既存確認もPASS。rpi4 y/n build exit 0・warning/error 0、全文C reviewとstyle-check total 0。詳細は[実行記録 i03/i04の結果](../execution-20261009.md#i03i04の結果2026-10-09)。

この部分attemptだけをclearedとしwhole Phaseはin-progress。電源・clock・MMUのregister・cache/TLB flush、IRQ、CL、TFU、reset/jobの実機動作は未実施。元のstage順・whole Phase条件は変更していない。[WS](../ws.md)のPhase表/再開点を同時に更新。次は実機V0の観測とV1の電源処理、V5の表確保・寿命管理・hardware設定の統合。
