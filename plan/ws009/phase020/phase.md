<!-- awesome-plan project=zedbsd record=ws009-p020 -->

# ws009-p020: DOC-54 gpu-command-stream.md と gpu-sysctl.md

Status: planned（2026-10-30 P1 の設計、q1701。p009 を節ごとに分けた物）
Parent: [WS009](../ws.md)
Approval: 2026-10-27 ユーザーの回答（クリック）「今書く」（DOC-54）。2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」
実装の担当: Sonnet 5.5 high（1 回の起動）
依存: なし（骨格 4dfcce008 と照合の script が main にあること）。p010〜p021 は互いに独立で、並べて起動してよい
骨格の基点: 4dfcce008

## 1. 読む物

- AGENTS.md の §4（担当の共通の規則）・§7（禁止）・§10（管理情報を書かない）。
- [docs/style.md](../../../docs/style.md) と [gpu-doc-rules.md](../gpu-doc-rules.md)（書き方・形・確かめ・終わりの条件の共通の規則。この phase.md と同じ重さの手順）。
- 自分の file の骨格の全文（各節の `<!-- spec: -->` が書く事の仕様）と [gpu.md](../../../docs/reference/gpu.md) の骨格（共通の規約の spec）。
- 着手の時に gpu-doc-rules.md 1 節の `git log 4dfcce008..HEAD -- ...` で基点の後の UAPI の変化を見る。
- include/uapi/gpu-op.h の先頭の comment（1〜180 行あたり）と `enum gpu_op` の要素の名前（例に出す物だけ）
- userland/desktop/libvulkan/context.c `vulkan_context_transaction`・`vulkan_context_poll`、libvulkan/wire.c `vulkan_command_begin`・`vulkan_command_execute`
- include/uapi/sysctl.h の hw.gpu.*・hw.gputelemetry の comment と構造体、kernel の handler（`grep -rn 'HW_GPU_\|HW_GPUTELEMETRY' src`）、telemetry を埋める driver（grep）
- graphical login が hw.gpu.attaching を使う所（`grep -rn 'gpu.attaching\|HW_GPU_ATTACHING' userland`）

## 2. 作る物

- `docs/reference/gpu-command-stream.md`
- `docs/reference/gpu-sysctl.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 表の Meaning の列（あれば）を全行。
- 節ごとの主張の表と、3 つの読み直し（gpu-doc-rules.md 4 節の要求の節の無い file の形）。
- 出典にする試験の見当: この 2 file は要求の節を持たない。

## 4. 注意

- command の stream は形だけを書き、命令ごとの表を写さない（header が命令ごとの reference）。
- Venus の版は header の comment どおりに名指す（virglrenderer 1.1.0、venus-protocol の commit）。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-command-stream.md docs/reference/gpu-sysctl.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu-command-stream.md docs/reference/gpu-sysctl.md plan/ws009/phase020/phase.md plan/ws009/ws.md`。

## 結果

日付: 2026-10-30（host の `date`）。書いた物: `docs/reference/gpu-command-stream.md`（74 行）、`docs/reference/gpu-sysctl.md`（114 行）。状態: 書いた・照合 PASS（cleared は Q1 が決める）。

### 実行した command と最後の行

- `git log --oneline 4dfcce008..HEAD -- include/uapi/gpu*.h include/uapi/sysctl.h include/uapi/errno.h src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c userland/desktop/libvulkan/context.c` -> 出力なし（基点の後の UAPI・gpu.c・gpu-fence.c・libvulkan の変化なし）。
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-command-stream.md docs/reference/gpu-sysctl.md` -> `check-gpu-uapi-doc: PASS (14 files, 46 index rows, 46 request sections, 98 constant rows, 47 structures, 20 capset rows, 656 placeholders, final 2 files)`
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008` -> `check-gpu-uapi-doc: PASS (14 files, ..., 656 placeholders, final 0 files)`（他の file の PENDING は数えるだけ）
- `noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture` -> `Markdown relative-link check: PASS (497 links)`
- `git diff --check` -> 出力なし（exit 0）

### 自分で決めた点

