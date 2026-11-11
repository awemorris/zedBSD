<!-- awesome-plan project=zedbsd record=ws347-p001 -->

# ws347-p001 KC705 への bitstream の書き込みと PCIe の見え方

Status: in-progress

- 親: [WS347](../ws.md)。状態: in-progress（q2009、Q1 が直に）。

## 範囲

1. Vivado の hw_manager で KC705 を JTAG から見る（書き込まない）。
2. ユーザーが示した bitstream（minimal・200 MHz）の path・SHA-256・作った時の WNS を記録し、`program_hw_devices` で書く。DONE=1 を確かめる。
3. VF2 を起動し直し（JTAG の書き込みは揮発なので KC705 の電源は保つ）、zedBSD の PCI の走査で Amaterrace GPU の function が見えるか（vendor・device ID・BAR）。
4. 手順を 1 本の script（`plan/ws347/tests/kc705-program.sh`）にまとめる。

## 受け入れ

- 書き込みの後 DONE=1、VF2 の zedBSD で GPU の PCI の function が見える（見えない時は原因の切り分けを記録）。

## 記録

- 2026-11-11 11:5x Q1: hw_server に Digilent の target `210203368704A`、device `xc7k325t_0`（IDCODE 43651093）が見え、DONE=0。
- 2026-11-11 Q1: host で見つかった bitstream は `~/amaterrace-gpu/build/vivado/atg_kc705_top/atg_kc705_top.bit`（2026-11-07 11:44）だけ。
  その build の log では WNS −0.911 ns（タイミング未達）で、README の 200 MHz の結果（WNS +0.019 ns）とは別の物。所在をユーザーに確かめ中。
- 2026-11-11 Q1: ユーザーから、bitstream を作ってみる、minimal ならタイミングは閉じているはず、という趣旨の指示を受けた。電源は KC705 がプラグミニ 2 `14C19FA80F86`、VF2 がプラグミニ 3 `AC276E432AE6`（ユーザー）。
- 2026-11-11 Q1: amaterrace-gpu main bace7d66 を `git archive` で `build/ws347/atg-bace7d66/` に取り出し、`config/minimal.mk` を config.mk にして `include/atg_config.vh` を作り（CFG_HAS_BLT 0・CFG_SU_NPHYS 4・CFG_MC_ENABLE_PROT 0）、IP の core（amaterrace-gpu の build/vivado/kc705_ip）を `build/ws347/kc705_ip/` に写して `tools/vivado-build.sh kc705 atg_kc705_top`（strategy Default、出力 `build/ws347/out/`）を流し始めた。
