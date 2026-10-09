<!-- awesome-plan project=zedbsd record=ws083-p008 -->

# ws083-p008: 性能・門の既定化・利用者への案内・result status query

Status: in-progress（2026-10-10 P2: 残りは性能の数字（T1-435 の E、5330）だけ。result status query・利用者への案内・門（既定は OFF で閉じる）・SAMPLED/TRANSFER_SRC（HD5 で不要）・ws.md の制限は済み。E の数字を記録したら cleared 候補）（旧: in-progress（q897、P2。この attempt は host の分: result status query と利用者への案内））
Disposition: normal
Parent: [WS083](../ws.md)
Queue: q897（Q1 の dispatch、2026-10-08 午後「p008 の host の分を先に」）

## 範囲

[design.md](../design.md) §9 の p008 のうち実機の要らない部分:

1. **result status query**（`VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR`）: design の D15 で「最初の目標に入れない」とした物。下の設計で入れる。
2. **利用者への案内**: app を書く人向けの Vulkan Video の説明（何が使えるか、`i915.debug=video`、規格に合わない点 N1〜N4、result status、`vkvideo-probe`）。

残り（実機の後）: 性能（1080p の decode の時間、U6・U8）、D19 の既定化（`i915.debug=video` を外す、p007 の実機の後）。HD5（SAMPLED・TRANSFER_SRC）はユーザーの決定で要らない（2026-10-07）。

## 設計（result status query、2026-10-08 P2）

### 事実

- 規格: `VkQueueFamilyQueryResultStatusPropertiesKHR.queryResultStatusSupport` が TRUE の family で、`VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR`（1000023000）の pool（pNext に `VkVideoProfileInfoKHR`）を video coding scope の中の `vkCmdBeginQuery`・`vkCmdEndQuery` で囲むと、その間の video の操作（1 つ）の結果の状態が残る。`vkGetQueryPoolResults` は `VK_QUERY_RESULT_WITH_STATUS_BIT_KHR`（0x10）で各 query の最後の値に `VkQueryResultStatusKHR`（ERROR −1、NOT_READY 0、COMPLETE 1）を書く。使えない query は 0（NOT_READY）を書き、WAIT 無しなら `VK_NOT_READY`。reset は scope の外の `vkCmdResetQueryPool`。
- ANV（Mesa 25.0.7 `genX_query.c`）: TRUE を報告し、slot は 64 bit 1 つ。begin で 0、end で `MI_FLUSH_DW` の post-sync で 1 を書く（status は常に COMPLETE、失敗を見分けない）。`vkGetQueryPoolResults` は WITH_STATUS の時、unavailable でも各 query の位置 0 に availability（0 か 1）を status として必ず書き、unavailable なら `VK_NOT_READY`。
- zedBSD の今: libvulkan は FALSE（external-properties.c）、実行器は occlusion の pool だけ（fence.c、それ以外の type は ENOTSUP）。video の family の submit は query の reset を render の batch に書き、その batch は video の実行の**後**に走る（command.c の family 1、video.c の walk）。video の decode は worker で同期に走る（`i915_video_run` は VCS0 の完了を待つ）。pool の object は CPU から見える（`address`、results は CPU が読む）。

### 決定