- Executors の表は、header の comment（i915 と bcm2711 だけ）でなく code に従い、kernel で decode する 7 つの driver（amdrdna・bcm2711・ghci・i915・mali・nvrtx・powervr、それぞれ vk/ の stream の executor と dispatch が ENOTSUP を返す）を 1 行にまとめ、venus を別の行にした。
- header の flag の語は code から書いた: amdrdna・bcm2711・ghci・mali・nvrtx・powervr は 0 と 1 以外の flag を EINVAL で拒み、i915 は 0 以外を全て reply の要求に数える。
- video の command を decode する driver は i915・nvrtx・bcm2711（CONFIG_DRIVER_BCM2711_VIDEO の時）と書いた（header の「native i915 だけ」でなく code の route から）。
- i915 が native batch（先頭の語が i915 の magic の stream）も受けることを 1 段落で書いた（形式は書かない、この protocol の外）。
- libvulkan の送り方の分岐（GPU_COMMAND で試す条件、EAGAIN での GPU_COMMAND_SUBMIT、通知なしの GPU_COMMAND）、trailer の待ち（通知あり: 完了の後に trailer が揃わなければ即 device lost、なし: 1 ms ごとに 10 秒まで）、ENOMEM・ENOSPC だけが out-of-memory の結果になること、を context.c から書いた。header は「trailer の最後の語を poll する」とだけ言う。
- hw.gpu.attaching の使い手は sessiond（userland/desktop/sessiond）で、count が 0 でない間は最大 120 秒、display が既にあっても 0 になるまで待つ（sessiond_display_next）。sysctl.h の comment（「/dev/gpu0 を待つのは count が 0 でない間だけ」）より code は少し細かい。
- hw.gpu.start を持つ driver は i915（i915.start=manual、amd64）と bcm2711 の Raspberry Pi 5（CONFIG_DRIVER_BCM2712_GPU、rpi5gpu.start=boot で boot 時に開始）。header の comment は i915 だけを挙げる。
- hw.gputelemetry の driver ごとの valid の表は 3 つの reader（i915・powervr・bcm2711 の v3d）の code から書いた。他の driver は telemetry を登録しない（kern_gpu_telemetry_register の呼び出しは 3 箇所だけ）。
- Variables の下の小見出しは H5（`#####`）にした。

### header・comment と code の食い違い

