<!-- awesome-plan project=zedbsd record=ws141-p003 -->

# ws141-p003: display（N0 → N1 → N2 → P1 → P2 → P3 → P5、P4 は後）

Status: in-progress（N0実装済み、N1の純粋な配置/コピー準備を実装。実機待ち）
Disposition: normal
Parent: [WS141](../ws.md)
Queue: none
依存: [p002](../phase002/phase.md)（骨格・段の印・P0。p002 の QEMU の回帰と実機の P0 の写真が先にあると安全）
実行者: 独立Codexセッション（旧P2 generation13の実装を引き継ぐ）

## 範囲（[design](../rpi4-gpu-design.md) §3.1・§10 の p003）

段ごとに実機の写真で確かめながら進める。各段は `rpi4gpu.stop=<段>` で止められ、危ない書き込みの前に begin の行と 3 秒の待ちを出す（p002 の helper）。register の名前は `plan/ws141/temp/rename/rename-map.tsv` の独自の名前で書く（GPL の名前を写さない）。

1. **N0**（読むだけ）: mailbox の get の tag だけで firmware の framebuffer（物理・pitch・幅・高さ）と clock（core・HDMI の 2 つ）。HDMI の clock が 0 なら HVS より先は読まず「display 無し」で抜ける。0 でなければ HVS の全体の enable、出力の切り替え、channel の enable・幅・高さ・状態・次と今の display list の位置、display list の解読（plane の数・format・位置・大きさ・pointer・pitch・end）、pv2・pv4 の enable と timing を読み、80 桁の複数の行に出す。
2. **N1**（画面を消さない引き継ぎ）: firmware の display list の word をそのまま写した list を、firmware の list と filter の係数を避けた領域に書き、読み返して一致を確かめ、channel の「次の list」に入れる。「今の list」が自分の位置になるのを時限付きで poll。
3. **N2**（N1 の直後）: firmware に display の終了を通知（mailbox、判断の項目 17 で値は事実として使う）。前後で HVS・pv2/4・display list・clock・framebuffer の memory と mailbox の答えを読み比べる。変わったら止まって記録する（設計を見直す条件、判断の項目 16）。
4. **P1**: pixelvalve の vblank と HVS の underrun の割り込み（p002 で登録した masked の handler を本物にして unmask）。1 秒あたりの vblank の数。
5. **P2**: 同期の page flip（driver が持つ 1 GiB より下の連続の 2 buffer、cache の clean、display list の切り替え、vblank で完了）。
6. **P3**: plane の合成（console の plane の上に CPU で埋めた plane、console の領域の外、背景の fill）。core clock の underrun の対処（判断の項目 15）。
7. **P5**: resident display（`drv_gpu_display_ops`）の統合と display の device の登録（`drv_gpu_register`、display の役、companion は V3D の device）。cap の bit は gpu.c の検査に従う（design §4）。

範囲の外: P4（HDMI の mode set、H5〜H10）、EDID（判断の項目 14、`rpi4-firmware.c` の拡張が要るときは最小で、既存の呼び手の挙動を変えない）、V3D（p004）、`include/hal/hal.h`（判断の項目 16 で要るなら差分を plan に置いて止める）。

## 受け入れ

- 各段の build（rpi4、warning 0）と、変えた所の host の試験（display list の組み立て・解読、timing の読み取りの計算）。
- QEMU（Q1 経由で T1）: raspi4b で boot が壊れない（N0 は firmware の revision で emulator と判定し、HVS の register を読まずに抜ける見込み、未観測。QEMU の revision が build の時刻に見える値なら HVS を読んで bus の error になりうるので、この回帰は実機の前に必須）。
- 実機（ユーザー、判断の項目 6）: 段ごとの写真で印と画面（N1・N2 で画面が変わらない、P1 の vblank の数が 60±1、P2 の flip、P3 の重なり）。
- `rename-map.tsv` の旧名で driver の source に一致 0。

## 実機の手順

（p003 の実装のときに書く。p002 と同じく HDMI0 の画面の写真。serial は画面が消える段だけ。）

## 結果（2026-10-04、P2 generation13、途中でラップアップ）

