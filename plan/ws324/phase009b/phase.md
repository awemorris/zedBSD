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

（実施中）
