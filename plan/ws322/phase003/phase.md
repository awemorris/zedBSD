<!-- awesome-plan project=zedbsd record=ws322-p003 -->

# ws322-p003 PowerVR の executor: NV12 の image・plane の view・export・import（Video Player の GPU の道）・query pool・capset・queue family

- WS: [WS322](../ws.md)。状態: cleared（2026-11-01 23:42 P2 q1863-i01。Q1 の受け入れ待ち）。担当: P2（q1863-i01、worktree p2-ws322、branch agent/p2-ws322）。
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

## 結果（2026-11-01 P2、q1863-i01）

### commit（基点 main 9ca68405f）

- f382cef66b・a2ca1907d6: design.md 第 3 版、この phase.md、ws.md、plan/tools/powervr/p009/run.sh の `-I"$root"`（main で compile が落ちていた、Q1 の許可）。
- af8cc9e313・ba3c303c82・60426900ec と最後の commit（Q1 への報告に SHA）: 実装と試験。

### 実装（範囲の 1〜6）

- `image-state.c`・`.h`: NV12 の表の行（kind PLANAR、両 unit の code 無し、features は TRANSFER_SRC だけ）と `powervr_image_lay_out_planes`（R8 の luma と R8G8 の chroma、同じ pitch、各 plane の行を 32 に丸め、chroma は 4 KiB 境界）。
- `vk/vk-image.c`・`vk/vk.h`: NV12 の create（flags は 0 か MUTABLE_FORMAT | EXTENDED_USAGE、usage は SAMPLED・TRANSFER_SRC と engine があれば VIDEO_DECODE_DST・DPB、SAMPLED は EXTENDED_USAGE の時だけ、1 level・1 layer・偶数の辺、tiling は両方で中は linear）、subresource layout の PLANE_0・PLANE_1、plane の view（PLANE_0 を R8・PLANE_1 を R8G8、全体の形式と COLOR の aspect は断る）。view に形式と layer の大きさを持たせ、`vk/vk-layout.c` の descriptor はそれを使う。
- `vk/vk-pass.c`: NV12 の framebuffer を断る。`vk/vk-transfer.c`: NV12 は CopyImageToBuffer の PLANE_0・PLANE_1 だけ（他は記録で ENOTSUP）。transfer は 4・8・16 byte の texel しか raw で写さない（`transfer-program.c` の `powervr_transfer_raw_format`）ので、plane の行を 32 bit の word として写す（region の始まり・幅・buffer の行と offset が word に揃う時だけ、揃わなければ記録で ENOTSUP。板の 1 回目でこれが無く transfer が EOPNOTSUPP で落ちたので足した）。
- `vk/vk-device.c`: NV12 の format（engine があれば VIDEO_DECODE_OUTPUT・DPB）、image format（NV12 の flags と usage、limits は 1 level・1 layer）、`..._IMAGE_FORMAT_PROPERTIES2`・`..._EXTERNAL_BUFFER_PROPERTIES`（node の handle の型で EXPORTABLE | IMPORTABLE）、queue family の video の flag、`..._QUEUE_FAMILY_VIDEO_PROPERTIES`。
- `render.c`: `GPU_CAP_ALLOCATION_SHARE`（`render_export` は image 無しの export を既に扱う）、session の open で video の operations を読んで固定（`render-private.h` の `video_operations`）。`render-runtime.c`: engine が H.264 decode を出す時だけ capset 176（byte 168 に `0x5a4e4154`、172 に bit 0）。
- 新 `video-state.c`・`.h`: `drv_powervr_video_register`（ops は `operations` だけ、一度だけ、`powervr.h` に宣言）と operations からの報告、query の記録 1 件の形（host で build）。新 `vk/vk-query.c`: RESULT_STATUS_ONLY の pool（作成・破棄・結果）、記録の reset・begin・end、submission の中での実行、decode の報告の口 `powervr_vk_query_report`（p004 が使う）。
- `platform/riscv64/vmunix.mk` に新 file 2 つ。HAL・`include/uapi/`・libvulkan・libmedia・Video Player は変えていない。

### 確かめた事

