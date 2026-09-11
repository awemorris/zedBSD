# WS017 shared test cases

Parent: [WS017](../ws.md)

No test implementation exists yet. Future Queue execution places reusable
fixtures in this directory and disposable QEMU evidence under `../temp/`.

| Case ID | Owning Phase | Required observation |
| --- | --- | --- |
| LFB-T001 | p001 | Versioned 32/64 UAPI layout, zero reserved fields, capability, and unavailable query are stable |
| LFB-T002 | p001 | 8/16/24/32 layout validation accepts exact valid forms and rejects masks/stride/offset/extent overflow |
| LFB-T003 | p001 | Device mmap rejects private/exec/nonzero-offset/wrong-length/protection escalation and maps valid device pages |
| LFB-T004 | p001 | Fork, split, partial unmap, close-before-unmap, process exit, and second open preserve one lease and one final release |
| LFB-T005 | p002 | amd64 RGBX8888/BGRX8888 handoffs produce exact width/height/stride and RGB masks |
| LFB-T006 | p002 | User/kernel views are coherent inside a fully framebuffer-owned page span; unproven prefix/suffix bytes force fallback and no adjacent RAM/MMIO is exposed |
| LFB-T007 | p002 | Final unmap resumes console/reopen; Cirrus/VGA and absent/invalid handoffs remain on ioctl fallback |
| LFB-T008 | p003 | Xzed conversion emits exact RGB/cursor bytes for valid 16/24/32 mask layouts |
| LFB-T009 | p003 | Dirty edges, stride padding, data offset, arithmetic limits, and cleanup never escape the visible aperture |
| LFB-T010 | p003 | Indexed/absent/invalid/query-failure/mmap-failure cases retain prior RGB24 BLIT/FLUSH output |
| LFB-T011 | p004 | amd64 UEFI `startx` reaches Xzed/zwm/zshell/zterm with `lfb-mmap`, correct redraw/input, exit, and restart |
| LFB-T012 | p004 | Forced LFB unavailability reaches the same usable desktop with `ioctl-blit` fallback |

The supported build gate is `make -j16`; the aggregate `make check` target and
repository `.internal/` tests are not part of this WS. PC-98 Cirrus is outside
this test matrix.
