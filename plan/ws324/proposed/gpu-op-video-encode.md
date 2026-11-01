<!-- awesome-plan project=zedbsd record=ws324-proposed-gpu-op -->

# 案: GPU の命令の番号（video encode）と native の capset の video の bit

Parent: [WS324 design](../design.md) §10。状態: 案の第 2 版（2026-11-01 P1、review 1 の 3・16・17・18 番を反映）。HAL（`include/hal/hal.h`）ではなく UAPI（`include/uapi/gpu-op.h`）と libvulkan の capset の約束。全部の executor（i915・nvrtx・bcm2711・PowerVR）の番号の台帳なので、WS319・WS322 と同じ値を使うよう Q1 が揃える（Q324-3）。p004・p008a・p008b で適用する。

## 1. `include/uapi/gpu-op.h` の追加

今の video の番号は reply 付きが `0x10000`〜`0x10009`、記録が `0x1000a`〜`0x1000d` で、i915（`vk-dispatch.c:174`・`vk-command.c:316`）と bcm2711（`vulkan-video.c:629,742`）は範囲で振り分ける。その前提を壊さないよう、新しい番号は範囲を分けて置き、範囲の約束を comment に書く:

- `0x10010`〜`0x1001f`: reply 付きの問い合わせ・作成（video の第 2 群）
- `0x10020`〜`0x1002f`: 記録の命令（video の第 2 群）

| 番号 | 名前 | Vulkan | 形 |
| --- | --- | --- | --- |
| `0x10010` | `GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_ENCODE_QUALITY_LEVEL_PROPERTIES` | `vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR` | reply 付き。[physical device][`VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR`（profile と quality level）] → [`VkVideoEncodeQualityLevelPropertiesKHR` と codec の chain] |
| `0x10011` | `GPU_OP_GET_ENCODED_VIDEO_SESSION_PARAMETERS` | `vkGetEncodedVideoSessionParametersKHR` | reply 付き。[device][`VkVideoEncodeSessionParametersGetInfoKHR` と codec の chain（書く id の組）][u32 pData の有無][u64 受ける byte の数] → [VkResult][u64 書いた byte（pData 無しなら要る大きさ）][bytes][feedback の chain]。pData があって足りない時は何も書かず 0 と `VK_INCOMPLETE`（refpage の規則） |
| `0x10012` | `GPU_OP_CREATE_VIDEO_QUERY_POOL` | `vkCreateQueryPool`（type が `VIDEO_ENCODE_FEEDBACK` の時） | reply 付き。今の `CREATE_QUERY_POOL` の field に、profile（`VkVideoProfileInfoKHR` と codec の chain）と feedback の flag（`VkQueryPoolVideoEncodeFeedbackCreateInfoKHR`）を足した形 |
| `0x10020` | `GPU_OP_CMD_ENCODE_VIDEO` | `vkCmdEncodeVideoKHR` | 記録（reply 無し）。[`VkVideoEncodeInfoKHR` と codec の chain] |

- 今の `GPU_OP_CREATE_QUERY_POOL`（47）は Venus の wire format 1 の番号で、全部の executor が chain の presence 0 を前提に読む（libvulkan は常に 0 を送る、`query.c:96-104`）。feedback の query pool だけを新しい番号に分け、今の番号の形は変えない。今の result status の pool（WS083・WS243、chain 無しで作る）は今の番号のまま（profile が要らない）。
- 結果の語の数は libvulkan が数える: feedback の pool は flag の数（offset・bytes written）＋ result status の有無。
- rate control と quality level は今の `GPU_OP_CMD_BEGIN_VIDEO_CODING`・`GPU_OP_CMD_CONTROL_VIDEO_CODING` の chain（`VkVideoEncodeRateControlInfoKHR`・`VkVideoEncodeQualityLevelInfoKHR`）。これらの記録の chain は video の wire（`libvulkan/video.c`）の物で、video の capset の backend だけが読む。
- `GPU_OP_PROTOCOL_VERSION` は 2 のまま。capset の bit が無い backend には libvulkan が送らない（今の video と同じ規則、`gpu-op.h:134-140`）。
- 各番号の comment は今の形（Sent by・Decoded by・Order）で書く。

## 2. native の capset（176 byte）の byte 172 の flags

| bit | 意味 | 今 |
| --- | --- | --- |
| 0 | H.264 decode | ある（`libvulkan/context.c:40`、`nvrtx/runtime.c:89`） |
| 1 | sync command | ある |
| 2 | H.265 decode（`VK_KHR_video_decode_h265` の wire を読める） | 新 |
| 3 | H.264 encode（`VK_KHR_video_encode_queue`・`_h264` の wire と第 2 群の番号） | 新 |
| 4 | H.265 encode（`VK_KHR_video_encode_queue`・`_h265` の wire と第 2 群の番号） | 新 |

- WS322 の design §8.2 の案（bit 2 H.265 decode、bit 3 H.265 encode）と bit 3 が違う。H.264 encode を持つ driver（nvrtx・i915・Pi 4・VCN）が多いので、codec の順（decode H.264・H.265、encode H.264・H.265）で並べる上の案を推す。
- libvulkan の門は bit ごと（今の `video_h264` の 1 つの門、`instance.c:353,1314,1369`・`context.c:229-232` を 4 つに）: bit 3 も 4 も無ければ encode の queue の flag と format の encode の feature を隠し、`VK_KHR_video_encode_queue` を出さない。bit 0 も 2 も無ければ decode の物を隠す。family の codec の操作は renderer の答えと bit の積で残す（今の `instance.c:1392-1410` の H.264 decode だけの許可を拡げる）。
