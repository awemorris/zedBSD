<!-- awesome-plan project=zedbsd record=ws083-p002 -->

# ws083-p002: libvulkan の Vulkan Video の骨組み

Status: in-progress（2026-10-08 q902 P1 の照合: §8.2 の QEMU 回帰は T1-371 PASS。実機の family・拡張の列挙は T1-435 の A（未実行））（旧: in-progress（q833、P1。2026-10-07 夜 段 1〜3 の実装と host 試験済み、QEMU の回帰は p004 の受け入れで T1））
Disposition: normal
Parent: [WS083](../ws.md)

## 範囲（Q1 の ACK 2026-10-07、ユーザーの判断 H1・H2・H3・HD1・HD6 の後）

1. `include/uapi/gpu-op.h` に承認済みの `proposed/gpu-op-video.diff` を当てる（H1）。
2. `include/libc/vulkan/vulkan_video.h`（`tools/maintain-video.noct` で 1.4.309 の header から選ぶ）と `vk_video/` の 3 header、API-PROVENANCE・LICENSE-API（H3）。
3. libvulkan: capset の native の語（HD1）、拡張の列挙と照合、queue family の濾しと video の properties、external-properties の pNext と format の feature、`video.c`、`sync2.c`（H2）、codec・dispatch の生成し直し、README の非適合（HD6）。
4. host の試験 `plan/ws083/tests/host-libvulkan-video.c`、build warning 0、export の数。

kernel（boot.c）と i915 は触らない（p003b）。toolchain は触らない。

## 記録

- 2026-10-07 夕 段 1: `gpu-op.h` に diff を当てた（`git apply` そのまま、版 2、`0x10000`〜`0x1000d`）。`make BUILD=build/p1-k … vmunix dynamic/libvulkan.so` 成功 warning 0。
- 2026-10-07 夕 段 2: `include/libc/vulkan/vulkan_video.h`（`userland/desktop/libvulkan/tools/maintain-video.noct` で 1.4.309 の header から: 4 拡張の block、19 command、Vulkan 1.3 の 8 構造体・6 typedef・146 の stage/access の定数）、`vk_video/` の 3 header（package から無変更で複写、SHA は API-PROVENANCE）、`vulkan.h` が `vulkan_video.h` を include、API-PROVENANCE.md の節。確認: host の gcc（`-std=c99 -pedantic -Werror`）と target の clang で `<vulkan/vulkan.h>` の video・sync2・StdVideo の型と関数の宣言を使う 1 file が通る。`make … vmunix dynamic/libvulkan.so bin/wayland bin/vkdemo dynamic/libGLESv2.so` 成功 warning 0。

- 2026-10-07 夜 段 3（P1 の新しい世代）: libvulkan の C。
  - `context.c`: capset が 168 か 176 byte の時に vendor 部を読み（flags の完全一致は不変）、176 byte で byte 168 が `0x5a4e4154` かつ byte 172 の bit 0 の時だけ `context->video_h264`。
  - `internal.h`: object kind `VIDEO_SESSION`・`VIDEO_SESSION_PARAMETERS`、拡張の bit 256・512・1024・2048、`VULKAN_VIDEO_QUEUE_FLAGS`・`VULKAN_VIDEO_FORMAT_FEATURES`（encode の bit は固定した 1.3.269 の header で beta なので値で）、physical に family ごとの codec ops、context に `video_h264`、`vulkan_command_record_begin/finish`（commands.c の記録の framing の公開の包み）、`vulkan_video_profile_list_check`。
  - `instance.c`: D3（video の語の無い session は family の video の flag と format の video の feature を隠す）、`physical_load_video`（`GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_VIDEO_PROPERTIES` を 1 回、H.264 decode の family が 1 つも無ければ拡張を出さず flag も隠す）、4 拡張の列挙。`device.c`: 4 拡張の照合と依存（sync2 は instance の properties2、video_queue は sync2、decode_queue は video_queue、h264 は decode_queue）。
  - `external-properties.c`: features2 の sync2、queue family properties2 の `VkQueueFamilyVideoPropertiesKHR`・`VkQueueFamilyQueryResultStatusPropertiesKHR`（FALSE）、image format properties2 の `VkVideoProfileListInfoKHR`（profile を検べ、usage ⊆ {DST, DPB}・NV12・2D・OPTIMAL・external 無しなら backend の 1.0 の問い合わせへ）。
  - `video.c`（新、13 entry point）: profile の検べ（op・format・layout・codec の順の規格の error）、caps（出力の chain の形を送り入れ子で受ける）、format・memory requirements（32 個まで一度に、手元で切り詰め `VK_INCOMPLETE`）、session・parameters の create/destroy、bind、parameters の key（SPS 32・PPS は (sps, pps) の 8192 bit、update の重複と範囲外の id は送る前に拒む、create は template の key を置き換えてよい）、4 つの記録。構造体は手で encode（§4.2 を本文に合わせて直した: 数の field は u32 で位置に、配列は `[u64 数]`、flags は名前で bit 0 から、VUI は送らない）。
  - `sync2.c`（新、6 entry point）: stage は 1.0 の bit をそのまま・無い bit は ALL_COMMANDS・NONE は TOP（src）/BOTTOM（dst）、access は 1.0 の bit をそのまま・無い bit は MEMORY_READ|WRITE、barrier は 16 個ずつ、WaitEvents2 は event ごと、QueueSubmit2 は 1 つの block の配列に。
  - 生成: `maintain-dispatch.noct` が `vulkan_video.h` も読み 173 → 192（`dispatch-table.inc`・`api-commands.tsv`）。`maintain-codec.noct` は入力が core のままで出力は同じ（cmp 一致、100 encoder・47 decoder）。U13: export の数の検査は `maintain-dispatch.noct` の件数の検査だけ（ELF の checker は別に無い）。build の `libvulkan.so` の `vk*` の export は 192 で manifest と一致。
  - `README.md` に N1〜N4 と key の error、`API-PROVENANCE.md` に C99/GNU89 の要件・192・video の構造体は手で encode。
  - design.md からの変更: key の重複の error は `VK_ERROR_INITIALIZATION_FAILED`（`VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR` は固定した header で beta の encode の値）、video の構造体は codec の生成でなく `video.c` に手で（chain・bit field・2 次元の scaling list に規則が要る）。§3.5・§4.2 を直した。

