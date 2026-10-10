# Driversメニュー / 2026-10-10開始、2026-10-11結果

Source base: main c2b97e953。tools/menuconfig.py、config/drivers/menu.listと既存menuconfig-target-host-test.pyを変更。対象はUI/既存configの選択・保存、driver/HAL/sourceの移植は含めない。

## 調査と設計

USB CDC ECMには `CONFIG_DRIVER_USB_CDC_ECM` boolがconfig/drivers/usb.driversにあり、amd64/pcat vmunix.mkとplatform/pcat.cでソース選択/登録を制御する。現在のarm64 vmunix.mkのUSB source一覧にはECMが無い。UIの共通選択とCPUへのsource移植を混同しない。固定組み込み（SDHCI、console等の'-|fixed'）はON/OFF変数が無いのでtoggleを捏造しない。

DriversはBoot Optionの後、Developmentの前。既存option filesを読むdriver_rows()が全CPUのCONFIG_DRIVER_* boolを一つの一覧へまとめ、重複keyを除く。現在23件。normalize()のplatform非対応driverをnへ戻す処理を撤去。save・CPU切替で指定値を保持する。driver_defaults(platform)は既存platform metadataを初期/未指定値だけに使用。load時はconfigが指定したkeyを保持し、未指定keyにtargetの既定値を補う。既存初期既定値との一致を検証した。

## 初期検証（flat UI、階層化の追加指定前）

Linux host、Python 3.13.5、GNU Make 4.4.1。独立worktree codex/menuconfig-drivers。

```
PYTHONDONTWRITEBYTECODE=1 python3 build/menuconfig-drivers/verify.py
TERM=xterm-256color make menuconfig ZEDBSD_CONFIG=build/menuconfig-drivers/selected-rpi4.mk
```

private probeは開発用、commitしない。実registry・handler・save/load、既存fixture check_packages()/check_menu_layout()だけを使用。scratchはown build/tmpへ残す。

- 全6platformで同じ23bool、全件toggle/save/load、各保存configのMake validate-image-config: PASS。
- 最小configの未指定driver defaultを変更前のload+normalizeと23key全件/全6platformで比較: PASS。
- real CPU / Board handler、amd64→rpi4→amd64で23keyのy/n混在選択を保持: PASS。
- Make実効CONFIG_DRIVER_USB_CDC_ECMはamd64/i386/rpi4全てy/nを保持。amd64/i386はsource/obj一覧がONのときだけECMを含む: PASS。rpi4一覧にECMが無い既存の実装状態も確認。
- real PTYで指定menu位置/Drivers/rpi4の一覧とUSB CDC ECMを確認。ECMをOFFへtoggle、保存fileでnを確認: PASS。
- existing targeted fixtures、Python compile構文、git diff --check: PASS。

## 規則全文review・未実施

AGENTS.md/Guardrailの全適用規則、既存Pythonの規約、option ownership/正本の一致、重複key/固定driver/初期値/保存/CPU切替を変更全文でmanual review。新C無し。外部source・toolchain・共有build変更無し。root/config formatは既存互換。既存回帰fixtureの階層期待だけ更新、新共有test追加なし。規約違反残件なし。

Pythonのみの編集でnative compile対象なし。kernel/image/QEMU/実機の確認、arm64 ECMのsource移植・通信確認は未実施。main統合は具体的成果commitの承認待ち。push無し。shared Master/Queue/Guardrail/GitHub投影はQ1へ保留。

## 階層化後の最終検証（2026-10-11）

ユーザー追加指定「Disk, Input, GPU, Audio, Ethernet, WiFi, USBみたいに階層化してほしいです。」を適用。config/drivers/menu.listは分類とkeyの対応だけを持つ。driver_rows()の共通正本から分類するため、CPUによる除外はない。ACPIと今後の未分類boolはPlatformに置く。

| 分類 | 設定数 | 主な設定 |
| --- | --- | --- |
| Disk | 2 | NVMe、USB storage |
| Input | 2 | USB HID、LPSS I2C |
| GPU | 4 | Framebuffer、Venus、i915、BCM2711 |
| Audio | 1 | HDA |
| Ethernet | 4 | NE2000、USB CDC NCM/ECM、LGY-98 |
| WiFi | 2 | AX211、RTL8822BU |
| USB | 7 | UHCI/EHCI/XHCI、hub、CCID、Bluetooth、Type-C |
| Platform | 1 | ACPI |

同じprivate probeを階層に追従し、実drivers_menu→各driver_optionsへ入る操作で全設定を切替。
- 全6platformで同じ8分類/23設定、重複と欠落なし、全件toggle/save/load/Make validation PASS。
- 旧default全23key・全6platform一致、real CPU切替で選択保持、ECMのMake実効値/既存x86 source分岐を再確認PASS。
- real PTY: Raspberry Pi 4 Arm64 header、Boot Option→Drivers→Developmentの位置、Driversの8分類、Ethernetで4設定表示、ECM ON→OFF、保存fileにCONFIG_DRIVER_USB_CDC_ECM := nを確認PASS。
- 対象既存fixture、2 Python sourceのcompile、git diff --check PASS。分類追加を含む最終差分全体をAGENTS/Guardrail・既存Python規約に照らしてmanual review PASS。

native build対象なし。driver移植・image・QEMU・実機試験はscope外で未実施。main統合は具体的commitの承認待ち。
