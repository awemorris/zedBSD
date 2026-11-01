<!-- awesome-plan project=zedbsd record=ws324-proposed-gpu-op -->

# 案: GPU の命令の番号（video encode）と native の capset の video の bit

Parent: [WS324 design](../design.md) §10。状態: 案（2026-11-01 P1）。HAL（`include/hal/hal.h`）ではなく UAPI（`include/uapi/gpu-op.h`）と libvulkan の capset の約束。全部の executor（i915・nvrtx・bcm2711・PowerVR）の番号の台帳なので、WS319・WS322 と同じ値を使うよう Q1 が揃える（Q324-3）。p004・p008 で適用する。

## 1. `include/uapi/gpu-op.h` の追加（`GPU_OP_CMD_DECODE_VIDEO = 0x1000d` の後）

| 番号 | 名前 | Vulkan | 形 |
| --- | --- | --- | --- |
| `0x1000e` | `GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_ENCODE_QUALITY_LEVEL_PROPERTIES` | `vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR` | reply 付き。[physical device][`VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR`（profile と quality level）] → [`VkVideoEncodeQualityLevelPropertiesKHR` と codec の chain] |
| `0x1000f` | `GPU_OP_GET_ENCODED_VIDEO_SESSION_PARAMETERS` | `vkGetEncodedVideoSessionParametersKHR` | reply 付き。[device][`VkVideoEncodeSessionParametersGetInfoKHR` と codec の chain（write する id の組）][u64 受ける byte の数（0 なら大きさだけ）] → [VkResult][u64 書いた byte][bytes][feedback の chain] |
| `0x10010` | `GPU_OP_CMD_ENCODE_VIDEO` | `vkCmdEncodeVideoKHR` | 記録（reply 無し）。[`VkVideoEncodeInfoKHR` と codec の chain] |

- encode の feedback の query pool は今の `GPU_OP_CREATE_QUERY_POOL` の chain（`VkQueryPoolVideoEncodeFeedbackCreateInfoKHR` と `VkVideoProfileInfoKHR`）で作る。新しい番号は要らない。
- rate control と quality level は今の `GPU_OP_CMD_BEGIN_VIDEO_CODING`・`GPU_OP_CMD_CONTROL_VIDEO_CODING` の chain（`VkVideoEncodeRateControlInfoKHR`・`VkVideoEncodeQualityLevelInfoKHR`）。
- `GPU_OP_PROTOCOL_VERSION` は 2 のまま。capset の encode の bit が無い backend には libvulkan が送らない（今の video と同じ規則、`gpu-op.h:134-140`）。
- 各番号の comment は今の形（Sent by・Decoded by・Order）で書く。

## 2. native の capset（176 byte）の byte 172 の flags

| bit | 意味 | 今 |
| --- | --- | --- |
| 0 | H.264 decode | ある（`libvulkan/context.c:40`、`nvrtx/runtime.c:89`） |
| 1 | sync command | ある |
| 2 | H.265 decode（`VK_KHR_video_decode_h265` の wire を読める） | 新 |
| 3 | H.264 encode（`VK_KHR_video_encode_queue`・`_h264` の wire） | 新 |
| 4 | H.265 encode（`VK_KHR_video_encode_queue`・`_h265` の wire） | 新 |

- WS322 の design §8.2 の案（bit 2 H.265 decode、bit 3 H.265 encode）と bit 3 が違う。H.264 encode を持つ driver（nvrtx・i915・Pi 4・VCN）が多いので、codec の順（decode H.264・H.265、encode H.264・H.265）で並べる上の案を推す。
- libvulkan は bit 3 か 4 があれば `VK_KHR_video_encode_queue` を出す。family の codec の操作は renderer の答えと capset の bit の積で残す（今の `instance.c:1392-1410` の H.264 decode だけの許可を拡げる）。