| ID | 決定 | 理由 |
| --- | --- | --- |
| S1 | video の device（`vk->video`）の実行器だけが RESULT_STATUS_ONLY の pool を作る。object の形は occlusion と同じ（query q の 2 語と availability の語）で、status は 2q+1 の語、2q の語は 0。results は今の式（end − begin）がそのまま status を返す | results の reply を変えずに済む |
| S2 | status は GPU でなく **CPU** が書く: video の walk（apply）の中で、decode は同期に走り終わっているので、`vkCmdEndQuery` の時に CPU が status（64 bit の 2 の補数、`(uint64_t)(int64_t)status`、2q の語は 0）と availability 1 を書いて clflush。status pool の reset も CPU が op の順に（family に依らず、`drv_i915_gfx_query_execute` が render の batch を取る前に status pool の reset を CPU で行う）。**不変条件**: END の時、その query の decode の batch は VCS0 で走り終わっている（`i915_video_run` が decode ごとに走らせ待つ）。将来 decode を 1 つの run にまとめるなら END の前にその batch を走らせる（walk に注記） | render の batch の reset は video の後に走るので、status を GPU や render の batch で書くと順が逆になる。同期の実行なので CPU の書き込みは decode の後と保証される |
| S3 | status の値: query の decode が MFX で走り終われば COMPLETE（1）、D17 の検べで飛ばせば ERROR（−1）、decode の無い query は COMPLETE（zedBSD の選択、ANV も availability を status にするので同じ）。hang・DEVICE_LOST では書かない（unavailable のまま）。途中の ENOMEM（batch が作れない、`VK_ERROR_OUT_OF_DEVICE_MEMORY`）では、walk の前の方で終わった query は COMPLETE のまま残る（slot の遷移と同じ既存の途中適用） | ANV より情報が多い（飛ばした decode を app が知れる）。規格の ERROR は「操作が失敗した」 |
| S4 | 検べ（模擬の walk = apply の前にも同じ検べ、§6.5 の「一部だけ走ってから拒む形にしない」）: video の family の `vkCmdResetQueryPool` は scope の外・pool の範囲内、`vkCmdBeginQuery` は scope の中・status pool・範囲内・active な query が無い時だけ、`vkCmdEndQuery` は同じ scope の active な query と同じ pool・index、1 つの query の中の 2 つ目の decode は拒否（規格は 1 query に video の操作 1 つ）、scope の終わりに active な query が残れば拒否。video の family では `EBADMSG` → `drv_i915_video_submit` が `VK_ERROR_DEVICE_LOST`（D18 と同じ）。graphics の family の status pool の begin・end は `EIO`（`i915_command_result` が DEVICE_LOST に写すのは ETIMEDOUT・EIO だけ、EBADMSG は INITIALIZATION_FAILED になるので使わない。video の命令の D18 と同じ）。video の family で occlusion の pool の begin・end も拒否 | 規格の VUID に沿う。誤った使い方で GPU や CPU が他の pool を書かない |
| S5 | libvulkan: video の decode の family で `queryResultStatusSupport` を TRUE。status pool は result の語 1 つ（status）を持つ pool として作り（`status_only`）、`vkGetQueryPoolResults` は status pool では各 query に `available ? status : 0` を必ず書く（PARTIAL の有無に依らず、32 bit では下位 32 bit = 符号つきの値）。pNext の profile は送らない（実行器は profile を見ない、family と同じ codec しか無い） | 規格の WITH_STATUS の書き方。wire は変えない |
| S6 | `vkvideo-probe` は family が TRUE なら 1 query の pool を作り（作れなければ query 無しで続ける）、decode ごとに reset（scope の外）→ begin coding →（control）→ begin query → decode → end query → end coding、wait の後に WITH_STATUS で読み、COMPLETE でなければ `vkvideo-probe: picture N status S` を出し、失敗が 1 つ以上の時だけ最後の行に `, N failed` を足して exit 5（UAT の文字列 `N frames decoded, N match the reference` は失敗が無ければ変わらない） | p005・p006b の実機で、hash の不一致と D17 の飛ばしを区別できる。probe の Vulkan の経路は host 試験が無く（host-vkvideo-probe は reader・hash・DPB だけ）、実機でだけ確かめる |

UAPI（gpu-op.h）・HAL・wire の形は変えない（query の op は 1.0 の物）。

### 試験（host）

- `host-libvulkan-video.c`: queue family properties2 の status が video の family で TRUE、他は FALSE。
- 新 `host-libvulkan-status.c`（query.c を transport の stand-in で）: status pool の create の byte 列（type 1000023000）、results: available の −1 を 32・64 bit で、unavailable は 0 と `VK_NOT_READY`、COMPLETE、stride の padding を保つ。
- 往復（`host-video-wire.c`・`host-video-executor.c`）: status pool の作成（video の device だけ）、reset → begin → 通る decode → end で COMPLETE、飛ばす decode で ERROR、results の reply、scope の外の begin・active のまま end・二重 begin・occlusion の pool の begin は DEVICE_LOST、graphics の family の status pool の begin は DEVICE_LOST。

