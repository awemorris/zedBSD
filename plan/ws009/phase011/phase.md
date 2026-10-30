<!-- awesome-plan project=zedbsd record=ws009-p011 -->

# ws009-p011: DOC-54 gpu-capabilities.md: capability の bit・GET_INFO・GET_CAPSET・capset 4 の形

Status: planned（2026-10-30 P1 の設計、q1701。p009 を節ごとに分けた物）。実装の担当は docs/reference/gpu-capabilities.md を書いた・照合 PASS（2026-10-30。cleared は Q1 が決める）
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
- src/drivers/gpu/gpu.c: `gpu_info_ioctl`（capabilities は `device->ops->capabilities`）・`gpu_capset_ioctl`・`gpu_ioctl_body`（bit の無い時の errno）
- 各 driver の capset の関数（`grep -rn 'get_capset' src/drivers/gpu`）: venus/venus.c `venus_get_capset`・`venus_get_info`、amdrdna/runtime.c `runtime_capset`、nvrtx/runtime.c、powervr/render-runtime.c、bcm2711/render-runtime.c、i915/command.c `i915_get_capset`、mali、ghci/vk/vk-node.c
- userland/desktop/libvulkan/context.c の capset を読む所（`VULKAN_VENDOR_CAPSET_*`・`VULKAN_NATIVE_CAPSET_*`、`vulkan_load_word(capset.data + N)`）と各 flag の使い道
- src/drivers/gpu/venus/transport.c（strict queue が無い時の `GPU_JOB_RESERVE`・`GPU_JOB_CAPACITY` の ENOTSUP）
- bit を足す・消す所: amdrdna/node.c・amdrdna/present.c、nvrtx/node.c、powervr/render-runtime.c、bcm2711/render-runtime.c、mali/sched.c・mali/render.c、ghci/ghci.c・ghci/vk/vk-node.c（register の前か後か）

## 2. 作る物

- `docs/reference/gpu-capabilities.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 要求の節ごとに `Requires:`・本文・`Errors:`（最後に `- Other values:`）・`Sources:` の形（gpu-doc-rules.md 2 節の 6）。
- 構造体の表の Direction と Meaning を全行。定数の表の Meaning を全行。
- 自己の照合の表と、`Errors:` から 3 つの読み直し（gpu-doc-rules.md 4 節）。
- 出典にする試験の見当: gpu-admission・gpu-recovery・display-scanout（GPU_GET_INFO）。capset を直に読む試験は無い（libvulkan が consumer）。

## 4. 注意

- ユーザーの決定: 2026-10-29 ユーザーの回答（クリック、Q1 の中継）Q-D54-1「capset の形も書く」。解釈: capset id 4・version 0 の payload の形（大きさ・magic・flag の欄）を書く。
- capset の payload は public な header に無い ABI。文書が reference になる。表の名前（`VENDOR_MAGIC` など）は文書の名前で header の macro ではないと書く。
- capset の表の値は script が libvulkan と照らす。driver が書く値（例: amdrdna の flags）と libvulkan の定義が食い違えば結果の節に書く。
- bit が node の寿命の中で増えるかは p019（drivers）でも扱う。ここでは結論と、program がいつ問い直すかを書く。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-capabilities.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu-capabilities.md plan/ws009/phase011/phase.md plan/ws009/ws.md`。

## 結果

実施日 2026-10-30（host の `date`）。担当: Sonnet 5.5（worktree `s-ws009-p011`、基点 05d32e667）。`docs/reference/gpu-capabilities.md` を 297 行にした（骨格の `<!-- spec: -->` と PENDING を全て置き換え、見出しは変えていない）。

### 実行した command と最後の行

- `git log --oneline 4dfcce008..HEAD -- include/uapi/gpu*.h include/uapi/sysctl.h include/uapi/errno.h src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c userland/desktop/libvulkan/context.c`: 出力なし（基点の後の変化なし）。
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-capabilities.md`: `check-gpu-uapi-doc: PASS (14 files, 46 index rows, 46 request sections, 98 constant rows, 47 structures, 20 capset rows, 1206 placeholders, final 1 files)`
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008`: `check-gpu-uapi-doc: PASS (14 files, ... final 0 files)`
- `timeout 120 .../noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture`: `Markdown relative-link check: PASS (290 links)`
- `git diff --check`: 出力なし。

