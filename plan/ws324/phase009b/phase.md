<!-- awesome-plan project=zedbsd record=ws324-p009b -->

# ws324-p009b NVENC の関門: kernel の中の自己診断で H.264 の I 絵と P 絵を encode し、host の ffmpeg で decode する

Status: in-progress（2026-11-02 01:22 P1、q1884-i01）

- WS: [WS324](../ws.md)。設計: [design.md](../design.md) §3.3（NVENC の interface、版の食い違い R1）・§7.2（push）・§7.4（H.264 の 1 絵の設定）・§12 R1・R2・§15 p009b。
- 基点: main `ad65fbe3f`（worktree `/home/awe/zedBSD-worktrees/p1-ws324`、branch `agent/p1-ws324`）。前提: [p009a](../phase009a/phase.md)（channel は class c4b7・chid 49、GSP-RM が ucode を持つ）。
- 時間の上限: 無い（2026-11-01 ユーザーの Q324-4 の答え）。手詰まり（同じ所で 3 回以上違う仮説が外れた）の時は、仮説と観測をここに書いて Q1 に途中で知らせる。

## 1. 目的

NVENC が driver の組んだ 1 絵の設定（`nvenc_h264_drv_pic_setup_s` と slice・me・md・q の制御）で H.264 の I 絵（IDR）と P 絵を encode し、出力の bitstream が規格どおりであることを、host の ffmpeg の decode で確かめる。R1（版）・R2（既定の無い値）の答えを記録する。H.265 は H.264 が通った後に、できれば。

## 2. 手順（実装の形）

- `nvenc-records.c`・`nvrtx-nvenc.h`: H.264 の 1 絵の設定の組み立て（`nvrtx_nvenc_h264_setup`、512 byte の本体と制御の配列、合わせて 1536 byte）と 1 回の encode の push（`nvrtx_nvenc_push_encode`）。host 試験で compile する（hardware に触れない）。
- `nvenc-test.c`（新、段 `nvenc-test`）: boot の引数 `nvrtx.nvenc.test=1`（と `nvrtx.video=1`）の時だけ、nvenc-channel の後に 1 回。system memory の buffer（入力の絵 2 枚・再構成 2 枚・bitstream・状態・history・co-located・rc の持続）を work channel の空間に写し、CPU で 16x16 の tile の形の入力を書く。I 絵（IDR、frame_num 0、POC 0）、P 絵（参照は I の再構成、frame_num 1、POC 2）を CQP（QP 26）で encode し、状態（128 byte）と bitstream と再構成の頭を kernel の log に 32 byte の行で出す（行ごとに offset と FNV-1a、全体の長さと FNV-1a）。
- 試し（attempt）: 1 起動で既定の値の組を順に試し、I・P とも状態の誤り 0 で bitstream が出た所で止める。fence が来ない（hang）と channel は使えなくなるので、その起動の残りは試さない。
- 絵: 256×128、luma は 16x16 の block ごとに一様な値（block の中の並びに依らない）、chroma は全体で一様（Cb 100・Cr 150）。P 絵は I 絵を 16 pixel 右へずらした物（左端の列は新しい値）。
- host: `plan/ws324/tests/nvenc-decode.py`（試験だけ）が log から bitstream を組み直し（FNV-1a で照合）、同じ値の SPS・PPS を前に付けて Annex B の file にし、`ffmpeg -xerror` で decode、模様との PSNR と PNG。
- 実機: `plan/ws324/tests/rtx-nvenc-test.sh OUT VMUNIX`（p009a の script の形、lock の中、1 本）。

## 3. 受け入れ

- build: `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/ws324-p009b/k build/ws324-p009b/k/vmunix` が warning 0。
- host: `sh plan/ws324/tests/nvenc-host-test.sh` が PASS（設定の byte と bit の位置、push の語）。
- 実機: 状態の `error_status`・`ucode_error_status` が I・P とも 0。host の ffmpeg が I 絵と P 絵を誤り無く decode し、模様との PSNR が 35 dB 以上（luma）。R1・R2 の答えの記録。終わりに NVMe の kernel と boot の行を戻して cksum が前と一致、Debian に戻る。

