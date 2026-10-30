<!-- awesome-plan project=zedbsd record=ws331-p007 -->

# ws331-p007: HAL の early console の framebuffer の map を、kernel が外せる形に（amd64 の fb_window を他の platform へ）

Status: in-progress
Parent: [WS331](../ws.md)
Queue: q1720（P2）

## 目的

2026-10-30 ユーザーから、HAL が unmap device できるよう、early console の GOP（boot の framebuffer）の領域を map の list に入れておく形が GHCI の時に PC か VF2 で入っているはずなので確かめ、あれば他の platform でも真似する、という趣旨の指示を受けた。

## Q1 が確かめた事（amd64）

- `src/hal/amd64/space.c` の `fb_window`（状態 NONE・HELD・GIVEN・REMOVED、`fb_window_hold`・`fb_window_give`・`fb_window_remove`、3392 行の辺り）: HAL の early console が boot の framebuffer を専用の窓（ZBL6_FRAMEBUFFER_VIRTUAL_BASE）に map して HELD。kernel が framebuffer のちょうどの範囲を `hal_space_map_device` で求めると、その窓を渡して GIVEN（424 行）。kernel がそれを `hal_space_unmap_device` で外すと REMOVED（462 行）、early console は `pcat_cons_framebuffer_release`（bsp.h:65）で以後描かない。
- kernel の側は pcat の `backend.c` の `drv_pcat_graphics_backend_end`（1330 行の辺り）が、`kern_device_map` で窓を受け取り `kern_device_unmap` で外す。

## 範囲

1. arm64（Raspberry Pi 4・5 の `bsp-rpi-common`、ODROID-C5 の `bsp-s905x5m`）と riscv64（VisionFive 2）の HAL に、同じ形の窓（hold・give・remove と early console の release）を実装する。Pi は今 framebuffer を direct map で描いているので、専用の窓に移す。hal.h は変えない（HAL の実装の補完）。
2. 共通の `boot-fb.c` の end を、pcat の backend と同じく、HAL の窓を `kern_device_map` で受け取って `kern_device_unmap` で外す形にし、外れたら EBUSY を返さない。
3. HAL の stall の報告・致命的な停止の表示は、窓が外れた後は framebuffer に描かない（HVS の plane など、描ける所だけ）。
4. 確かめ: QEMU raspi4b で、GPU の最初の flip に当たる試験（kern_text_end に範囲を渡す）で EBUSY が出ず map が外れる事、login と描き直し。VF2 の実機で起動と /dev/graphics、map が外れる事（log）。Pi 5・ODROID-C5 は build。20 port の build。