### 自分で決めた点

- `GPU_GET_INFO` は `Requires: open mode none; capability none.`、`GPU_GET_CAPSET` は `open mode none; capability GPU_CAP_CAPSET.`（`gpu_info_ioctl`・`gpu_capset_ioctl` は readable/writable を見ない。capset は handler の先頭で `GPU_CAP_CAPSET` を見て EOPNOTSUPP）。
- `struct gpu_capset` の `bytes` は入力で 0 を要求する出力なので Direction は `out`（Meaning に「入力は 0、違えば EINVAL」）。
- capset の payload の表に範囲の行を 2 つ足した（offset 8〜151 と 156〜159。libvulkan は読まない）。`timelines` の Meaning は libvulkan が 0 か否かだけを見るので「完了 timeline への対応。0 は拒否、他は受理」とし、spec の「timeline の数」とは書かなかった。
- 出典の試験: `GPU_GET_INFO` は gpu-admission/main.c、`GPU_GET_CAPSET` は venus-frame/client.c（phase.md の「4 注意」は capset を直に読む試験は無いとあるが、client.c が `ioctl(GPU_GET_CAPSET)` を呼ぶので link した）。
- 「bit は node の寿命の中で変わらない」を結論にした。全 driver の `capabilities` の代入・`|=` を grep で全部見て、全て `drv_gpu_register` の前（amdrdna は display の追加も `amdrdna_node_register` の前、i915 は `drv_i915_display_bind_ops` が register の前、ghci は `ghci_vk_attach` が register の前、venus は static const）。登録時に `gpu_ops_validate` が 1 回だけ検査する。

### source と spec・header・comment の食い違い（Q1 へ。文書は code に従った）

1. i915 の capset は `capset_id`・`capset_version` を検査せず、どの id・version にも同じ payload を返す（`i915_get_capset` → `drv_i915_render_get_capset`）。他の driver は 4・0 以外を ENOTSUP（venus は EOPNOTSUPP）で断る。spec の「全 driver は id 4 version 0 だけ」と食い違う。文書には例外として書いた。
2. venus の `venus_get_info` は JOB・JOB_CAPACITY を strict queue が無い時に外すが、`gpu_info_ioctl` が `device->ops->capabilities`（`VENUS_CAPABILITIES` は JOB・JOB_CAPACITY を常に含む）で上書きするので、`GPU_GET_INFO` では外れない。venus.c の comment（"capability queries remain available without strict queue completion"）が示す形と実際が違う。文書は実際の挙動（bit は立ったまま、`GPU_JOB_RESERVE`・`GPU_JOB_CAPACITY` が ENOTSUP）を書いた。
3. venus の kernel は host の capset が `VENDOR_BYTES`（168）の時だけ vendor suffix を見る（`venus_strict_queue_find`）。libvulkan は 168 と 176 の両方で読む。host の payload が 176 バイトで strict の flag を持つ時、libvulkan は strict と見るが kernel は `GPU_JOB_RESERVE` を ENOTSUP にする。文書に注意として書いた。
4. include/uapi/gpu-op.h の `GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_VIDEO_PROPERTIES` 付近の comment は video の capset を持つのを i915 とし「bcm2711 has no route」と言うが、code では nvrtx（`nvrtx_node_video_available`）と bcm2711（`bcm2711_vulkan_video_promise`、video 付きの build）も `VIDEO_DECODE_H264` を申告する。この文書は code に従った（header は直していない）。
5. i915 の `i915_render_capset_fill` は flags 7 を「connectivity check のための宣言」（XXX comment、契約は未実装）と注記しているが、そのまま返される。文書は返される値（7）を書いた。

### 自己の照合の表