1. `include/uapi/gpu-op.h` 冒頭: 「i915 Vulkan executor と bcm2711 (V3D) Vulkan executor が読む」と言うが、同じ番号の stream を kernel で decode する driver は amdrdna・bcm2711・ghci・i915・mali・nvrtx・powervr の 7 つ（src/drivers/gpu/*/vk/ の vk-stream.c・vulkan-stream.c）。
2. 同じ comment の file 名は古い: `i915/transport.c`・`i915/dispatch.c` などは実在せず、実体は `src/drivers/gpu/i915/vk/vk-transport.c`・`vk-dispatch.c`（`vk-` の付いた名前）。`bcm2711/vulkan-stream.c`・`vulkan-dispatch.c` は `src/drivers/gpu/bcm2711/vk/` にある（comment は `src/drivers/gpu/bcm2711/`）。確かめたのは transport・dispatch・stream の 4 file だけで、他の file 名は確かめていない。
3. gpu-op.h の video の節の comment「native i915 だけ」「bcm2711 has no route for any of them」「Venus host には送らない」: code では nvrtx（vk-video.c の route）と、CONFIG_DRIVER_BCM2711_VIDEO の時の bcm2711（vulkan-video.c の `bcm2711_vulkan_video_dispatch`）も video の command を decode する。送るかどうかは libvulkan が capset の video の bit だけで決め、Venus かどうかは見ない。
4. `include/uapi/sysctl.h` の hw.gpu.start の comment は i915 だけを挙げ、bcm2711（Raspberry Pi 5）の start を書かない。
5. 食い違いではない観察（Bug の候補）: `userland/base/sysctl/main.c` の `show_name` の汎用の経路は `uint64_t value` を初期化せず 8 バイトの長さで `sysctlbyname` を呼ぶ。hw.gpu.attaching は 4 バイトなので、`sysctl hw.gpu.attaching` の表示の上位 32 bit は未定義になりうる（実行して確かめていない）。文書は `sysctlbyname` を 4 バイトの buffer で呼ぶと書き、この command では勧めていない。

### 自己の照合: 節ごとの主張の表

| 主張（節と要約） | 根拠（file と関数） | 試験 |
| --- | --- | --- |
| command-stream 冒頭: 145 + 14 の command、番号 0〜180 と 0x10000〜 | gpu-op.h の enum（`GPU_OP_` の要素 159 のうち 0x の物が 14） | 無し |
| Protocol version: PROTOCOL_VERSION を比べる code は無い | grep（`GPU_OP_PROTOCOL_VERSION` は gpu-op.h の定義だけ）、context.c は wire_version == 1 と xml_version を見る | 無し |
| Wire form: flag は 0 か 1、他は拒否（i915 は非 0 を要求に数える） | amdrdna・nvrtx・ghci・powervr・mali の vk-stream.c、bcm2711/vk/vulkan-stream.c `execute_command`、i915/vk/vk-dispatch.c `drv_i915_render_dispatch` | 無し |
| Wire form: reply の先頭の語が送った番号でなければ device lost | libvulkan/wire.c `vulkan_command_execute` | 無し |
| One submission: SET_REPLY の 36 バイト、SEEK、version の trailer の 20 バイト | context.c `vulkan_encode_reply_stream`・`vulkan_encode_reply_trailer` | venus-frame（client.c が SET_REPLY・SEEK を送る） |
| One submission: GPU_COMMAND_MAX - 128 を越えると外の blob と EXECUTE_STREAMS | context.c `vulkan_context_transaction`・`vulkan_encode_external_stream` | 無し |
| One submission: GPU_COMMAND、EAGAIN、GPU_COMMAND_SUBMIT の分岐 | context.c `vulkan_context_transaction`、`vulkan_context_open`（sync_command） | 無し |
| One submission: trailer の待ち（通知あり・なし、10 秒、1 ms） | context.c `vulkan_context_poll`・`VULKAN_TRANSPORT_TIMEOUT_NS` | 無し |
| One submission: ENOMEM・ENOSPC だけが out-of-memory、他は device lost で session に残る | context.c `vulkan_kernel_error`、objects.c `vulkan_context_error`、context.c `vulkan_context_execute` | 無し |
| Executors: 未知の command は ENOTSUP で stream が止まる | 各 driver の dispatch（bcm2711 `bcm2711_vulkan_dispatch`、powervr・mali の `dispatch`、amdrdna・nvrtx・ghci の vk-dispatch.c、i915 `i915_dispatch_builtin`） | 無し |
| Executors: venus は長さが 4 の倍数かだけを見て SUBMIT_3D（0x0207）で送る | venus.c `venus_command_submit`、transport.c `drv_venus_transport_submit` | venus-frame |
| Executors: GPU_COMMAND は 4 の倍数でなければ EINVAL | gpu.c `gpu_command_ioctl` | 無し |
| Executors: video は i915・nvrtx・bcm2711 | i915/vk/vk-video.c、nvrtx/vk/vk-video.c、bcm2711/vk/vulkan-video.c と vulkan-dispatch.c の routes | 無し |
| Variables: 読みの protocol（長さだけ・ENOMEM・EINVAL）、書きは EPERM | sysctl.c `sysctl_output`・各 leaf の handler | gpu-selftest（hw.gpu.attaching・stages を読む） |
| Variables: hw.gpu.start の EPERM・EINVAL・ENODEV | sysctl.c `sysctl_gpu_start`、i915/device.c `i915_start_release`、bcm2711/attach71.c `start_engine` | 無し |
| Variables: hw.gpu.attaching の使い手と 120 秒 | sessiond/main.c `main_wait_display`・`MAIN_DISPLAY_WAIT_MS`、fallback-rules.c `sessiond_display_next` | 無し |
| Variables: stages の text の形 | powervr/stage.c `powervr_stage_text`、stage-log.c | gpu-selftest（text を読む） |
| Telemetry: 最大 8、失敗した GPU は省く、header 24・entry 72 | sysctl.c `kern_gpu_telemetry_register`・`sysctl_gputelemetry` | 無し |
| Telemetry: driver ごとの valid | i915/gt-power.c `drv_i915_rps_telemetry_read`、powervr/render.c と bcm2711/render-device.c の `render_telemetry` | 無し |

### 読み直し 3 件

1. hw.gpu.start の write: `sysctl_gpu_start` は newp が NULL で newlen が 0 でなければ EINVAL、非 superuser は EPERM、newlen が 8 でなければ EINVAL、値が 1 でなければ EINVAL、gpu_start_ops が NULL なら ENODEV、その後 driver の start の戻りが非 0 ならそれを返す。文書の 4 つの項と合う（順序も同じ）。
2. 外の blob の閾値: `vulkan_context_transaction` は `writer->bytes > GPU_COMMAND_MAX - 128` で外の blob に写し、`vulkan_encode_external_stream` が EXECUTE_STREAMS を書く。枠の全体が GPU_COMMAND_MAX を越えれば VK_ERROR_OUT_OF_HOST_MEMORY。文書の「GPU_COMMAND_MAX less 128 bytes」と合う。
3. telemetry の省略: `sysctl_gputelemetry` は `source->read` が非 0 の GPU を数えず、header の count を数えた物にして長さを `*oldlenp` に入れる。長さの問い合わせは登録された全ての GPU 分を返す。文書の「covers every listed GPU」「a successful read stores the length of the entries that are present」と合う。

### 未確認・未実施

- 文書だけの変更なので build と QEMU・実機の試験は未実施。telemetry の値・hw.gpu.start の実機の挙動は観測していない。
- i915 の GPU_COMMAND_SUBMIT で 4 の倍数でない長さを executor が拒否するかは確かめていない（文書は GPU_COMMAND の kernel の検査だけを書いた）。
- Venus の host の側が未知の command を返す結果は zedBSD の code にないので書いていない。

