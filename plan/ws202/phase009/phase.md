<!-- awesome-plan project=zedbsd record=ws202-p009 -->

# ws202-p009: Vulkan Video の back end (1) device・session・decode・読み出し

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 7 LW
依存: p008、H1（Vulkan Video の無い機械の扱い）

## 目的

p008 の picture を Vulkan Video で decode し、出力を linear の NV12 の picture にする back end の中身を作る（design §5.1・§5.4・§5.5）。
表示順・seek・失敗の扱いの仕上げは p010。

## 成果（`userland/desktop/libmedia/`）

1. `vkvideo-device.c`（design D8・D9）:
   - libvulkan の dlopen（名は image の `/lib` の物を確かめる: U7）、`vkGetInstanceProcAddr` から instance の関数、`vkGetDeviceProcAddr` から device の
     関数（`vkCreateVideoSessionKHR` 等の拡張の関数を含む）を `struct vkvideo_functions` に埋める。
   - 共有の device: `pthread_once` で作る instance（apiVersion 1.1 を要求、probe と同じ）、video の family のある physical device、4 つの拡張を
     有効にした device、video の queue、`VkPhysicalDeviceMemoryProperties`。参照の数、queue の mutex、「video が無い」「壊れた」の印。
     probe の `probe_instance`・`probe_video_family`・`probe_device` の流れを手本にする。
2. `vkvideo.c` の decoder の state と decode:
   - open の時: SPS（avcC）から profile（`VkVideoDecodeH264ProfileInfoKHR`、PROGRESSIVE）、capability の問い（`probe_capabilities` を手本）と
     SPS との比べ（design §5.4、外れは PROFILE）、session と memory の bind（`probe_session`）、parameters（`probe_parameters`、全 SPS・PPS から）、
     slot の image（NV12・OPTIMAL・DST|DPB・HOST_VISIBLE、map、view、`probe_picture`）、bitstream の buffer（1 MiB から 2 倍ずつ、`probe_bitstream`）、
     command pool・buffer・fence、result status の query pool（`probe_status_pool`）。
   - 1 picture の decode（`probe_decode` を手本）: slice を start code 付きで buffer に（offset を 32 に揃える）、記録（reset の query、最初の decode の
     image の layout の barrier、begin coding（参照と setup の slot）、RESET の control（session の最初・flush の後）、begin query・decode・end query、
     end coding）、queue の mutex の中で submit、fence を待つ（5 秒で ETIMEDOUT）、result status を読む。
   - 読み出し（design §5.5）: `vkGetImageSubresourceLayout`（PLANE_0・PLANE_1）の offset・rowPitch で Tile Y を de-tile し、crop の窓だけを
     picture の pool（p003）の buffer に linear の NV12 で写す。de-tile の式は `vkvideo-probe/frame.c` の `frame_tile_y_offset` と同じ（16 byte の
     column の単位の memcpy）。SAR（VUI → pasp）・色（VUI → colr → 既定）を picture に入れる。
   - D6: parameters の作り直し（p008 の `parameters_changed`）、SPS の大きさ・profile・参照の数の変化での session・image の作り直し（queue を idle に）。
3. ops（`media_vkvideo_ops`、backend 名 "vulkan-video"、codec 名 "h264"）: open・send・receive・picture（pool の参照を返す）・flush・close と
   picture 系（p003 の `media_picture_ops_*`）。この Phase の receive は decode の順に出す（並べ替えは p010）。表にはまだ入れない（p010 で入れる）。
4. host 試験の枠 `plan/ws202/tests/host-vkvideo.c`・`run-host-vkvideo.sh`（design §10.2）: 偽の Vulkan の関数の表（`vkvideo_functions` を試験が埋める）。
   偽の decode は出力の slot の image の memory に、picture の番号から決まる Tile Y の模様を書く。試験は: 作る object の順と主な引数（profile・
   extent・slot の数・format）、decode ごとの setup・参照の slot（p008 の計画と同じ）、bitstream の buffer の offset の揃え、de-tile・crop の結果が
   模様と一致、parameters の作り直し、video の無い device（family 無し）で DEVICE。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/run-host-vkvideo.sh` | 上の項目が PASS（ASan/UBSan） |
| `sh plan/ws202/tests/run-host-h264.sh` | PASS |
| libmedia・videoplayer の build | warning 0 |

## 注意

- 本当の decode は host でできない（host の Vulkan は llvmpipe）。正しさの最後の確かめは p012 の 5330。
- 実装の担当は QEMU・実機を起動しない（T1 に頼む）。
- libvulkan・i915（WS083 の範囲）を変えない。Vulkan の振る舞いに疑いがあれば `docs/reference/vulkan-video.md` と WS083 の design を読み、
  Q1 に報告する。