| 主張（節と要約） | 根拠（file と関数） | 試験 |
| --- | --- | --- |
| Capability bits: 各 bit が有効にする request と bit が無い時の errno | gpu.c: `gpu_create_ioctl`・`gpu_blob_ioctl`・`gpu_transfer_ioctl`・`gpu_command_ioctl`・`gpu_present_ioctl`・`gpu_map_ioctl`・`gpu_display_ioctl`・`gpu_export_ioctl`・`gpu_import_ioctl`・`gpu_allocation_export_ioctl`・`gpu_allocation_import_ioctl`・`gpu_submit_ioctl`・`gpu_wait_ioctl`・`gpu_fence_ioctl`・`gpu_dependency_ioctl`・`gpu_commit_ioctl`・`gpu_events_ioctl`・`gpu_refresh_ioctl`・`gpu_job_reserve_ioctl`・`gpu_job_action_ioctl`・`gpu_capacity_ioctl`・`gpu_policy_ioctl` の各冒頭の bit の検査 | gpu-admission/main.c（DISPLAY_EVENTS）、他は無し |
| 規則 1: `GPU_GET_INFO` の bit は登録した mask で、driver の書いた値は捨てる | gpu.c `gpu_info_ioctl`（`information.capabilities = device->ops->capabilities`） | 無し |
| 規則 2: bit は node の寿命の中で変わらない（全て register の前） | `drv_gpu_register`・`gpu_ops_validate`、amdrdna/node.c・present.c・runtime.c `amdrdna_runtime_bind`、nvrtx/node.c、powervr/render.c、bcm2711/render-device.c、mali/render.c・sched.c、ghci/ghci.c、i915/i915.c、venus/venus.c | 無し |
| 規則 3: HOST_BLOB が無ければ flag を消す。SHAREABLE/CROSS_DEVICE との同時は先に EINVAL | gpu.c `gpu_blob_ioctl` | 無し |
| 規則 4: JOB bit が立っても venus は strict で無い host で ENOTSUP | venus/venus.c `VENUS_CAPABILITIES`・`venus_job_reserve`、venus/transport.c `drv_venus_transport_job_reserve`・`drv_venus_transport_capacity`・`venus_strict_queue_find` | 無し |
| 規則 5: bit の含意（JOB→NOTIFICATION、JOB_CAPACITY→JOB、SHARE→BLOB、MAPPING→RESOURCE/BLOB） | gpu.c `gpu_ops_validate` | 無し |
| `GPU_GET_INFO`: 入力は version・size だけ、driver_name の末尾を NUL、max_* の強制と errno（EINVAL・ENOSPC） | gpu.c `gpu_info_ioctl`・`gpu_resource_reserve`（呼び出し: create・blob・allocation import・resource import） | gpu-admission/main.c |
| `GPU_GET_CAPSET`: capacity 1〜256、bytes 入力 0、driver が bytes を決め EIO、成功時だけ copyout、id・version を kernel は解釈しない | gpu.c `gpu_capset_ioctl` | venus-frame/client.c |
| capset の driver ごとの形（表） | 各 driver の capset 関数と定数（上の Sources） | 無し |
| libvulkan の読み方（受理する値、flag の組、native の flag の使い道） | libvulkan/context.c `vulkan_context_open`、instance.c・device.c・wsi-image.c・wsi-display.c の flag の使い方 | 無し |
| venus の kernel の flag の組（3・7・15）と HOST_SCANOUT の export の条件 | venus/transport.c `venus_strict_queue_find`、venus/share.c `share_export` | 無し |

### `Errors:` から選んだ 3 つの読み直し

1. `GPU_GET_CAPSET` の `EIO`（driver の bytes が capacity より大きい）: `gpu_capset_ioctl` の `if (response.bytes > request.capacity) return EIO;` を読み直した。合う。
2. `GPU_GET_CAPSET` の `EINVAL` の 3 条件（version・size、capacity が 0 か 256 超、bytes が 0 でない）: `gpu_capset_ioctl` の三つの検査を読み直した。順序は copyin の後。合う。
3. `GPU_GET_INFO` の `EINVAL`（version か size の不一致）と、書き戻しが成功の時だけ: `gpu_info_ioctl` の検査と、copyout が最後で失敗時は呼ばれないことを読み直した。合う。

### 未確認・未実施

- 未確認: libvulkan の `vulkan_context_open` 以外（実機・QEMU）での挙動、`xml_version` 1.4.343 の host の実在、bcm2711 の video の promise の条件の細部（`bcm2711_vulkan_video_available`）、`nvrtx_node_video_available` の判定の中身、Venus host が返す capset の実際の大きさ。
- 未実施: QEMU・実機の試験（文書の作業のため）。gpu-requests.md の索引の `Open mode`・`Capability` の列（p022 が埋める。本文の `Requires:` の行を元にする）。
