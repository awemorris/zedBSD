<!-- awesome-plan project=zedbsd record=ws083-p008 -->

# ws083-p008: 性能・門の既定化・利用者への案内・result status query

Status: in-progress（q897、P2。この attempt は host の分: result status query と利用者への案内）
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
| S2 | status は GPU でなく **CPU** が書く: video の walk（apply）の中で、decode は同期に走り終わっているので、`vkCmdEndQuery` の時に CPU が status と availability 1 を書いて clflush。status pool の reset も CPU が op の順に（family に依らず、`drv_i915_gfx_query_execute` が status pool の reset を CPU で行う） | render の batch の reset は video の後に走るので、status を GPU や render の batch で書くと順が逆になる。同期の実行なので CPU の書き込みは decode の後と保証される |
| S3 | status の値: query の間の decode が全部 MFX で走り終われば COMPLETE（1）、D17 の検べで飛ばした decode が 1 つでもあれば ERROR（−1）、decode の無い query は COMPLETE。hang・DEVICE_LOST では書かない（unavailable のまま） | ANV より情報が多い（飛ばした decode を app が知れる）。規格の ERROR は「操作が失敗した」 |
| S4 | 検べ（模擬の walk でも）: video の family の `vkCmdBeginQuery` は scope の中・status pool・範囲内・active な query が無い時だけ、`vkCmdEndQuery` は同じ scope の active な query と同じ pool・index、scope の終わりに active な query が残れば拒否（`EBADMSG` → `VK_ERROR_DEVICE_LOST`、D18 と同じ）。graphics の family で status pool の begin・end は拒否（RCS が depth count を書くのを防ぐ）、video の family で occlusion の pool の begin・end も拒否 | 規格の VUID に沿う。誤った使い方で GPU や CPU が他の pool を書かない |
| S5 | libvulkan: video の decode の family で `queryResultStatusSupport` を TRUE。status pool は result の語 1 つ（status）を持つ pool として作り（`status_only`）、`vkGetQueryPoolResults` は status pool では各 query に `available ? status : 0` を必ず書く（PARTIAL の有無に依らず、32 bit では下位 32 bit = 符号つきの値）。pNext の profile は送らない（実行器は profile を見ない、family と同じ codec しか無い） | 規格の WITH_STATUS の書き方。wire は変えない |
| S6 | `vkvideo-probe` は family が TRUE なら 1 query の pool を作り、decode ごとに reset → begin coding → begin query → decode → end query → end coding、wait の後に WITH_STATUS で読み、COMPLETE でなければ `vkvideo-probe: picture N status S` を出し、最後の行に `, N failed` を足して exit 1 | p005・p006b の実機で、hash の不一致と D17 の飛ばしを区別できる |

UAPI（gpu-op.h）・HAL・wire の形は変えない（query の op は 1.0 の物）。

### 試験（host）

- `host-libvulkan-video.c`: queue family properties2 の status が video の family で TRUE、他は FALSE。
- 新 `host-libvulkan-status.c`（query.c を transport の stand-in で）: status pool の create の byte 列（type 1000023000）、results: available の −1 を 32・64 bit で、unavailable は 0 と `VK_NOT_READY`、COMPLETE、stride の padding を保つ。
- 往復（`host-video-wire.c`・`host-video-executor.c`）: status pool の作成（video の device だけ）、reset → begin → 通る decode → end で COMPLETE、飛ばす decode で ERROR、results の reply、scope の外の begin・active のまま end・二重 begin・occlusion の pool の begin は DEVICE_LOST、graphics の family の status pool の begin は DEVICE_LOST。

## 記録

- 2026-10-08 午後 P2: 設計（上）。design-reviewer の review の後に実装。
