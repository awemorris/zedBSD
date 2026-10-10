<!-- awesome-plan project=zedbsd record=ws193-p006 -->
# WS193 p006: Firmwareも全CPU共通の選択へ

Status: cleared
Disposition: normal
Parent: [WS193](../ws.md)
Queue: [Codex Firmware Queue](../codex-firmware-queue.md), menuconfig-firmware-20261010-i01

## 承認・範囲・設計

2026-10-10 current user: 「arm64でFirmwareにUSB WiFIまで出ないのはおかしいです。Firmwareもアーキテクチャに関係なくすべて選べるようにしてください。」
前のp005のFirmware platform条件保持を、この追加指示で置き換える。前回のBase/Desktop/Packages source fe4300313はmain統合済み。Firmwareも同じ共通registryへ加え、メニュー・save/load・Make実効選択とpackaging入力で落とさない。
ユーザーがmainへの統合を指示したmenuconfig共通選択の追加訂正。pushは承認なし。有限範囲は登録policy・既存fixture/readme追従・対象host確認・全文規則reviewと記録まで。download/image/kernel/driverの変更・実機試験は含めない。

## 適用規則と依存

AGENTS.md/Guardrailのownership、外部firmware独立取得/cache/manifest/license/default-off境界を適用。これは選択方針の拡張で、driver capabilityやlicenseの変更ではない。C編集なし、Python/Makeは既存規約を保持。Tests/X11の直接configのみの既存条件は維持。
新しい選択規則のauthorityは上のuser原文。root registry、README、既存host fixtureへ反映し、Guardrail/shared Master/Queue/automation投影・GitHub公開はQ1に保留（担当境界のためここへdurable記録）。
前提: p005 source fe4300313が実際にmainにある。依存は取得/操作のsource変更ではなくregistry出力。

## 完了条件・検証の有限枠

全6platformでFirmware全登録（現在4件）が共通に表示・選択・保存される。実Firmware handlerをrpi4で操作し、USB WiFiのRTL8822Bを含む4件を選べる。Makeの実効選択、rootfs配置/取得prerequisite/manifest入力に4件が残る。default-offを維持し、既存check_packages()の更新部分のみ確認。Python構文、make parse/config validation、diff-check、適用全文規則manual review。dataのみの選択metadata変更でcompile対象なし、firmware取得/実機upload/bootは不要。

## 結果 / menuconfig-firmware-20261010-i01

cleared。全6platformの表示・4件選択・save/load・Make実効選択/packaging入力/config validation PASS。RPi4 real handlerとPTYでRTL8822Bを含む全4件表示・選択を確認。default-off維持、既存fixtureの対象check・Python構文・diff-check・適用規則全文review PASS。[証拠/範囲/未実施](../tests/firmware-selection-20261010.md)。mainへの追加訂正としてsource統合/read-backを行う。push無し。共有Master/Queue/Guardrail/automation/GitHub投影はQ1へ保留。次Queueを開始しない。