## 4. 結果

（実施中。RTX 2070 の実機だけ、QEMU は使っていない）

### 4.1 設定の byte の確かめ（host）

- `nvrtx_nvenc_h264_setup` の 1536 byte を、open-gpu-doc の 2 版（97c510e・9fdf5c4）の header を host で gcc で compile して bit field で同じ値を入れた物と比べ、IDR・P・chroma の offset・codec field の 4 通りで全 byte が一致（scratchpad の照合、repo に header は入れない）。`nvenc-host-test.sh` は 61 checks・0 failed（NVDEC の 88 checks も）。

### 4.2 実機の観測（2026-11-02 01:40〜02:23、7 起動、各回 lock の中、終わりに NVMe の kernel 1463007375・cfg 38198411 を戻して MATCH、Debian に戻る）

- 状態の語（status の 4 byte 目）は `error_status`（2 bit）| `ucode_error_status` << 2。firmware が status の頭 32 byte（picture_index・error 語・pic_type）を書く。
- 誤りを判ける試し（run6）: magic を壊す → code 2（E_MAGIC_NUMBER_MISMATCH）、bitstream の method 無し → code 5（E_OUTPUT_BITSTREAM_OFFSET）、history 無し → 14（E_INPUT_HISTORY_OFFSET）、入力の絵無し → 13（E_INPUT_BITSTREAM_OFFSET）。番号は 2024 版の enum（E_RDO_ERROR が入った後）に合う → **Turing の firmware は 2024 版の interface**（R1 の答えの候補）。これらは error_status 2 で、fence が通り channel も生きる（bad magic だけは fence が来ず RC も無い）。
- 正しい設定では、毎回 error_status 1・code 0 を書いた後に GSP の RC 47（ROBUST_CHANNEL_NVENC0_ERROR、570.144 `nverror.h`）で channel が止まる（fence は 2 s で来ない）。error_status 1 は hardware の側の誤りと見る。
- channel は RC の後に free して作り直せる（`nvrtx_nvenc_restart`、chid 49 のまま、bring-up の NOP と自前の release が毎回通る）。1 起動で 5〜7 通り試せる。
- 診断の道: falcon の register（0x1c2000〜）は CPU から全部 `badf1201`（読めない）。RC_TRIGGERED の rcJournal（6544 byte）は GR・FIFO の register の写しだけで NVENC の値は無い。

### 4.3 外れた仮説（同じ HW の誤り、status も同じ）

| 起動 | 試した事 |
| --- | --- |
| run1〜3 | 既定の設定（system memory）、rc_process・coloc の method 無し、chroma を luma の address ＋ setup の offset（128）で、codec field 0・1・3、FORCE_OUT_PIC を外す、DEBUG_MODE の bit 16 |
| run4 | 絵だけ VRAM、全 buffer を VRAM（BAR1 で CPU が書く）、VRAM で optional 無し・chroma offset・codec 3 |
| run5 | GPTIMER_ON と 504・508 の両方に 81000000、rate control の値（rhopbi・30 fps・2 Mbit/s・gop 30・Np・Ravg・R）、SET_WATCHDOG_TIMER 0xffffffff、SET_IN_RCDATA（0 の page）、入力を block linear（tiled 0）、ME の mbc_mb_size 16・FBM の winner 1、その全部 |
| run7 | strips_in_frame 1・slice_encoding_row_num 8、＋mpec_threshold 16、＋GPTIMER・ME cache、行数だけ、strip だけ |

- 次: pipeline の切り分け（ME only、deblock off、CABAC、intra 16x16 だけ、64x64 の絵、非 IDR の I）。