| 項目 | 結果 |
| --- | --- |
| host 試験 | `sh plan/ws322/tests/p003/run.sh build/tmp/p003-host3` → state 69 passed 0 failed、executor 752 passed 0 failed（p009 の executor の 505 件を含む）。`plan/tools/powervr/p010/run.sh` も全部 0 failed |
| build | `make ZEDBSD_CONFIG=config/ci/config-visionfive2.mk BUILD=build/ws322-p003 build/ws322-p003/vmunix` warning 0。`plan/ws291/tests/config-vf2-shot.mk` の vmunix（cksum 1831050385）と wayland・files・terminal・keiland-shot・vkmatrix・vkcoherency・vkcompute・vkimage warning 0。nv12-board（`build-board.sh`）warning 0 |
| 既定の動作の非退行（VF2 実機、10.0.30.2） | `run-vf2.sh build/tmp/p003-vf2-r5 build/ws322-p003s`（23:37〜23:41）の vf2-regress: vkmatrix 109/109、`-p 3` PASS、`-r 3` 327/327、vkcoherency PASS、vkcompute 4096/4096、desktop の compositor error 0・起動 1、keiland-shot の PNG cksum 518389155（ws291-p020・q1817・q1839 と同じ）、step の間の再起動なし（boot-000324）、fault・lost の行なし |
| NV12 の sample・plane の copy（実機） | nv12-board の A: 64x64 の NV12（OPTIMAL、SAMPLED | TRANSFER_SRC、MUTABLE_FORMAT | EXTENDED_USAGE、export できる host visible の memory）に模様を書き、R8・R8G8 の plane の view を fragment shader で sample: 0/4096 pixel 違い。PLANE_0・PLANE_1 の CopyImageToBuffer: 0/5120 texel 違い。subresource layout は plane 0 が offset 0・row 64・4096 B、plane 1 が 4096・64・2048 B |
| export・import（実機） | 別の VkInstance（別の open）の B に OPAQUE_FD で import、Video Player と同じ create（SAMPLED だけ）の NV12 に bind して sample: 0/4096 pixel 違い。memory の要求の大きさは A と同じ |
| engine 無しの報告（実機） | family 1 つ、video の queue flag 0x0、`VK_KHR_video*` の拡張 0（libvulkan は native capset の bit が無い時だけそうなる）。vkimage PASS、試験の間に kernel の行の増え無し |
| 板の後始末 | 各回の後 `/boot/boot0/vmunix` 3758577736（= vmunix.q1825orig）、`/etc/group` 4260090434、`zedbsd.cfg` 632416842、/tmp は空。/boot に新しい file を作っていない |

- 板の回: r1（regress PASS、nv12-board は試験の program の誤りで instance 作成が VK_ERROR_INCOMPATIBLE_DRIVER: apiVersion 1.1）、r2（device の拡張に instance の external memory capabilities が要る）、r3（plane の copy が transfer の EOPNOTSUPP → 上の word の写しを足した）、r4（nv12 PASS）、r5（最後の kernel で regress と nv12 の両方 PASS）。証拠は worktree の `build/tmp/p003-vf2-r1`〜`r5`（git に入れない）。QEMU では流していない（QEMU に PowerVR は無い）。

## 未実施

- engine がある時の capset 176・video の family・NV12 の video の usage は host 試験（session の operations を直に置いて）だけ。実機は p004（Wave5 の engine の登録）で。
- query の begin と end の間の decode の報告は host 試験で口だけ。p004 の decode で実機。
- 1080p の大きさの plane の view と copy は host 試験だけ（実機は 64x64）。幅が 4 の倍数でない NV12 の plane の copy は ENOTSUP（video の幅は 16 の倍数なので p004 では起こらない見込み）。
- Video Player での NV12 の再生そのものは p004（decode の image が無い今は試せない）。
- capset の 176 byte の reply は host 試験に無い（`render-runtime.c` は host で build しない）。出す条件は `powervr_video_capset_flags` の host 試験で見た。

## 引き継ぎ（p004 へ）

- design.md 第 3 版のとおり DPB_AND_OUTPUT_COINCIDE で報告し、pool の linear[i] から GPU の transfer で宛先へ写す。NV12 の宛先は 2 plane の linear で、この Phase の plane の word の写しと同じ形で pool から写せる（image への copy は今は断っているので、executor の中の写しは record を通さず transfer の request を直に作る）。
- `drv_powervr_video_register` の ops を広げ、VF2 の glue から登録する。
