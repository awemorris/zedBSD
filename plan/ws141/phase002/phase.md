<!-- awesome-plan project=zedbsd record=ws141-p002 -->

# ws141-p002: 定数の一括の改名、段の印の helper、driver の骨格（P0・V0）と QEMU での安全な抜け

Status: in-progress（q695-i01、P2 generation13）
Disposition: normal
Parent: [WS141](../ws.md)
Queue: q695
依存: [p001](../phase001/phase.md)（文書と判断の項目。項目 2〜17 は 2026-10-04 ユーザー「既定案で全部承認」）
実行者: phase-runner（high）

## 範囲（[design](../rpi4-gpu-design.md) §10 の p002）

1. **定数の一括の改名**（code を書く前、commit しない）: `plan/ws141/temp/` の 3 つの作業の文書の register・bit・field・mailbox の tag・DT の binding の定数の名前を、全て一度に独自の名前へ変えた版を `plan/ws141/temp/rename/renamed/` に作り、対応表を `plan/ws141/temp/rename/rename-map.tsv` に置く（道具 `plan/ws141/temp/rename/rename.py`）。zedBSD の code は改名の後の版から書く。Linux の関数・構造体の名前（小文字）は出典を辿るために作業の文書に残す（zedBSD の code に現れない）。
2. **段の印の helper**（`src/drivers/gpu/bcm2711/stage.c`）: 段の始め・終わり・失敗の行を `kern_logf` で出す（1 行は 79 桁までに切る、console は 80 桁 × 25 行）。危ない書き込みの前の待ち（既定 3 秒、p002 では使わない）。boot の parameter: `rpi4gpu.off=1`（attach しない）、`rpi4gpu.stop=<段>`（display の段の前で止める）、`v3d.stop=<段>`（V3D の段の前で止める）。parameter は boot の command line の生の文字列を `kern_boot_handoff("boot.command-line")` で読み、`kern_boot_parameters_token_present` で語ごとに照らす（attach は parameter の解析より前の platform の発見の中で走るため。`src/kern/boot.c` の既知の parameter の表は変えない。未知の parameter として起動の log に 1 行出るだけ）。
3. **骨格**（`src/drivers/gpu/bcm2711/`）:
   - **P0**（display）: DT から HVS・pixelvalve2・pixelvalve4・HDMI0・HDMI1・dvp・l2 の割り込みの束・束ね役の node を探す。disabled の node も使う（判断の項目 3）。reg と割り込みを読み、design 1.1 の表と照らして印に出す。HVS・pixelvalve2・pixelvalve4 を uncached で map する（読み書きはしない）。割り込み（HVS 97・pv2 101・pv4 110 の SPI、INTID は + 32）の handler を `kern_irq_register` で登録し、**unmask しない**（P1 で開ける。firmware が device の割り込みを有効にしていると level の割り込みが止まらないため）。
   - **V0**（V3D）: DT から V3D（`brcm,2711-v3d`）を探し、hub・core0 の reg 2 つ、割り込み 1 つ、clock・電源 domain・reset の参照を読む。hub・core0 を uncached で map する（**V3D の register は読まない**、電源の前に触らない）。割り込み（SPI 74）の handler を登録し、unmask しない。
   - **mailbox の clock**（get の tag だけ、firmware の wiki が出典）: clock の ID 4（core）・5（V3D）・13（M2MC、HDMI の state machine）・14（PIXEL_BVB）の state・今の rate・最大の rate を読み、印に出す。`drv_rpi4_firmware_init` は 2 回目の呼び出しでは何もしないので、driver が先に呼んでよい。
   - display と V3D は別の GPU device（判断の項目 11）だが、p002 では `drv_gpu_register` はしない（ops が無いので登録は p005）。2 つの部分を別の struct と別の段の名前（P・V）で持つ。
4. **QEMU（raspi4b）での安全な抜け**: p002 は hardware の register を読み書きしない（DT・map・handler の登録・mailbox の get だけ）ので、QEMU でも実機でも何も変えない。node が無い・reg が読めない・map が失敗したときは、その部分だけ「無し」で抜けて boot を続ける。QEMU の mailbox が clock の tag にどう答えるかは未観測（印に出る値で分かる）。
5. **build への組み込み**（Q1 の確認が要る共有の file）: `CONFIG_DRIVER_BCM2711_GPU`（menuconfig の `config/drivers/architecture/arm64.drivers`、Makefile の既定は rpi4 で y と `-D` の flag）、`platform/arm64/vmunix.mk` の source、`src/kern/platform/rpi4.c` の attach の呼び出し（PCIe の後）。

範囲の外: hardware の register の読み書き（N0 以降・V1 以降）、`drv_gpu_register`、EDID（`rpi4-firmware.c` の変更は p002 では要らないので触らない）、`include/hal/hal.h`。

## 受け入れ

- 改名の版と対応表が temp にある（commit しない）。対応表で全ての旧名が別の名前に変わっている（同じ名前が残らない、道具が確かめる）。
- arm64（`config/ci/config-rpi4.mk`）の `make vmunix` が exit 0・warning 0。`CONFIG_DRIVER_BCM2711_GPU=n` の build も exit 0・warning 0。
- host の試験（段の印の helper: 79 桁の切り、parameter の照合）が PASS。
- QEMU の回帰（Q1 経由で T1）: raspi4b の `plan/tools/boot-test.sh` で login prompt が出る（driver が boot を壊さない）。PNG に `rpi4gpu: P0 …`・`v3d: V0 …` の行が写れば記録する（判定は login prompt だけ）。
- 実機（ユーザー）: 下の「実機の手順」の画面の写真で、P0・V0 の印が design 1.1 の値と一致する。
- `git grep` で zedBSD の code に Linux の vc4・v3d の定数の名前が無い（`rename-map.tsv` の旧名で照らす）。

