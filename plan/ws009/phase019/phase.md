<!-- awesome-plan project=zedbsd record=ws009-p019 -->

# ws009-p019: DOC-54 gpu-drivers.md: driver と node の種類の表

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
- `grep -rln drv_gpu_register src/drivers/gpu`（gpu.c を除く 11 file）と各 file の ops の capabilities の値（macro は数値まで辿る）
- bit を足す・消す所: amdrdna/node.c・amdrdna/present.c、nvrtx/node.c、powervr/render-runtime.c、bcm2711/render-runtime.c、mali/sched.c・mali/render.c、ghci/ghci.c・ghci/vk/vk-node.c、ほか grep `capabilities |=`・`capabilities &=`・`capabilities =`
- i915 の display の 3 つの道: i915/display/display.c・i915/display/bootfb.c・i915/display/capture.c
- 各 driver の display の wait の operation と release の後に画面に出す物（amdrdna/present.c など）
- display の flag（`GPU_DISPLAY_FIFO`・`VIRTUAL_CLOCK`・`ATOMIC_MODE_PRESENT`・`HAS_CURSOR`・`HAS_OVERLAYS`）を立てる所（grep）
- config/drivers/ の表と各 driver の build の規則（architecture）
- venus/venus.c `venus_get_info`・venus/transport.c

## 2. 作る物

- `docs/reference/gpu-drivers.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 表の Meaning の列（あれば）を全行。
- 節ごとの主張の表と、3 つの読み直し（gpu-doc-rules.md 4 節の要求の節の無い file の形）。
- 出典にする試験の見当: この file は要求の節を持たない（Sources・Errors の規則は掛からない）。表の Source の列に driver の file への link。

## 4. 注意

- 行は driver でなく node の種類。bit は名前で書く。
- runtime の bind が `drv_gpu_register` の前か後かを、呼ぶ順を読んで決める。後なら、先に開いた program が後の GPU_GET_INFO で新しい bit を見ることを書く。読み切れなければ未確認として結果の節に。
- bit と振る舞いの差（venus の JOB）は事実として書き、不具合と呼ばない。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-drivers.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu-drivers.md plan/ws009/phase019/phase.md plan/ws009/ws.md`。

## 結果

2026-10-30 10:05、実装の担当（Sonnet 5.5）。docs/reference/gpu-drivers.md を書いた（57 行、骨格の `<!-- spec: -->` と `PENDING` を全て置き換えた。見出しは変えていない）。cleared は Q1 が決める。

### 実行した command と最後の行

- `git log --oneline 4dfcce008..HEAD -- include/uapi/gpu*.h include/uapi/sysctl.h include/uapi/errno.h src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c userland/desktop/libvulkan/context.c`: 出力なし（基点の後の変化なし）。
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-drivers.md`: `check-gpu-uapi-doc: PASS (14 files, ..., final 1 files)`。
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008`: `check-gpu-uapi-doc: PASS (14 files, ..., final 0 files)`。
- `noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture`: `Markdown relative-link check: PASS (303 links)`。
- `git diff --check`: 出力なし（終了 0）。

### 自分で決めた点

- 行は node の種類。bcm2711・mali・powervr は表示と描画で 2 行、i915 は常駐の表示・Gen9 の boot framebuffer・capture（`I915_TEST_CAPTURE=y` の試験用 build だけ）の 3 行、amdrdna・ghci・nvrtx・venus は 1 行。
- bit は `GPU_CAP_` を除いた名前で書き、表の前に断った。
- 「後から足される bit」は、全ての driver が `drv_gpu_register` の前に ops の bit を決める（`capabilities |=` / `=` の全 grep で確かめた）ので、登録の後に変わる bit は無いと書いた。`GPU_GET_INFO` は gpu_info_ioctl が登録済みの `ops->capabilities` を返す（driver の get_info が書く値は捨てる）ことを根拠にした。そのため先に開いた program が後の GPU_GET_INFO で新しい bit を見る場合は無い。条件（`amdrdna.vk=0`・`amdrdna.disp.own=0`・`nvrtx.vk=0`・ghci の display controller と shader unit・i915 の display の有無）は「後から足される bit」の列に書いた。
- `GPU_DISPLAY_FIFO` の意味は header に無い。core は読まず、libvulkan の comment（native display が refresh 完了の契約を持つ時だけ FIFO を出す）に合わせて「whole frame を順に受ける driver の宣言」と書いた。
- lease の後の画面は driver ごとに事実だけを書いた。core が最初の flip で text console を終わらせる（gpu_console_end）ことを表の後に書き、text console が戻る約束はしないと書いた。
- ghci の「Present は 12 秒」は `GHCI_DISPLAY_ADOPTION_MS` (`GHCI_HANG_MS` + 2000)、bcm2711 の 100 ms は `FLIP_ADOPTION_TICKS`。
- Known differences の節は venus の JOB のほか、nvrtx・i915 の `GPU_DISPLAY_POWER`（`GPU_CAP_DISPLAY_CONTROL` があっても `GPU_DISPLAY_POWER_CONTROL` が無い output は `EOPNOTSUPP`）と、cursor・overlay が output の flag に依る事を足した。
- Driver-specific documents は visionfive2-gpu.md と vulkan-video.md（i915 の video decode）。

### source と header・spec・comment の食い違い（Bug の候補。文書には誤りとも予定とも書いていない）

