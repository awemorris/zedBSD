<!-- awesome-plan project=zedbsd record=ws300p004 -->

# ws300-p004: VMD の下の link の ASPM L1・L1 substates・LTR

Phase ID: `ws300-p004`
Parent: [WS300](../ws.md)
Status: planned（2026-11-15 P5 q2114-i01: ユーザーの判断 Q300-6（`_OSC` を見ずに ASPM を設定してよいか）の後に main の上で作り直す。今回は手を付けない、Q1 の指示）
Phase disposition: normal
Queue: —（第 2 版の実装は q1683-i01（P6）の branch `agent/p6-ws300`、merge されず）

## 範囲

[design.md](../design.md) §4.3。VMD の attach の scan の後に、port ごとに link の両端（root port と slot 0 function 0 の endpoint）を設定。既定では何もしない。`pci.vmd.aspm=on` の時だけ、
FADT の PCIe ASPM Controls が立てば on でも何もしない（review R1・R2）。restore の順（LnkCtl の ASPM を L1SS の後に）は [BUG-424](../../bugs/BUG-424.md)（debec3781b）で main に入った。

## 作り直す時の形（第 3 版）

- 計算（ASPM L1 の可否、LTR、L1SS の enable・CMRT・T_POWER_ON・閾値）は新しい .c でなく header の inline（案 `include/drivers/pci/pci-link-plan.h`）。新しい .c は mk の変更（ユーザーの確認）が要るため。
- 書きは `pci-vmd.c`、FADT の bit の読みは `src/drivers/acpi/acpi-kern.c`（既存の file）。host 試験は `plan/ws300/tests/host/link-plan-test.c`（第 2 版の物を header の API に合わせて移す）。
- `pci-power.c` の save・restore は BUG-424 で変わったので、両端の値が S0 idle の往復で戻るかを読み直してから書く。

## 第 2 版の記録（2026-10-29 P6、branch の上、merge されず）

- `pci-link-plan.c`・`.h`（計算）、`pci-vmd.c` の `vmd_link_power`（下流から ASPM を切る → LTR を上流から → L1SS の enable を下流から 0 → CTL2・CTL1 → enable を上流から → ASPM L1 を上流から、
  log 1 行）、`drv_acpi_pcie_aspm_forbidden()`（FADT の IAPC_BOOT_ARCH offset 109 の bit 4）、`pci-power.c` の restore の順。
- 確かめ（branch の上、2026-10-29）: link-plan-test PASS、config-5330・pcat・amd64・rpi4・rpi5・riscv64-virt・visionfive2・m68k-virt の vmunix が exit 0・warning 0（当時の build）。
