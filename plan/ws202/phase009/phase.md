<!-- awesome-plan project=zedbsd record=ws202-p009 -->

# ws202-p009: Vulkan Video の back end (1) device・session・decode・読み出し

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 8 LW
依存: p015（slot を持つ entry だけを積む計画の形）、H1、J6。D25 の判定の口は p016 と並べて作り、p010 で両方を合わせる（L3-06）

## 目的

H.264 の picture を Vulkan Video で decode し、出力を linear の NV12 の picture にする back end の中身を作る（design §5.1・§5.4・§5.5）。表示順・seek・作り直し・表は p010。

## 成果（`userland/desktop/libmedia/`）

1. `vkvideo-device.c`（D8・D9）:
   - **`dlopen("/lib/libvulkan.so")`**（image の path、名だけにしない: host で Mesa を読まないように。L2-07）、dlclose しない。`vkGetInstanceProcAddr`・`vkGetDeviceProcAddr` から `struct vkvideo_functions`。
   - 共有の device を **mutex と参照の数**で遅延に作る（`pthread_once` を使わない）。instance は **apiVersion `VK_API_VERSION_1_0`** と instance の拡張
     `VK_KHR_get_physical_device_properties2`（`vkvideo-probe/main.c` の `probe_instance`）。queue family は `vkGetPhysicalDeviceQueueFamilyProperties2KHR`（KHR の名で引く）と
     `VkQueueFamilyVideoPropertiesKHR`（`probe_video_family`）。device は 4 つの拡張（`probe_device`）。
   - 「video が無い」を覚える（instance を壊しても残す）。「壊れた」の印（p010 で使う）。queue の mutex。
   - **参照の数が 0 になったら、いつも device と instance を壊す**（M2-06・L2-20: kernel の video・render の context は kernel session の close でだけ外れる、design §15 E13）。
2. `vkvideo.c` の decoder の state と decode:
   - **open（D28、M2-02）**: parser の用意、avcC の SPS があれば `h264_check_sps`、共有の device の参照、続けて capability（`probe_capabilities`）と D20（大きさ・MB の数・
     参照の数・slot の数。level では断らず、Vulkan に渡す level_idc は design §5.4 の表で列挙へ写し `maxLevelIdc` に丸める）、D21（slot の image の合計 256 MiB）、session と
     memory（`probe_session`）、parameters（`probe_parameters`）、slot の image（`probe_picture`）、bitstream の buffer（`probe_bitstream`）、command・fence、status の query pool
     （`probe_status_pool`）まで作る。BUSY・PROFILE・ENOMEM は open の問題になり、libavcodec があれば表の次へ回る。open は window の thread で走る（L-01: 時間は p010 で測る）。
     avcC に SPS の無い track（D16）は session 以下を最初の in-band の SPS の時に作る（p010）。
   - 1 picture の decode（`probe_decode` を手本）: D25 の判定の口（p016、並べて作る間は「いつも decode」の stub）で「decode」の picture だけ。参照に積むのは slot を持つ entry だけ。slice の写し（32 byte 揃え）、記録、queue の
     mutex の中で submit、fence（5 秒）、status。
   - 読み出し（D10）: memory type が HOST_COHERENT でなければ先に `vkInvalidateMappedMemoryRanges`（D24、L2-18。zedBSD は COHERENT）。PLANE_0・PLANE_1 の subresource layout で Tile Y を de-tile し、crop の窓を pool の buffer へ。SAR（VUI → pasp）・色（VUI → colr → 既定）。
   - D6: parameters の作り直し。同じ track の中の in-band の SPS の変化（IDR での大きさ・profile・参照の数）で session・image の作り直し（L2-10）。
   - session の作成の失敗の分け方（U10 は閉じた、L3-01・L3-07、design §5.4）: **`vkCreateVideoSessionKHR` の `VK_ERROR_OUT_OF_DEVICE_MEMORY` だけ**を BUSY に写す
     （`render/video.c` 1077・1082〜1088。`vkAllocateMemory` の同じ結果は本物の memory 不足で ENOMEM）。`vkCreateVideoSessionKHR` の `VK_ERROR_INITIALIZATION_FAILED`
     （hang・quarantine、1044〜1049）は DEVICE にするが「video の無い機械」の覚えを立てない。libvulkan が renderer の結果を変えずに返すことを `objects.c` で確かめる（U21）。
     session は open で作るので、これらは open の問題。
   - 2 段目（degraded 1）では avcC に SPS のある track を試さず FORMAT を返す（1 段目の BUSY・PROFILE を繰り返さない。decoder.c は D27 で FORMAT でない方を返す）。
3. ops（`media_vkvideo_ops`、backend "vulkan-video"、codec "h264"）: open・send・receive（この Phase は decode の順）・picture・flush・close、picture 系は p003 の関数。
   表にはまだ入れない。
4. U11: level_idc の丸めが Vulkan の仕様の VUID に触れないかを仕様で確かめ、phase.md に記録（触れるなら丸めずに断るか、SPS の level を変えずに渡すかを Q1 に報告）。
5. host 試験 `plan/ws202/tests/run-host-vkvideo.sh`: 偽の Vulkan の関数の表。instance の apiVersion が 1.0 で properties2 の拡張が有効、properties2 の KHR の関数で family を問う、
   **open で** session・image まで作る、session の作成の失敗が open の BUSY・D20・D21 が open の PROFILE、作る object の順と引数、setup・参照の slot（slot を持たない参照を
   積まない、D25 で捨てる picture は submit しない）、32 byte 揃え、de-tile・crop が模様と一致、parameters の作り直し、video の無い device で DEVICE、通常の close で参照の数 0 なら
   device と instance を壊す、本番の `dlopen("/lib/libvulkan.so")` が host で失敗して DEVICE（偽の表を入れない回）。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/run-host-vkvideo.sh` | PASS（ASan/UBSan） |
| `sh plan/ws202/tests/run-host-h264.sh` | PASS |
| design §10.6 の build（`ZEDBSD_CONFIG=config/ci/config-amd64.mk`、libmedia・libbrowser・videoplayer・music） | exit 0、`grep -c 'warning:'` が 0 |

## 注意

- 偽の Vulkan は本番の dlopen の道を通らない。本物は p010 の終わりの 5330 の小さい確認で。実装の担当は QEMU・実機を起動しない。
- libvulkan・i915 を変えない。疑いは Q1 に報告。