## 実機の手順（ユーザー、判断の項目 6）

1. この Phase の image（rpi4 の CI の config、`CONFIG_DRIVER_BCM2711_GPU=y`）を SD に書き、Pi 4 の HDMI0 に monitor をつないで起動する。
2. 画面（firmware の framebuffer、中央の 80 桁 × 25 行の console）に出る次の行を写真に撮る:
   - `rpi4gpu: P0 begin` → `rpi4gpu: P0 hvs fe400000+8000 irq 129 …`（HVS の address・大きさ・INTID）、`pv2`・`pv4`・`hdmi0`・`hdmi1` の行 → `rpi4gpu: P0 ok`。
   - `v3d: V0 begin` → `v3d: V0 hub fec00000+4000 core0 fec04000+4000 irq 106` → `v3d: V0 ok`。
   - `rpi4gpu: clk core …`・`v3d: clk 5 …`・`rpi4gpu: clk hdmi …`（state・今・最大の Hz）。
3. 期待値（design 1.1）: HVS 0xFE400000・32 KiB・INTID 129、pv2 0xFE20A000・INTID 133、pv4 0xFE216000・INTID 142、V3D hub 0xFEC00000・core0 0xFEC04000・INTID 106。画面が消えることは無い（p002 は hardware に書かない）。
4. `rpi4gpu.off=1` を cmdline.txt に足すと上の行が出ないこと（任意）。
5. serial は使わない（画面が消える段だけ、判断の項目 6）。

## 結果（2026-10-04、q695-i01、P2 generation13、途中）

- 改名（temp、commit しない）: `plan/ws141/temp/rename/rename.py` で 630 の定数を独自の名前へ（2026-10-04 の追補で channel の添字が小文字の名前 18 を足した）（block の語の置き換え: V3D の hub → `GXH_`、core → `GXC_`、control list → `GXQ_`、MMU → `GXT_`、HVS → `CMP_`、pixelvalve → `TG_`、HDMI → `HX_`、PHY → `HXP_`、PM → `PWR_`、AXI の bridge → `PWB_`、firmware の tag → `FWM_`、block の無い field・packet → `F_`、単独の mnemonic → 意味の名前か `B_` + 13 文字の回転）。Linux の API の名前（`BUG_ON` など 17）は hardware の定数でないので残し、対応表に `linux-api` と記す。道具は旧名が残らないこと（同じ名前の対応が無いこと）を assert で確かめる。改名の版は `temp/rename/renamed/` の 3 file。
- code（`src/drivers/gpu/bcm2711/`）: `bcm2711-gpu.h`（attach の口）、`bcm2711-private.h`、`attach.c`（FDT・`rpi4gpu.off=1`・mailbox の準備・P0・V0）、`stage.c`（段の印 79 桁・待ち・parameter）、`fdt-util.c`（node・window・GIC の SPI・map・masked の handler の登録）、`clock.c`（clock の get の tag 3 つ）、`display.c`（P0）、`v3d.c`（V0）。hardware の register は読み書きしない。
- 組み込み（Q1 の委任 2026-10-04）: `Makefile`（`CONFIG_DRIVER_BCM2711_GPU ?= rpi4 で y`、`-D` の flag）、`config/drivers/architecture/arm64.drivers`（1 行）、`platform/arm64/vmunix.mk`（source）、`src/kern/platform/rpi4.c`（PCIe の後に attach）。`src/kern/boot.c` は変えない。
- build: rpi4（`config/ci/config-rpi4.mk`、driver y）exit 0・warning 0、rpi4 の driver n exit 0・warning 0、amd64（`config/ci/config-amd64.mk`）exit 0・warning 0（`-DCONFIG_DRIVER_BCM2711_GPU=0`）。`plan/tools/menuconfig-target-host-test.py` PASS。`git diff --check` 0。
- host の試験: `plan/ws141/tests/stage-host-test.sh` PASS（79 桁の切り、off と stop の語の照合、長すぎる段の名前の拒否、待ちの印）。
- GPL の名前の照合: `rename-map.tsv` の旧名 630 で `src/drivers/gpu/bcm2711/` を語単位で grep し 0 件。driver の局所の macro も `V3D_` で始めない（`ENGINE_`）。
- QEMU の回帰: T1-092 PASS（2026-10-04、raspi4b の boot-test で login prompt。P0・V0・clk の行は画面の外に流れて PNG に写らず、値は未取得）。
- 未実施: 実機（ユーザー）。

## 独立Codexセッションの再開確認（2026-10-09）

ユーザーがWS141を担当へ割当。開始tree a05865278のrpi4 kernelをdriver y/nでbuildし、両方exit 0・warning/error 0、stage/list host試験PASS。source修正は無し。詳細は[実行記録](../execution-20261009.md)。ユーザー回答「実機確認は後で行う」により実機条件は未達のまま保持。whole Phaseのclearanceは行っていない。