## 確認（段 3）

| コマンド | 結果 |
| --- | --- |
| `sh plan/ws083/tests/run-host-libvulkan-video.sh`（新、video.c・sync2.c・external-properties.c・objects.c・wire.c・codec.c を gnu89 `-Wdeclaration-after-statement -Werror` で、transport と 1.0 の command を stand-in に） | plain・ASan/UBSan とも PASS: profile の 6 つの拒否（transport に届かない）、caps の要求の byte 列と入れ子の返事の decode（caller の chain の link を保つ）、format の count・`VK_INCOMPLETE`・usage の拒否、session の create の byte 列（profile・header 版・identity）・memory requirements の切り詰め・bind・destroy、parameters の create の byte 列（SPS の flags bit 0・8・15、負の値、cycle の offset、非対称の scaling list、PPS の符号）、update の key の重複・範囲外は送らずに拒否・新しい key は送って保持・template の key を引き継ぐ、Begin の 3 つの形（slot あり・slot −1・resource NULL）・Control RESET・Decode（H.264 の picture・2 slice・setup の slot）・End、sync2（video decode の stage・access の広げ、barrier 無しの実行依存、20 個の image barrier の 16＋4 の分割、Set・Reset・Wait（event ごと）・Timestamp・Submit2 の 2 batch）、features2・queue family properties2・image format properties2（profile list の受理と 4 つの拒否）。期待値を 1 つ変えた変異は位置を出して abort（検出を確認） |
| `make -j16 BUILD=build/p1-k ZEDBSD_CONFIG=config/ci/config-amd64.mk build/p1-k/dynamic/libvulkan.so build/p1-k/bin/wayland build/p1-k/bin/vkdemo build/p1-k/dynamic/libGLESv2.so` | 成功、warning 0 |
| `llvm-nm -D --defined-only libvulkan.so` の `vk*` と `api-commands.tsv` | 192 で一致 |

未実施: context.c の native の語の読み（ioctl の要る open は host の試験の外、168・176 の分岐は読みで確認）、instance.c の D3 の濾しと `physical_load_video`（host の試験の外、build と読みで確認）、QEMU の回帰（design §8.2 は p004 の受け入れ、T1）、実機。

気づいた事（範囲の外、Q1 へ）: 段 2 で `vulkan.h` が `vulkan_video.h`（Khronos の行 comment を含む）を include するので、`-std=c89` で `vulkan.h` を読む `plan/ws014/tests/run-libvulkan-external-properties-test.sh` は compile できない（master の試験の一覧に無い開発の試験）。

## 再開の情報（段 3 から、済み）


段 3（libvulkan の C、design.md §3〜§5・§4.2・§4.3）の順:
1. `context.c`: capset の native の語（`bytes == 168 || bytes == 176` で vendor 部、176 かつ byte 168 が `0x5a4e4154` の時 byte 172 の bit 0 → `context->video_h264`）。
2. `internal.h`: object kind 2 つ、拡張の bit 4 つ（256・512・1024・2048）、physical に family の codec ops。
3. `instance.c`・`device.c`: `video_h264` の時だけ 4 拡張を列挙・照合（sync2 は `vkCreateDevice` の時に properties2 を検べる）、`physical_load_queues` の後に D3 の濾しと `GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_VIDEO_PROPERTIES`。
4. `external-properties.c`: queue family properties2 の pNext、features2 の sync2、image format properties2 の profile list、format の feature（D23）。
5. `video.c`（13 entry point、§3.3〜§3.6、record は §4.2）、`sync2.c`（6、§3.7）。
6. `tools/maintain-codec.noct`・`maintain-dispatch.noct` で codec と dispatch を生成し直す（173 → 192）、`Makefile`・export の数の検査（所在は U13）。
7. README の非適合 N1〜N4。
8. host の試験 `plan/ws083/tests/host-libvulkan-video.c`（transport の stub）。

## T1-351 の判定（2026-10-07 Q1）

T1-348 の C（i915.debug=video）・D（既定）の zdesktop の capture を 5330 の passthrough で流し直した（capture.c の build error は be2544532 で直した）。sheet.png の desktop は C・D とも普段どおり → 門（video）を入れても desktop は変わらない、の受け入れは満たす。result.json の `wiseview_closes`・`close_ends_viewer` が C・D とも false（門に依らない）。WS181 の Wiseview・App Home の作り直しに capture の scenario（plan/ws031/tests/i915-capture.py）が追いついていない疑い。scenario を使う時に、試験の整理の基準で直すか削除する。C の guest の video の行の確認は未実施。
