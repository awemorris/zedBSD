<!-- awesome-plan project=zedbsd record=ws349-p003 -->

# ws349-p003 計算の部品（wire）と host 試験

- WS: [ws.md](../ws.md)。設計: [p001](../phase001/phase.md)。依存: p002（values.md）。
- 状態: cleared（Q1 の受け入れ待ち。2026-11-15 P8、Queue q2124-i01、worktree `/home/awe/zedBSD-worktrees/p8-ws349p002`、branch `agent/p8-ws349p002`）
- 担当の定義の推し: phase-runner-high（driver の部品）。

## 目的

hardware に触れない計算を `src/drivers/platform/rpi5/rpi5-hdmi-audio-wire.c`・`.h` にまとめ、host で試す。mk の枠が無くても進められる（host 試験は cc で直に build する）。

## 作る物

- `rpi5-hdmi-audio-wire.h`・`.c`（[coding-style](../../coding-style.md) の全文、出典は block ごと、名前は独自）:
  - IEC 60958 の subframe: 16 bit の 1 frame（左右）から 2 word。preamble、parity、channel status の bit（192 frame の block の中の位置から）。
  - fragment の変換: framework の ring の fragment 1 つ（1024 frame）と通し番号から subframe の fragment。
  - 通し番号の進め: 前回の位置と今の位置（SRC から求めた fragment）から、取りこぼした割り込みを含めて進める。
  - channel status の 24 byte（48 kHz、16 bit）。
  - CTS と N: PHY の読み戻し（RM_OFFSET・REFFRQ・VCOCLK_DIV）から f_TMDS、規格の N の表、CTS（整数でない時の前後の 2 つ）。読み戻しが「分からない」時の誤り。
  - MAI_SMP の N・M（最良の有理近似）。
  - Audio InfoFrame の byte と packet RAM の word（slot の 0x24 byte、残りの 0）。
  - DMA の control block の環（40 bit の番地の分け方、TI・SRCI・DESTI、NEXT_CB、各 fragment の INTEN、cell の印の読み）。
  - SRC・SRCI から fragment と ring の中の位置（offset ÷ 2、終わりの番地は 0）。
  - stepping の表（HVS の VERSION → C・D0、DREQ、MAI_THR の field の詰め方）、tree の cell との照合の結果。
- `plan/ws349/tests/run-hdmi-audio-wire-test.sh` と `plan/ws349/tests/hdmi-audio-wire-test.c`（`plan/tools/rpi4-audio/run-rpi4-audio-ring-test.sh` と同じ形: 通常と ASan・UBSan の 2 通り、出力は自分の `build/tmp/`、最後の行 `hdmi-audio-wire: all PASS`）。

## 試験の項目

1. subframe: 既知の sample（0、0x7fff、0x8000、0x1234）の word、parity が bit 4〜31 で偶数、block の始めだけ preamble 0x8、左 0x2・右 0x4。
2. channel status: 192 frame の bit の列が 24 byte と一致。ring の巻き戻し（8192 frame）をまたいで block が続く。同じ fragment の変換し直しが同じ word。割り込みを 1・3 回取りこぼしても block がずれない。
3. core の mmap の写しとの順の模擬: core が fragment n+1 を写した後に変換した時に新しい中身が出る（p001 D7）。prepare の時の全変換と start の 0・1 の変換し直し。
4. CTS・N: 25.2、27、74.25、74.25/1.001、148.5、148.5/1.001、297、594 MHz と 10 bit・12 bit の deep color の f_TMDS に当たる RM・VCODIV から、f_TMDS と N・CTS が規格の表と一致。OFFSET_ONLY が落ちている・VCODIV_EN が 0・範囲の外は誤り。
5. MAI_SMP: 108 MHz で N=2250・M=1、他の rate でも誤差の上限。
6. InfoFrame: checksum（全 byte の和が 0）、word の並び、残りの 0。
7. DMA: CB の各 word、NEXT_CB が次の CB（最後は先頭）、番地の上位 8 bit、4 GiB の境を跨ぐ配置を拒む。cell `0x41fa000c`・`0x41fa000a` の読み。
8. 位置: SRC の各値（fragment の中、境、終わり）から fragment と offset。
9. stepping: 0x53・0x54・他、C1 の dtb の cell と D0 の chip の組み合わせで「食い違い」と D0 の DREQ。