## 記録

- 2026-10-08 午後 P2: 設計（上）。design-reviewer の review（should-fix 6・minor 5、blocking 無し）を反映: F1 graphics の family は EIO（S4）、F2 64 bit の符号の書き方を明記し 32 bit・64 bit の両方を試験、F3 reset の範囲と scope の外を模擬で、F4 1 query に decode 1 つ、F5 不変条件を S2 と walk に、F6 同じ submit の中の reset の順を試験（render の batch で reset すると host でも NOT_READY にならず落ちる形）、F7 type の field と CPU の分岐を batch の前に、F8 ENOMEM の途中適用を S3 に、F9 design.md の D15・§3.2・§13、F10 probe の `, N failed` は失敗がある時だけ・begin query は control の後・pool が作れなければ無しで、F11 profile を見ない事を利用者への案内の「規格との違い」に。
- 実装（2026-10-08 午後 P2）:
  - libvulkan: `query.c`（status pool の `status_only`、`vkGetQueryPoolResults` は各 query に `available ? status : 0`）、`external-properties.c`（decode の family で `queryResultStatusSupport` TRUE）。
  - 実行器: `render/fence.c`・`fence.h`（type 1000023000 を video の device だけ、pool の `type`、status pool の reset を CPU で、begin・end は EIO、`drv_i915_gfx_query_status_pool`・`drv_i915_gfx_query_in_range`・`drv_i915_gfx_query_status_end`）、`render/video.c`（walk の S4 の検べ、skip で ERROR、END で CPU が書く）。
  - `userland/tests/vkvideo-probe/main.c`（S6）。
  - 利用者への案内: `docs/reference/vulkan-video.md`（新、`docs/reference/README.md` から）: 使える条件（native の i915、VCS0、`i915.debug=video`）、拡張、queue family、decode できる形と上限、NV12 と plane の読み方、飛ばす decode と result status、DEVICE_LOST の場合、規格との違い、`vkvideo-probe`。

## 確認（host・build、2026-10-08 P2）

| コマンド | 結果 |
| --- | --- |
| `sh plan/ws083/tests/run-host-libvulkan-status.sh`（新、query.c） | plain・ASan/UBSan PASS: status pool の create の byte 列（type 1000023000）、32 bit で ERROR・COMPLETE・unavailable は 0 と `VK_NOT_READY`・stride の padding を保つ、64 bit の −1・1、occlusion は今まで通り |
| `sh plan/ws083/tests/run-host-libvulkan-video.sh` | PASS（queue family の status: video の family TRUE、graphics の family FALSE） |
| `sh plan/ws083/tests/run-host-video-roundtrip.sh` | PASS（plain・ASan/UBSan、genxml 36 instruction 89 check）。追加の `test_status_queries`: 新しい pool は NOT_READY、IDR と P の decode で COMPLETE 2 つ、video の family の reset だけの submit で NOT_READY に戻る、start code の無い slice で IDR が ERROR（64 bit で −1、32 bit で 0xffffffff）、scope の外の begin・scope で終わらない query・2 つ同時・scope の中の reset・別の query の end・occlusion の query・1 query に decode 2 つ・pool を越える reset は DEVICE_LOST で何も走らない（status は前のまま）、同じ submit の end の後の reset で NOT_READY、graphics の family で reset は走り begin は DEVICE_LOST |
| `sh plan/ws031/tests/run-vk-host-tests.sh` | 10 個 PASS（fence.c の変更の回帰） |
| `sh plan/ws083/tests/run-host-vkvideo-probe.sh` | PASS（reader・hash・DPB、probe の Vulkan の経路は対象外） |
| `make -j16 BUILD=build/p2-k ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_USER_PROGRAMS="libvulkan vkvideo-probe" build/p2-k/bin/vkvideo-probe build/p2-k/dynamic/libvulkan.so build/p2-k/vmunix` | 成功 warning 0（amd64 vmunix check PASS） |