- venus/venus.c `venus_get_info` は strict queue の無い host で `GPU_CAP_JOB` と `GPU_CAP_JOB_CAPACITY` を落とすが、gpu.c `gpu_info_ioctl` が `information.capabilities = device->ops->capabilities` で上書きするので、`GPU_GET_INFO` では常に 2 つの bit が立つ（spec の想定と一致。driver 側の落とす処理は効かない）。
- amdrdna/present.c の `present_present` の comment は「the call returns once the flip has begun」だが、code は `amdrdna_display_flip_wait` で flip を取り込む更新を待ってから返る。
- i915/display/present.c の file 先頭の comment は「one output ... No hot-plug and no topology events」だが、code は他の connector の output（`i915_display_other`）・`GPU_DISPLAY_LIMITED`・display events を持つ。
- config: `CONFIG_DRIVER_PCI_AMDRDNA` の既定値は config/drivers/pci.drivers が `n`、Makefile の `?=` が amd64 で `y`。文書は architecture（amd64）だけ書き、既定値は書いていない。

### 節ごとの主張の表

| 主張（節と要約） | 根拠（file と関数） | 試験 |
| --- | --- | --- |
| 導入: 8 driver、1 node 型 5・2 node 型 3 | `drv_gpu_register` の caller 11 箇所（grep） | 無し |
| Node kinds: bit は登録の前に決まり、登録の後に変えない | 全 driver の `capabilities` 代入の grep、gpu.c `gpu_info_ioctl` | 無し |
| amdrdna: executor は `amdrdna.vk=0` で外れ、display は `amdrdna.disp.own=0` で外れる | amdrdna/node.c `amdrdna_node_create`、amdrdna/runtime.c `amdrdna_runtime_bind`、amdrdna/present.c `amdrdna_a8_register`、amdrdna/sessions.c | 無し |
| amdrdna: flag FIFO・BLOB・REFRESH_COUNTER・POWER_CONTROL・HAS_CURSOR、wait は即答、lease の後は console surface、3 秒で text console | amdrdna/present.c `present_query`・`present_wait`・`present_end_lease`・`present_rest_watch` | 無し |
| bcm2711 render: bit 11 個、self-test 後に登録 | bcm2711/render-device.c `bind_render`・`bcm2711_render_register`、render-runtime.c | 無し |
| bcm2711 display: bit 8 個、flag、plane 3、adoption 100 ms、wait は最大 1 秒 | display-device.c `bind_operations`・`output_query`・`output_wait`、display-flip.c `bcm2711_display_flip_wait` | 無し |
| ghci: 条件付き bit（display controller・shader unit と GSI1） | ghci/ghci.c `choose_policy`・`register_device`、ghci/vk/vk-node.c | 無し |
| ghci: present は adoption 12 秒で ETIMEDOUT、release は dark | ghci/display.c `display_adoption_wait`・`display_release`、display-scan.c `ghci_scan_lease_end` | 無し |
| i915 常駐: 複数 output、FLIP armed で返る、wait は latch まで、10 秒保持 | i915/display/display.c、present.c `drv_i915_present_hold_start`・`drv_i915_present_display_wait` | 無し |
| i915 bootfb・capture の bit・flag | bootfb.c `drv_i915_bootfb_bind_ops`・`i915_bootfb_query`、capture.c | 無し |
| mali: display が先、bit、lease の後に console | mali/mali-attach.c、mali/render.c、mali/sched.c、mali/display/display.c | 無し |
| nvrtx: `nvrtx.vk=0`、HAS_CURSOR は cursor channel 次第、power は EOPNOTSUPP | nvrtx/node.c、nvrtx/stage.c、nvrtx/display/present.c | 無し |
| powervr: display が先、HAS_CURSOR は frame counter 次第 | powervr/display/display.c、vf2-gpu.c | 無し |
| venus: 定数の bit 16 個、flag、10 秒保持（log が quiet の時） | venus/venus.c、venus/display.c `display_release_output`・`display_hold_permitted` | 無し |
| Pairs: companion_id は全て 0、3 つの display node の constraints | 各 driver の query_device・constraints | 無し |
| Known differences: venus の JOB は `strict_queue == 0` で ENOTSUP | venus/transport.c reserve・capacity、`venus_strict_queue_find` | 無し |
| Known differences: cursor・commit・overlay_info の NULL は EOPNOTSUPP | gpu.c の cursor_set・overlay_info・commit の分岐 | 無し |

### 読み直した 3 つ（code の該当の分岐）

1. bcm2711 の adoption の期限 100 ms: `FLIP_ADOPTION_TICKS = KERN_MS_TO_TICKS(100U)` を `bcm2711_display_flip_wait` の deadline に使い、過ぎれば `uncertain` を立てて `ETIMEDOUT`。合っている。
2. venus の lease の後の保持: `display_hold_permitted` は primary の blob の `shared_front` があり、transport が失敗しておらず、console worker があり、`kern_log_quiet() != 0` の時だけ 1。期限は `VENUS_DISPLAY_HOLD_TICKS` = 10 秒。合っている。
3. nvrtx の `GPU_DISPLAY_POWER`: `present_power` が display_id の不一致で `ENOENT`、generation の不一致で `ESTALE`、holder でなければ `EBUSY`、それ以外で `EOPNOTSUPP`。合っている。

### 未確認・未実施

- 実機・QEMU での動作確認（この Phase は文書だけ。試験は未実施）。
- 実機の `GPU_GET_INFO` の実測値は未確認（source から導いた）。
- text console が最初の flip の後に戻る条件は、board の console の実装（kern_text_end の後の resume）まで読んでいない。文書は「戻る約束をしない」と書いた。
- i915 の wait が flip の latch を待つ上限（`I915_XD_PICTURE_FLIP_EVENT_MS` = 100 ms の event 待ち）は文書に数値を書いていない。