## 受け入れ条件

- 試験の script が通常と ASan・UBSan で `hdmi-audio-wire: all PASS`（出力の最後の行を読む）。
- wire の file を arm64 の kernel の flag で compile して warning 0（mk の枠が無ければ、kernel の build の compile の行と同じ flag で単独に compile する。行は自分の build の log から取る）。
- 出典が block ごと、file の先頭に出典を書かない。

## 試験（短い）

- `sh plan/ws349/tests/run-hdmi-audio-wire-test.sh`（timeout 600 s）。

## 結果（2026-11-15 P8）

- 作った物: `src/drivers/platform/rpi5/rpi5-hdmi-audio-wire.h`・`.c`（hardware に触れない計算。public は `drv_rpi5_hdmi_audio_*` の 18 関数）、`plan/ws349/tests/hdmi-audio-wire-test.c`・`run-hdmi-audio-wire-test.sh`（79 の check）。
- 中身: channel status の 24 byte、subframe（preamble・C・P）、fragment の変換（block の位置は通し番号 mod 3 から）、通し番号の進め、PHY の読み戻しから f_TMDS（OFFSET_ONLY・VCODIV_EN・REFFRQ・25〜600 MHz を確かめる）、N と CTS（8 bit の表、deep color は N を探す、名目の rate に ±200 ppm で寄せる、`hardware_cts` の時は N 6144）、MAI の sample の分周（連分数の最良近似、N ≤ 0xffffff・M ≤ 256）とその word、InfoFrame と packet RAM の 9 word と slot の offset、DMA の cell の読み・CS の run の word・40 bit の CB の環（4 GiB の境・40 bit・CB の field の上限を拒む）、SRC・SRCI からの fragment と PCM の offset、HVS の VERSION からの stepping、DREQ の表と tree の cell の照合、MAI_THR の詰め方（C・D0）。出典は block ごとに「Values referenced:」「Procedure referenced:」（[values.md](../values.md) の行）、file の先頭には書いていない。
- 試験（host、自分の worktree の `build/tmp/`）: `timeout 600 sh plan/ws349/tests/run-hdmi-audio-wire-test.sh` → ASan・UBSan と通常の両方で `hdmi-audio-wire: 79 checks`・最後の行 `hdmi-audio-wire: all PASS`（1.4 s）。mmap の順の模擬は、変換を core の写しの後にする順で新しい音、前にする順で古い音になる事の両方を確かめた（DMA が次の fragment を読み始めるのは割り込みの手続きの前、という前提で判定）。
- kernel の flag の compile: `mk/platform-rpi5/vmunix.mk` の KERN_CPPFLAGS・KERN_CFLAGS と同じ flag（sysroot の include の代わりに同じ中身の `include/libc`、clang は共有の `build/llvm/bin/clang` 23.1.0 を実行だけ）で `-c` → warning 0（`-Werror`）。未定義の symbol は 0（`llvm-nm`）。`-std=c89 -pedantic -Wdeclaration-after-statement` でも `long long`・`_Bool`（tree の header の物）の他は warning 0。`git diff --check` 0。
- 未実施: kernel の build への組み込み（mk の枠が無い、U1）、x86_64・riscv64 の build（Pi 5 だけの file なので対象外）、実機・QEMU（範囲の外）。
- p004 への申し送り: 割り込みの手続きの順は D7 のまま（`drv_audio_interrupt()` の後に k と k+1）。f_TMDS が表の rate から ±200 ppm の外なら N 6144 と CTS の近い整数（exact は false）。EXTERNAL_CTS_EN を立てる既定では `hardware_cts` を true で呼ぶ。
