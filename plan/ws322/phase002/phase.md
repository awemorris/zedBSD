<!-- awesome-plan project=zedbsd record=ws322-p002 -->

# ws322-p002 VF2 の VPU の glue と Wave5 の core の boot、firmware の package（wave511）、実機の probe

- WS: [WS322](../ws.md)。状態: cleared（2026-11-01 22:40 P2 q1852-i01。Q1 の受け入れ待ち）。担当: P2（q1852-i01）。
- 設計: [design.md](../design.md) 第 2 版の §3.1・§4・§7.1〜§7.6・§9.2・§10.2・§14.3・§15 の p002。

## 目的

JH7110 の Wave511 を zedBSD から動かす最初の段: power domain・clock・reset の glue、firmware の boot と命令・割り込みの core、decode の instance の最小の道（instance・sequence・frame buffer・1 枚の decode）、firmware の package、そして p004 の形を決める実機の probe（R1・R2・R3・R5・R14 と firmware の版）。Vulkan の口（engine の登録、executor）は p003・p004。

## 手順

1. `vf2-pmu.c`・`.h` に VDEC（3）・VENC（6）の domain を許す。`vf2-crg.c` の SYS の表に Wave511（69〜76）と Wave420L（77〜82）の clock の行を足す（reset は表が無く番号で通る）。
2. `src/drivers/vpu/wave5.h`・`wave5-private.h`・`wave5-core.c`（software reset、firmware の写しと remap、INIT_VPU、identity の query、命令・query・割り込み・poll の待ち、uncached の memory）・`wave5-decode.c`（CREATE・DESTROY・ring・INIT_SEQ・SET_FB（linear → FBC の 8 枚ずつ）・1 枚の更新（option 1）・display の flag・DEC_PIC と GET_RESULT・ring の続けての追記）。
3. `vf2-vpu.c`・`.h`（domain・clock・reset、tree か board の場所、firmware を root から読む、`CONFIG_DRIVER_VF2_VPU` は boot の thread で上げる、`CONFIG_DRIVER_VF2_VPU_PROBE` は `/dev/vf2-vpu-probe` の read で上げて probe して下げる）、`vf2-vpu-probe.c`・`.h`、`kern/platform/visionfive2.c` の呼び出し（`#if` の中）、`Makefile`・`platform/riscv64/vmunix.mk`・`riscv64.drivers` の config（既定 n）。
4. firmware の package `userland/firmware/wave511/`（chagall.bin と LICENSE.txt、`firmware.list` の行、`license-components.json` の行、README）。
5. 試験の道具 `plan/ws322/tests/`: `config-visionfive2-vpu-probe.mk`・`config-visionfive2-vpu.mk`、`p002/make-streams.sh`（流れと host の ffmpeg の参照の FNV-1a）・`fnv-frames.py`・`board-run.sh`・`run-vf2.sh`（`plan/tools/vf2/boot-cycle.sh` で 1 回の起動、終わりに板の kernel を戻し置いた物を消し cksum で確かめる）・`compare.py`。
6. VF2 の実機で probe（7 回、条件を変えながら）と boot の時の bring-up。

## 受け入れ（design.md §15 の p002）

| 項目 | 結果 |
| --- | --- |
| 実機で product code `0x5110` と firmware の版の log | 済: product `0x5110`、name `WAVE`、chagall.bin は版 `0x0002d46a`、buildroot の wave511_dec_fw.bin は `0x00038582`。boot の時の kernel の dmesg に `vf2-vpu: WAVE 5110 firmware 0002d46a` |
| probe の H.264・H.265 の全 frame が参照と一致 | 済: way A で 6 本（H.264 の pyramid 15・main 15・slices 3・1080p 10、H.265 の B 15・1080p 10）の全 68 frame が host の ffmpeg の NV12 の FNV-1a と一致（表示の順）。両方の firmware で同じ hash |
| R1・R2・R3・R5・R14 の観測の記録 | 済（下の表） |
| riscv64 の build の warning 0 | 済: probe・boot・既定の 3 つの config の vmunix で warning 0 |
| off の build の object 不変 | 一部: 既定の config で `visionfive2.o` は main 6eded7df3 と同じ byte。`vf2-pmu.o`・`vf2-crg.o` は表の行と許す domain が config に依らず増えるので変わる（意図した差、GPU・display の行は不変） |

## 結果（2026-11-01 P2、q1852-i01）

### commit

- `bca9cfd19d`（実装と試験の道具の最初の形）、この Phase の記録と probe の直し・package は次の commit（Q1 への報告に SHA）。

### probe の観測（VF2 10.0.30.2、kernel は試験の物を置いて 1 回起動、終わりに元に戻した）

