<!-- awesome-plan project=zedbsd record=ws322-p003 -->

# ws322-p003 PowerVR の executor: NV12 の image・plane の view・export・import（Video Player の GPU の道）・query pool・capset・queue family

- WS: [WS322](../ws.md)。状態: in-progress（2026-11-01 P2 q1863-i01）。担当: P2（q1863-i01、worktree p2-ws322、branch agent/p2-ws322）。
- 設計: [design.md](../design.md) 第 3 版の §8.1（engine の口の最初の形）・§8.2・§8.4・§8.5（query の記録）・§15 の p003。

## 目的

VF2 の Wave511 の decode（p004）を、WS243 で Video Player に既にある GPU の道（`VP_GPU_PRESENT`: libmedia が decode の NV12 の image を export し、Video Player が import して plane の view を自分の shader で sample する）に載せるために、PowerVR の Vulkan の executor に要る物を、video の engine に依らない部分だけ先に足す。engine が無い今の既定の動作（capset 168、video の family 無し）は変えない。

## 範囲

1. NV12（`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`）の image: 2 plane の linear の配置（plane 0 は R8、plane 1 は R8G8、同じ pitch、4 KiB 境界、各 plane の行を 32 に丸める）、usage は SAMPLED・TRANSFER_SRC（engine があれば VIDEO_DECODE_DST・VIDEO_DECODE_DPB も）、create flags は 0 か MUTABLE_FORMAT | EXTENDED_USAGE、1 level・1 layer・偶数の辺。memory の要求・bind・subresource layout（PLANE_0・PLANE_1）。
2. plane の view: PLANE_0 を R8_UNORM、PLANE_1 を R8G8_UNORM の view。descriptor の image の語は view の形式と plane の layout から。NV12 の image への render pass・copy・blit・clear は断る。
3. plane の copy: `vkCmdCopyImageToBuffer` の PLANE_0・PLANE_1（libmedia の readback の段、hash の照合用）。
4. export・import: node に `GPU_CAP_ALLOCATION_SHARE`。`GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES2`・`GPU_OP_GET_PHYSICAL_DEVICE_EXTERNAL_BUFFER_PROPERTIES` に答える。image の format の問いは MUTABLE_FORMAT | EXTENDED_USAGE の NV12 に答える。
5. query pool: `VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR` の作成・破棄・結果、記録の reset・begin・end と実行（decode が結果を書く口は p004）。
6. video の engine の登録の口（`drv_powervr_video_register`、ops は `operations` だけ。p004 で広げる）。session は open の時に operations を読んで固定する。それが 0 でない時だけ: capset 176（byte 168 に native の magic、172 に H.264 decode の bit）、queue family 0 に VIDEO_DECODE（と ENCODE）の flag、`GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_VIDEO_PROPERTIES` の codec の operations、NV12 の format の VIDEO_DECODE_OUTPUT・DPB の features と video の usage。
- 範囲の外: video の session・parameters・capabilities・format properties の報告と decode の記録・実行（p004）、Wave5 の decoder（p004）、libmedia・libvulkan・Video Player の変更（要らない見込み。要ると分かれば Q1 へ）。

## UAPI・HAL

- HAL の API は変えない。
- `include/uapi/` は変えない（`GPU_CAP_ALLOCATION_SHARE`・使う opcode・wire の形は全部既にある。WS243 p006 で nvrtx に足したのと同じ組を PowerVR に足すだけ）。

## 手順

1. design.md を p002 の結果で第 3 版に直す（§7.3 ring を続けて使う、§7.5 pool と GPU の transfer を主に・index を生きた slot から厳密に、§9.2・R14 非一貫、R11 chagall.bin、R13・Q322-3 の訂正、D2 を COINCIDE に）。
2. `image-state.c`・`.h`: NV12 の表の行と plane の配置の関数。
3. `vk/vk-image.c`・`vk/vk.h`: NV12 の create・requirements・bind・subresource layout・plane の view。`vk/vk-layout.c`: descriptor を view の形式と layout で。`vk/vk-pass.c`: NV12 の framebuffer を断る。`vk/vk-transfer.c`: plane の copy と NV12 の他の transfer の拒否。
4. `vk/vk-device.c`: format・image format（flags・EXTENDED_USAGE・video の usage）・image format 2・external buffer・queue family（video の flag）・family の video の operations。
5. `render.c`: `GPU_CAP_ALLOCATION_SHARE`、session の open で video の operations を読む。`render-runtime.c`: capset 176。
6. `video-state.c`・`.h`（新）: engine の登録と、operations からの報告（capset の flags、queue の flags、codec の operations、format の features）と query の結果の 1 件の形、host で build できる純な部分。
7. `vk/vk-query.c`（新）: query pool と記録・実行。`vk/vk-command.c`・`vk/vk-queue.c`・`vk/vk-stream.c`・`vk/vk.h` の hook。`platform/riscv64/vmunix.mk` に新しい file。
8. 試験: `plan/ws322/tests/p003/`（host 試験、板の試験の program と script）。

## 受け入れ

| 項目 | 確かめ方 |
| --- | --- |
| host 試験 | `plan/ws322/tests/p003/run.sh`: NV12 の配置と plane の view の語（image-state.c）、video-state.c の報告と query の記録、libvulkan の encoder で書いた frame を executor に通す（NV12 の create・requirements・subresource・plane の view・descriptor・plane の copy・image format・external の問い・query pool・engine 有りと無しの capset と family）。既存の executor の host 試験（plan/tools/powervr/p009 の 505 件、p010）も通る |
| build | VF2 の既定の config（`config/ci/config-visionfive2.mk` か同等）の vmunix と libvulkan・試験の program が warning 0 |
| 既定の動作の非退行 | VF2 で `plan/ws291/tests/vf2-regress.sh`（vkmatrix・vkcoherency・vkcompute・desktop の PNG）が main の kernel と同じ形 |
| NV12 の sample | VF2 で試験の program: NV12（OPTIMAL、SAMPLED、MUTABLE_FORMAT | EXTENDED_USAGE、host visible の memory）に模様を書き、PLANE_0 の R8・PLANE_1 の R8G8 の view を fragment shader で sample して色の image に描き、読み出して期待の値と一致。PLANE_0・PLANE_1 の CopyImageToBuffer も一致 |
| export・import | 同じ program で、memory を OPAQUE_FD で export し、別の VkDevice（別の open）に import して NV12 の image（SAMPLED だけ、Video Player と同じ create）に bind し、plane の view の sample が一致 |
| engine 無しの報告 | 板の上で capset が 168 のまま、family の flag に video が無い（試験の program か vkvideo-probe --list で） |

## 結果

（作業中）

## 未実施

（作業中）