- N0 を実装（hardware には書かない）: `src/drivers/gpu/bcm2711/readout.c`（N0）、`list.c`（display list の解読、host で試験できる純粋な関数）、`firmware.c`（`clock.c` を改名し mailbox の get の汎用の口 `bcm2711_firmware_get` と `bcm2711_clock_hz` を追加）。attach の口に firmware の画面（boot の handoff の物理・大きさ・幅・高さ・pitch）を渡す形に変えた（`drv_bcm2711_gpu_attach(fdt_phys, screen)`、`src/kern/platform/rpi4.c` が handoff から埋める）。
- N0 の順: firmware の revision を mailbox で読み、build の時刻（0x40000000 以上）でなければ emulator として register を読まずに抜ける（QEMU の raspi4b には compositor が無く、その register の読みは bus の error になりうるため。QEMU の binary の未実装の領域の名前に hvs が無いことを strings で確かめた。revision の値は QEMU で未観測）→ mailbox の framebuffer（幅・高さ・depth・order・pitch）と handoff の物理 → HDMI の state machine の clock（13）が 0 なら抜ける → HVS の全体の enable と HDMI0・HDMI1 の channel の選択 → 3 channel の enable・mode・大きさ・次と今の list → firmware の出力先の port（HDMI0 を優先）の今の list の解読（plane ごとに format・order・位置・大きさ・pointer・pitch）→ 最初の plane が framebuffer を 1:1 で指すかの判定 → pv2・pv4 の enable・video・active の大きさ。
- build: rpi4（driver y）exit 0・warning 0、rpi4 の driver n exit 0・warning 0（amd64 は rpi4.c を build しないので影響なし、未再試験）。host の試験: `plan/ws141/tests/stage-host-test.sh` が stage と list の 2 つを流し両方 PASS。改名の旧名 630 で driver に一致 0。`git diff --check` 0。
- **再開点**: (1) Q1 経由で T1 に QEMU の回帰（raspi4b の boot-test、login prompt。N0 は emulator の判定で register を読まずに抜けるはず。PNG に行が写らないので、印を見るなら serial の対話か boot の後の dmesg を SSH で読む道を T1 と相談）。(2) 実機の写真（ユーザー）で P0・N0 の行と期待値を照合（N0 の行は 15 行前後で、25 行の console から P0 の行が流れる。必要なら `rpi4gpu.stop=N1` で止めて写真）。(3) 次の段 N1（firmware の list を写した自前の list、「次の list」の切り替え、今の list の一致の poll）。N1 の前に実機の N0 の結果（firmware の list の位置・word の範囲・plane の数）が要る。
- 未実施: QEMU の回帰（N0 の版）、実機。

## 独立Codexセッションの再開確認（2026-10-09）

ユーザーがWS141を担当へ割当。開始tree a05865278のrpi4 kernelをdriver y/nでbuildし、両方exit 0・warning/error 0、stage/list host試験PASS。source修正は無し。詳細は[実行記録](../execution-20261009.md)。ユーザー回答「実機確認は後で行う」により実機条件は未達のまま保持。whole Phaseのclearanceは行っていない。

## N1の準備処理（2026-10-09、i03）

ユーザーの継続指示により、実機観測と独立な配置計算とraw wordコピーだけを実装。list.cの`bcm2711_list_copy_prepare`はsnapshotと予約範囲から終端込みの連続領域を選び、decoded summaryを再構成せず全wordを保持する。予約範囲には全channelのcurrent/next list、filter、firmware専有範囲を含める責務を呼び手に明記。起動経路での呼び出し・hardware書き込みは無し。

list-copy-host-test.cで順不同/重複予約、filter回避、SRAM枯渇とexact fit、9 planeとscaling/contextの完全一致、snapshot不変、失敗時のimage不変を確認しPASS。rpi4 y/n build warning/error 0、全文C review・補助style-check total 0。詳細は[実行記録 i03/i04の結果](../execution-20261009.md#i03i04の結果2026-10-09)。この部分attemptのみcleared、whole Phaseはin-progress。次は実機N0観測を元にsnapshotと全予約範囲の取得を統合し、再検証後のwrite/readback・次listの切り替え・時限付きpollを進める。p004の独立したsoftware準備も[WS](../ws.md)へ投影済み。N1の実機条件は保持。