未実施: 実機（status が実機の decode で COMPLETE になること、D17 の飛ばしで ERROR になることは p005・p006b の T1 の依頼で probe が自動で見る）。QEMU は門が閉じているので status の経路は通らない。

## 残り

- 性能（1080p の decode の時間、U6・U8）、D19 の既定化（`i915.debug=video` を外す）: 実機の p005・p006b・p007 の後。

## 2026-10-09 夜 P1: 性能の測りの道具と門（ベータ2 で OFF にできるか）

### 性能の測り（U6・U8 の材料）

- `vkvideo-probe --time`（新）: 各 decode を、command の記録から fence まで（frame の hash は数えない）時計（CLOCK_MONOTONIC）で測り、最後に `vkvideo-probe: decode time: N decodes, total T ms, mean M ms, longest L ms`。`docs/reference/vulkan-video.md` の Example program に 1 段落。
- 確かめ: `make … "ZEDBSD_USER_PROGRAMS=libvulkan vkvideo-probe" build/p1-ws083/bin/vkvideo-probe` rc 0・warning 0、`run-host-vkvideo-probe.sh` PASS（reader・hash・DPB。probe の Vulkan の経路と option の解析は host 試験の外）、style-check の新しい指摘 0。
- 実機の手順（T1-435 に足す文の案）: `vkvideo-probe --time --expect=… /tmp/v/pb-high-352-pyramid.h264`、1080p は `/home/awe/zedbsd-media/sample-h264-{main,high}.h264` を `--time --frames=60`（hash は見ない）。数字を返す。参考の目安: 1080p で mean が 33 ms 以下なら 30 fps に足りる（probe は 1 decode ごとに submit と wait をするので、実の再生より遅く出る）。

### 門（2026-10-09 Q1「直前に OFF にできる門があることを確かめ、OFF の手順を」）

- 今の門: kernel は boot の parameter `i915.debug` に `video` がある時だけ video decode を出す（`src/drivers/gpu/i915/device.c` の `i915_boot_word_listed(…, "video")` → `drv_i915_render_video_request`、`render/vulkan.c` の capset）。既定は OFF。
- release の config（`config/release/config-amd64-beta2.mk`）には `i915.debug=video` が**無い**。つまり今の release の image は Vulkan Video が OFF。
- ON にする（T1-435 の A〜C と p007 の F1 が PASS した後、Q1・ユーザーの判断）: release の config に 1 行 `ZEDBSD_BOOT_EXTRA_LINES += i915.debug=video`（boot の cfg の名前に語が入るので image が作り直される、`platform/amd64/vmunix.mk` の AMD64_BOOT_EXTRA_TAG）。kernel の変更は要らない。
- OFF にする（直前でも）: その 1 行を消す（入れていなければ何もしない）。利用者の手元では、USB の FAT の partition の `ZEDBSD.CFG`（1 行 1 parameter）に `i915.debug=video` の行を足す・消すだけで切り替わる。
- **D19 の既定化（kernel の既定を ON にする）はベータ2 ではしない**のを推す: 実機の p005・p006b・p007 が済んでおらず、既定を ON にするなら OFF の parameter（例 `i915.video=off`）も要る。ベータ3 で。
- 注意（Q1・ユーザーへ）: release の image の中に Vulkan Video を使う program は無い。Video Player は libmedia → FFmpeg で CPU で decode し（`userland/desktop/libmedia/avcodec.c`）、FFmpeg は `--disable-hwaccels` で build される（`userland/packages/multimedia/libavcodec/Makefile`）。vkvideo-probe は試験の image だけ。だから beta2.md の UAT の 9（Video Player で H.264 の mp4）は WS083 を通らず、門の ON・OFF で結果は変わらない。WS083 の実機の確かめは T1-435（5330 の passthrough の vkvideo-probe）と p007 の F1・F2。release の門を ON にしても、利用者に見える違いは他の program が Vulkan Video を使う時だけ。