| 項目 | 観測 | p004 への意味 |
| --- | --- | --- |
| 遅れ 0（R1） | `FORCE_FB_LATENCY_PLUS1 = 1` で、decode された全ての絵で decoded の index ＝ display の index、decode の直後に出る（way A の 68 frame、POC で並べ直すと参照と一致）。VUI の書き換えは要らない | §7.4 の形のまま |
| index の組と display の flag（R2） | 選んだ 1 つ以外の全 index の flag を立てると、firmware はその 1 つに decode する（H.265 15/15、H.264 は 8/8・6/6）。参照の index に flag を立てても参照として読まれ、出力は bit 一致のまま。firmware は decode の後に出した index の flag を自分で立てる（host が下ろすまで使わない）。まだ参照される index を選ぶと firmware は断る（decoded `-1`、bitstream を読まない）。断られた後に別の index で DEC_PIC を出し直すと完了の印（自分の instance の bit）が来ず、結果も誤る | §7.5 の 1. の選び方（active な DPB の slot に結ばれない index）が必須で、断られてからの出し直しは使えない。断られたらその絵は ERROR |
| 1 枚の frame buffer の更新（R3） | 最初の decode の前の更新（option 1）は効く（宛先に正しく書かれ、pool は書かれない）。decode が始まった後の更新では、宛先が同じ address でも次の DEC_PIC が完了しない（index の書き方を 2 通り試した: 値そのもの、`(index << 8) \| index`） | §7.5 の 2. は使えない。3.（driver の linear の pool に受けて GPU の transfer で宛先へ）を正の道にする（design の直しは後の Queue） |
| read pointer の戻し（R5） | SET_BS_RD_PTR の query は両方の firmware が知らない（reason `0x8000`、未知の query）。DEC_PIC の `BS_RD_PTR` の書き込みだけでは firmware は自分の位置から読み続ける（ring の先頭に書いた次の絵は壊れる）。前の絵の後ろに続けて書く（firmware の read の位置がそこにある）と全部正しい | §7.3 は ring を続けて使う形（wrap の扱いを p004 で） |
| VPU の DMA と CPU の cache（R14） | 読み: direct map で ring の古い行を cache に載せた後に uncached の別名で新しい絵を書いても decode は正しい（VPU は cache を見ず DRAM を読む）。書き: 宛先を direct map で読んで fill の行を cache に載せ、decode の後に direct map で読むと 64 byte の行の約 95% が fill のまま、残りは新しい値（cache から追い出された行）。uncached の別名では正しい | tree の `dma-coherent` に反し、VPU は CPU の cache と一貫しない。uncached の view だけで触る今の形（map の時に書き戻して捨てる）が正しい。`kern/dcache.h` の追加の保守は要らない |
| CREATE_INSTANCE の完了 | busy の flag で終わる（割り込みの理由は来ない）。INIT_SEQ は理由 `0x40`、DEC_PIC は `0x100` の割り込みで来る | §7.1 の busy の待ちのまま |
| 割り込み（R6） | PLIC 13 で来る（poll で見つけた完了は 0） | poll は保険のまま |
| clock の rate | AXI（69・70・75・76）396 MHz、BPU（71）396 MHz、VCE（72）500 MHz、APB（73）49.5 MHz。firmware（U-Boot）が残した値のまま | |
| tree に無い reset（NOC 31、VDEC_MAIN 52） | 起動の時から解かれている。触らずに boot が通る | 解く道は残すが使われない |
| 1080p の 1 枚の時間 | H.264 1080p: core の tick で 2.07〜2.37 M cycle、host の時計で 7〜8 ms（1 ms の刻み、GET_RESULT と query を含む）。H.265 1080p: 1.56〜1.81 M cycle、6 ms | 1080p30 の decode には足りる見込み、60 は p007 で測る |
| chagall.bin と wave511_dec_fw.bin | 別の byte（949120 B・SHA-256 `aad0383b…` と 952768 B・`398eb201…`、版 `0x2d46a` と `0x38582`）。両方 boot し、全ての way で同じ hash（tick だけ少し違う） | 配るのは license の file がそろった chagall.bin（Q1 の判断）。driver は `/lib/firmware/wave511/chagall.bin` を読む |

### boot の時の bring-up（`CONFIG_DRIVER_VF2_VPU=y`）

- chagall.bin を置いて起動: dmesg に `vf2-vpu: WAVE 5110 firmware 0002d46a`。同じ起動で `gpu-selftest` 13 ok・0 fail、`vkcompute` PASS 4096/4096（GPU の非退行）。SSH は 31 s で戻る。

### 板の後始末（各回で確かめた）

- `/boot/boot0/vmunix` cksum 3758577736（vmunix.q1825orig と一致）、`/etc/group` 4260090434、`zedbsd.cfg` 632416842 は前後で同じ。`/var/vpu-probe`・`/lib/firmware/wave511`・`~kei/vpu-stage` は消した。

### 証拠の場所（git に入れない）

- worktree の `build/tmp/vf2-run1`〜`vf2-run7`（`summary.txt`、`probe-chagall.txt`・`probe-dec-fw.txt`、`cycle/boot1/`）。流れと参照は `build/tmp/vpu-probe`（`make-streams.sh` で作り直せる）。

## 未実施

- R14 の「VPU が書いた物を GPU の transfer で読む」は行っていない。CPU の uncached の別名（DRAM の値）で代えた。GPU の読みは p004 の executor の道で。
- p004 の host の試験（T1〜T7）は p004 の範囲。
- zedBSD-firmware の repository に `wave511/`（chagall.bin・LICENSE.txt）はまだ無い。package は repository からの取得に失敗すると上流（soft_3rdpart の固定の commit）から取り、SHA-256 で確かめる（上流からの取得は確かめた）。repository への追加は Q1。
- `make menuconfig-host-test` は MAC-T001 で落ちる（x86_64 の既定が `amd-navi-firmware` を選ぶ、この Phase の変更と関係しない既存の食い違い）。wave511 の行の検べ（package の有無、driver の無い firmware）はその前の段で通っている。

## 引き継ぎ（design.md の直し、p002 の後の Queue）

- §7.3: read pointer の戻しをやめ、ring を続けて使う（wrap の時の扱い）。
- §7.5: 1 枚の更新（2.）を外し、pool と GPU の transfer（3.）を正の道に。index の選びは active な DPB の slot に厳密に結ぶ（断られたら ERROR、出し直さない）。
- §9.2・R14: VPU は非一貫と観測。uncached の view だけで足りる。
- §4・R11: 配るのは chagall.bin（版 0x2d46a）。
