# FirmwareのCPU共通選択 / 2026-10-10

Base: main fe4300313。変更sourceはroot Makefileの共通registry、既存menuconfig-target-host-testの同じpolicy確認、firmware README。共通groupへfirmwareを加える。firmware bytes/source/download/cache/default-off/driver有効化は変更しない。

## 承認と規則

current user「Firmwareもアーキテクチャに関係なくすべて選べるようにしてください。」。p005で保持していたFirmware条件を追加訂正で置き換える。選択方針はCPUを問わず全4件。AGENTS.md/Guardrail適用規則全文を変更scopeでmanual review、Python/Makeは既存style。C編集なし、toolchain/共有build編集なし、WIP、push無し。Guardrail/shared計画/automationの投影はQ1へ保留。外部bytes/ライセンスの新規変更・取得無し。

## コマンドと結果

独立worktree codex/menuconfig-firmware。Python 3.13.5、GNU Make 4.4.1。private一時probe（build/menuconfig-firmware/verify.py）から既存fixtureのcheck_packages()のみを呼び、real menu registry/handler/save/loadとMakeを使用。再利用しない開発scriptはcommitしない。

```
PYTHONDONTWRITEBYTECODE=1 python3 build/menuconfig-firmware/verify.py
TERM=xterm-256color make menuconfig ZEDBSD_CONFIG=build/menuconfig-firmware/rpi4.mk
```

- i386/amd64/pc98/rpi4/sun4u/x68kでFirmware全4件の表示・選択・save/load: PASS。
- 全6platformでMakeのZEDBSD_USER_PROGRAMS実効値、ZEDBSD_USERLAND_DATA_INPUTS/FILESに4件と各manifestが残る: PASS。RTL8822Bは `/lib/firmware/rtw88/rtw8822b_fw.bin` 配置対象に残る。
- 各保存configで `make --no-print-directory -s ZEDBSD_CONFIG=<file> validate-image-config`: PASS。
- rpi4のreal edit_program_groupで4件を選択、全部 `[*]` になる: PASS。
- real PTYでtop→Firmware、Current target: Raspberry Pi 4 Arm64と `[*] Intel i915 display DMC firmware`、`[*] Intel AX211 firmware`、`[*] Intel Bluetooth firmware`、`[*] Realtek RTL8822B firmware`を確認して終了/保存: PASS。
- Firmwareの全4件がdefault-offのまま: PASS。
- 既存fixture check_packages()・Python compile構文・git diff --check: PASS。

metadataのみの変更でCPU codeのcompile対象無し。全fixture/集約make check/image生成/firmwareのnetwork取得/実機への書込/boot/CIは実施していない。起動に必須のRPi4 start4.elf等の扱いは本件の変更scopeではない。
