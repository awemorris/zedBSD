<!-- awesome-plan project=zedbsd record=ws017-p003 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws017/phase003/phase.md`

親: [ws017](https://github.com/awemorris/zedBSD/issues/18)

# WS017 Phase 003: Xzed mapped-LFB fast path

Last updated: 2026-08-27

WSID: `ws017`

Phase ID: `p003`

Combined ID: `ws017-p003`

Status: Planned; Queue-ready after `ws017-p001`--`p002`

Parent: [WS017](https://github.com/awemorris/zedBSD/issues/18)

Tests: [WS017 test index](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws017-lfb-graphics/tests/README.md)

## Objective

Make Xzed render completed dirty rectangles directly into a valid true-color
graphics mapping and automatically retain its current RGB24 BLIT/FLUSH path
for every unsupported or failed mapping case.

## Work packages

1. After `GRAPHICS_ENTER`, query LFB information and select the fast path only
   for a validated, mappable 16/24/32-bit direct-RGB layout.
2. Map exactly the reported length with shared read/write access and use
   `mapping + data_offset` plus byte stride; never use a physical address.
3. Keep Xzed's internal 32-bit RGB screen/window composition model. For each
   dirty pixel, including cursor overlay, scale 8-bit components to the
   returned normalized masks, shift them, preserve only declared unused bits
   as zero, and store 2/3/4 little-endian bytes without unaligned word access.
4. Bound dirty rectangles against width/height and check every row/offset
   computation before writing. Keep the existing `FLUSH` notification after
   direct writes so future non-coherent backends retain an ordering hook.
5. On absent capability, indexed 8-bit mode, query failure, invalid layout, or
   mmap failure, issue the existing RGB24 `BLIT` plus `FLUSH` with unchanged
   behavior.
6. Unmap before closing graphics ownership during normal cleanup and every
   initialization failure. Emit one concise startup diagnostic stating
   `lfb-mmap` or `ioctl-blit` and its selected geometry/format.

## Verification and completion conditions

LFB-T008--T010 compare exact red/green/blue/cursor bytes for representative
RGB565, BGR565, RGB888, BGR888, RGBX8888, and BGRX8888 layouts, exercise
non-page-aligned data offsets/stride padding/edge rectangles, and inject every
fallback condition. Existing Xzed protocol, pointer, launch, and session tests
must remain passing. The Phase completes when no fast-path write escapes the
reported visible extent, fallback output matches the prior path, cleanup has
no mapping/owner leak, and `make -j16` plus `git diff --check` pass.

## Reconsideration boundary

Stop if direct rendering requires changing the X11 wire-visible depth or if a
backend needs explicit cache synchronization beyond the existing `FLUSH`
contract. Extract that coherency design instead of hard-coding amd64 behavior
into Xzed.