## 2026-10-10 P2（WS083 の完了の段、ユーザー「P2を立ててWS083 Videoを完了しましょう。」）

design §9 の p008 の受け入れ（記録、ws.md の制限）の項目ごとの状態:

| 項目 | 状態 |
| --- | --- |
| result status query | 済み。host（2026-10-08、上の表）に加え、5330 の実機（2026-10-10 Q1、image 588c5cd、`i915.debug=video`）で 6 本の stream（I 3 本・P/B 3 本）の全 picture が COMPLETE: probe は family の `queryResultStatusSupport` が TRUE なので decode ごとに query を読み、COMPLETE でない picture があれば `, N failed` と exit 5、pool が作れなければ `no result status query` の行を出す。Q1 が全出力を見直し、どちらの行も無い（出力は capabilities・session・parameters・`N frames decoded, N match the reference` の 4 種だけ）。ERROR（D17 の飛ばし）の実機は未実施（正しい stream では起きない。host の roundtrip の `test_status_queries` で確かめた） |
| 利用者への案内 | 済み。`docs/reference/vulkan-video.md`（2026-10-08）の Availability に、既定は OFF（release の image も）と `zedbsd.cfg` に `i915.debug=video` の行で ON（既に `i915.debug=` の行があれば語を足す、`display,video`）を足した。release notes（`docs/release/zedbsd-1.0.0-beta2.md` の Hardware）の Vulkan Video の行を「既定は OFF、USB の EFI partition（FAT）の `zedbsd.cfg`（`ZEDBSD.CFG`）に `i915.debug=video` の行を足して起動し直すと ON、行を消すと OFF、image の中の program は要らない（Video Player は FFmpeg で CPU）」に書き換え、review の comment をユーザーの決定（2026-10-10、release は OFF）に直した |
| D19 の門の既定化 | **既定は OFF で閉じる**（2026-10-10 ユーザーの決定: release の config に `i915.debug=video` を入れない。使う program は vkvideo-probe だけ、利用者は `zedbsd.cfg` で ON。Video Player が使うようになったら ON を改めて判断）。kernel の既定を ON にする変更（と OFF の parameter）は作らない。改めて ON にする時は p007 の実機（F1・F2）の後に別の Phase で |
| SAMPLED・TRANSFER_SRC | 要らない（HD5、2026-10-07 ユーザー） |
| 性能（U6・U8） | **残り**: T1-435 の E（5330、`vkvideo-probe --time`、B・C の 6 本と 1080p の sample 3 本の `--frames=60`）。数字が届いたらこの節と ws.md の制限に書いて cleared 候補 |
| ws.md の制限 | ws.md の「制限（p008）」の節に書いた |

確認: 文書だけの変更（code は変えない）。`docs/` から `plan/` への link は足していない。

## 2026-10-10 性能（T1-435 の E・E2、5330 の実機、image 588c5cd、ESP は書いていない）

| stream | decodes | total ms | mean ms | longest ms |
| --- | --- | --- | --- | --- |
| i-baseline-64 | 3 | 2 | 0.666 | 1 |
| i-main-352-slices | 3 | 2 | 0.666 | 1 |
| i-high-352-cqm | 3 | 2 | 0.666 | 2 |
| p-baseline-64 | 10 | 8 | 0.800 | 5 |
| pb-main-352 | 15 | 14 | 0.933 | 2 |
| pb-high-352-pyramid | 15 | 16 | 1.066 | 2 |
| sample baseline（--frames=60、hash は見ない） | 60 | 250 | 4.166 | 5 |
| sample main | 60 | 304 | 5.066 | 6 |
| sample high | 60 | 309 | 5.150 | 8 |

全 stream が match。E2: 後も compositor が居て、vcs0・rcs0 の hang・reset の行は無い。sample（1080p 相当）の 1 frame 約 4〜5 ms で、60 fps（16.7 ms）に十分な余裕。
