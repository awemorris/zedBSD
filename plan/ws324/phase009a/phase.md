<!-- awesome-plan project=zedbsd record=ws324-p009a -->

# ws324-p009a NVENC の channel（段 nvenc-channel）: engine の確保、class の bind、firmware の確かめ、RC の通知の記録

Status: in-progress（2026-11-02 P1、q1880-i01）

- WS: [WS324](../ws.md)。設計: [design.md](../design.md) §3.1・§3.3（firmware）・§7.1・§9・§12 R5・§14.1 T3・§14.3・§15。
- 基点: main `0a4779015`（worktree `/home/awe/zedBSD-worktrees/p1-ws324`、branch `agent/p1-ws324`）。

## 1. 目的

RTX 2070（TU106、nvrtx）の NVENC に、NVDEC と同じ形の channel を work channel の address の空間に作り、class の bind と method が engine まで通ることを実機で確かめる。encode の絵は作らない（関門の p009b）。

## 2. 手順（実装の形）

- 新しい file: `src/drivers/gpu/nvrtx/nvenc.c`（channel）・`nvenc-records.c`（push と RM の params、host 試験）・`nvrtx-nvenc.h`。
- 段 `nvenc-channel`（`gsp.c`）: 条件は `nvrtx.video=1`、work channel の後、NVDEC の段の後（NVDEC が無くても試す）。`nvrtx.stop=nvenc-channel` で飛ばせる。失敗しても boot は続く。停止は NVDEC の前、解放も NVDEC の前。
- 手順: devinfo（RM の内部の番号）の engine type 0x25 の entry の runlist と ENG_DESC（`objects.c` の `nvrtx_devinfo_pick` に nvenc の 3 欄）→ `GET_CONSTRUCTED_FALCON_INFO` で context の大きさ（0 なら promote しない）→ VRAM の instance block・USERD と context → system memory の page（ring・semaphore 2 つ・push）→ channel の確保（NV2080 の engine 0x1b）・BIND・SCHEDULE → `PROMOTE_CTX`（engine 0x1b）→ object `0xC4B7`（`NV_MSENC_ALLOCATION_PARAMETERS` 12 byte、engineInstance 0）。chid 49（NVDEC の 48 の次）、handle `0xf1f00031`・`0xc4b70031`。
- 最初の submission を 2 つに分ける:
  1. subchannel 4 に SET_OBJECT（class）と NOP（`clc5b7.h` 0x100）、host の semaphore の fence（WFI、2 s）。channel と class の bind と、engine の 1 つ目の method。
  2. NVENC 自身の semaphore の release（SEMAPHORE_A・B・C 0x240〜0x248、SEMAPHORE_D 0x304 = 0: 1 語の release）で page の 0x810 に `0x4e56454e` を書かせ、host の fence（2 s）。fence が通り、値が入っていれば、engine の firmware が method を受けて memory を書いた（R5: GSP-RM が ucode を持つかの答え）。
- RC の通知の記録: `rpc.c` の channel error（RC_TRIGGERED、0x1004）の分岐で `gpu->rc_notices`・`rc_last_chid`・`rc_last_type` を数える（rpc_lock の下）。段の始めの数を取り、2 つの submission の後（成功でも失敗でも）200 ms の間 GSP の message を読み（`nvrtx_rpc_wait` の drain）、段の中で来た数と最新の chid・kind を stage の log に出す。
- HAL・`include/uapi/` は変えない（要らなかった）。

## 3. 受け入れ

- build: `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/ws324-p009a/k build/ws324-p009a/k/vmunix` が warning 0。
- host: `sh plan/ws324/tests/nvenc-host-test.sh`（T3: class、push の語、MSENC と promote の params、devinfo の pick の 0x25、NVDEC の host 試験の非退行）が PASS。
- 実機（10.0.10.3、`plan/ws324/tests/rtx-nvenc-channel.sh OUT VMUNIX`、lock の中）: dmesg（SSH）に `nvenc-channel ok`、`the encoder released 4e56454e itself`、channel error の数。NVDEC の `nvdec-channel ok` と work channel が今どおり、desktop の session の socket。終わりに NVMe の kernel と boot の行を戻して cksum が前と一致、Debian に戻る。
- R5 の答え（firmware を別に要るか）の記録。

## 4. 結果

（実機の前。build と host 試験は下。）

- build（2026-11-02 00:51）: 上の command で rc=0、warning 0（log の `warning` 0 行）。vmunix 3631880 byte。
- host（2026-11-02）: `nvenc host test: 24 checks, 0 failed`、続く `nvdec host test: 88 checks, 0 failed`。
- 未実施: 実機。
