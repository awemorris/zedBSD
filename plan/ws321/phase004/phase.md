<!-- awesome-plan project=zedbsd record=ws321-p004 -->

# ws321-p004 規約の全文の見直し（AGENTS §2）

- 親: [WS321](../ws.md)。Queue: q2111-i01（P2、worktree p2-ws321、branch agent/p2-ws321、基点 main 44bb7cdaac）。
- Status: cleared（2026-11-15 Q1、merge 91a6c53f0a）
- 依存: p002。

## 範囲

WS321 が足した C を [coding-style.md](../../coding-style.md) の全文で直す（動作は変えない）。

- `src/drivers/gpu/videocore/hevc-decoder.[ch]`
- `src/drivers/gpu/videocore/vk/vulkan-video-hevc.[ch]`・`vk/vulkan-video-hevc-slice.c`
- host 試験 `hevc-slice-host-test.c`（今は `plan/tools/h265/hevc-slice/`）

共有の executor（`vk/*.c`・`render-runtime.c`・`attach71.c`・`v3d71-job.c`）の WS321 の差分は `#if` と数行で、この Phase では読むだけにした。

## 記録と試験の今の形への直し

- ws.md・phase001〜003 の古い directory の名前（`bcm2711/`・`bcm2712/`）を `src/drivers/gpu/videocore/` に、`CONFIG_DRIVER_BCM2712_VIDEO` は今 `CONFIG_DRIVER_RPI5_GPU` に含まれる事を書いた（phase002 の「確かめ」の command は当時の形のまま、と注記）。
- host 試験の path を `src/drivers/gpu/videocore/vk/` に直した（run.sh と host 試験の include）。
- `plan/ws321/tests/config-rpi5-hevc.mk` を `include config/config.rpi5.mk`・`CONFIG_USERLAND_PROGRAMS +=`・`CONFIGMK=` の build の形に直した（`make ... --eval` で PROGS に keiland-shot・vkvideo-probe・media-probe・videoplayer・wayland・files・libvulkan・libmedia、`ZEDBSD_TEST_SCREEN_CAPTURE=y`、`CONFIG_DRIVER_RPI5_GPU=y` を確かめた）。
- host 試験を `plan/tools/h265/hevc-slice/` へ移し（`git mv`）、[tools.md](../../tools.md) に登録した。要らない `-Iplan/tools/h264sw/unit -Iplan/ws324/tests/unit` を外した（未完の WS324 の試験の dir に依らない）。

## 直した事（style-check の数）

`python3 -I plan/tools/style-check.py --summary <file>`:

| file | 前 | 後 |
| --- | --- | --- |
| hevc-decoder.c | 1（conditional） | 0 |
| hevc-decoder.h・vulkan-video-hevc.h | 0 | 0 |
| vulkan-video-hevc.c | 33（blank-after-brace 24、call-in-condition 8、paragraph-comment 1） | 0 |
| vulkan-video-hevc-slice.c | 30（blank-after-brace 19、call-in-condition 11） | 0 |
| hevc-slice-host-test.c | 140（blank-after-brace 57、call-in-condition 18、conditional 20、forward-declaration 7、paragraph-comment 38） | 0 |
| 計 | 204 | 0 |

checker の外で直した主な事: 失敗しうる呼び出しを一つずつ検べる（session の list・clock・device の確保を `session_resources` に、`report_iommu` の連鎖）、条件の中の呼び出しを変数に（`find_sps`・`find_pps`・`set_count`・`bits_read`・`bits_remaining`・`tile_of`・`clock_ticks`、いずれも副作用が無いか、元の短絡の順を保った）、3 つ以上の節の条件を行に分ける、段落と if の注釈。host 試験は関数の前方宣言・file-scope の変数の注釈・`?:` を if に・名前（`w`・`n`・`ok` ほか）・成功の return を最後に、長い関数から helper を 7 つ（P2 の helper が行った）。

残した事: `start_decode` の `(void)controller;`（この driver の dir の他の file と同じ形。`UNUSED_PARAMETER` は arm64 の HAL の defs.h にあるがこの file は含めない）。host 試験の `test_picture` は約 480 行の一つの関数のまま（分けると約 10 の変数を渡す）。

## 確かめ

| command | 結果 |
| --- | --- |
| `make -j4 CONFIGMK=config/config.rpi5.mk vmunix`（3 file を touch して再 compile） | rc 0、compiler の warning 0、`arm64 vmunix check: PASS (entry=0xffff000000080000, image=build/rpi5/vmunix, image_size=0x6ba000, bss_end=0x6b961c)` |
| `make -j4 CONFIGMK=config/config.rpi5.mk build/rpi5/packages/libvulkan/.stamp` | rc 0、warning 0（`SHLIB libvulkan.so`） |
| `make -j4 CONFIGMK=config/config.rpi5.mk build/rpi5/packages/vkvideo-probe/.stamp` | rc 0、warning 0（`LD bin/vkvideo-probe`） |
| `H265SW_CONFORMANCE_DIR=<worktree>/build/sources/h265-conformance sh plan/tools/h265/hevc-slice/run.sh`（移した後） | `hevc-slice run: PASS (147 streams, 15 refused)`。summary.txt（stream ごとの行と fuzz の行）は直す前と diff 無し: `fuzz program returns: 0 8197 EINVAL 25149 ENOTSUP 0 ENOSPC 12512 other 0`、`fuzz violations 0, mismatches 0, checks refused with sets not checked 0` |
| `git diff --check` | 無し |

適合の bitstream の cache は main の checkout に無かったので、`plan/tools/h265/conformance/fetch.sh` で worktree の `build/sources/h265-conformance` に取った（`fetch: PASS (147 zips)`、SHA-256 一致）。

## 未実施・残り

- Pi 5 の実機（依頼の範囲の外。SAODBLK_A・B と VPSSPSPPS_A の md5 の照らし直しも未実施）。style の直しは kernel の executor の動作を変えない意図だが、実機の decode での確かめは無い（host 試験は slice の reader と命令の FIFO だけを走らせる。`vulkan-video-hevc.c`・`hevc-decoder.c` の直しは build だけ）。
- Pi 4 の build（`CONFIG_DRIVER_RPI4_GPU`）: WS321 の file は Pi 4 の build に入らない（`mk/kernel.mk`）ので流していない。
- `plan/ws321/tests/pi5-hevc.sh`（実機の script）は ws321 に残した。
