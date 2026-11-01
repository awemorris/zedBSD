<!-- awesome-plan project=zedbsd record=ws322 -->

# WS322 VisionFive 2（JH7110） の Vulkan Video: H.264・H.265 decode（Wave511）、H.265 encode（Wave420L）を PowerVR の Vulkan の executor の Vulkan Video として。H.264 encode は hardware に無く独自の software（WS329）

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: incomplete（2026-10-30 Q1 が作成。2026-11-01 ユーザーから、P2 で WS322 を実行する、という趣旨の指示を受け、WS318 の completed を待たずに p001 を始めた）
- 目標: VisionFive 2（JH7110） の video の hardware のうち zedBSD で未実装の物を、kernel の Vulkan の executor（`src/drivers/gpu/powervr/`）の Vulkan Video（`VK_KHR_video_decode_*`・`VK_KHR_video_encode_*`）として実装し、libmedia から使えるようにする。

## ユーザーの決定（2026-10-30、原文）

- 「…H.264とH.265のデコーダとエンコーダの表の中で未の部分を埋めるようにVulkan Video driverを実装して…ドライバは個別のWSに分けて依存関係にしてもかまいません。先にlibmediaの方を作りきってcompleteにしてから、個別のドライバを実装する流れにします。…」（全文は [WS318](../ws318/ws.md)）
- 「VPUが独立だとしても、我々はVulkan Video driverとして見せる必要があります。それがOSのAPIなので。」
- H.265（2026-10-30 ユーザー「H.265のパテントプールへの支払いは行う方針です。H.265の実装を行っていいです。」）: WS236（2026-10-14）の「HEVCはライセンスがないのでやらないです」を置き換え、H.265 の decode・encode を実装する。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [ws322-p001](phase001/phase.md) | 調べと設計: hardware の能力の確認、firmware、出典と license、Vulkan Video の対応の範囲、試験の道 | cleared（2026-11-01 21:35 Q1、main 261624739。design.md 第 2 版と review 1 回の反映） | （WS318 の completed を待たない、2026-11-01 ユーザー） |
| [ws322-p002](phase002/phase.md) | VF2 の VPU の glue と Wave5 の core の boot、firmware の package（wave511）、実機の probe | cleared（2026-11-01 22:37 Q1 受け入れ、da223ef78 を main に merge。VF2 の実機で Wave511 が起動、6 本の stream の 68 frame が ffmpeg と一致、chagall.bin だけで足りる、VPU は CPU の cache と非一貫。p003 以降の設計の直しは design.md へ） | p001 |
| [ws322-p003](phase003/phase.md) | PowerVR の executor: NV12 の image・plane の view・export・import（Video Player の GPU の道）・query pool・capset・queue family | cleared（2026-11-01 23:51 Q1、main 361044f95。host の state 69・executor 752 PASS、VF2 で vf2-regress と nv12-board PASS、VF2 の既定の kernel は merge の後も warning 0） | p001 |
| [ws322-p004](phase004/phase.md) | H.264 decode の道（engine の口、Wave5 の decoder、parameter set の書き出し、executor、COINCIDE で pool から GPU の transfer、Video Player の GPU の道） | cleared（2026-11-02 01:55 Q1、main f9acea845。VF2 で H.264 の 8 本が全 frame で ffmpeg と一致、1080p30 の 600 frame 一致、Video Player の GPU の道で 16 s に 479 frame（late 2、CPU 約 20%）） | p002、p003 |
| ws322-p005 | libvulkan の H.265 decode の転送 | planned | WS318 p011a か先に着手（Q322-4） |
| ws322-p006 | H.265 decode | planned | p004、p005 |
| [ws322-p007](phase007/phase.md) | 速さと頑丈さ（timeout の後の立て直し、RESET の flush、task buffer の大きさ、速さの測り、JVT の照合） | cleared（2026-11-02 03:45 P2 q1896-i01、Q1 の受け入れ待ち。VF2 で立て直し・flush・3 回で停止・2 session・SIGKILL を確かめた、task buffer は vendor の ×1.5、1080p の VPU 7 ms・2160p 20 ms、Video Player 1080p60 は始めの数秒の後 late 0、JVT 75/101 一致。非同期・命令の queue は入れず） | p004 |
| ws322-p008 | encode の設計の詳細（Wave420L） | planned | Q322-2 は A（2026-11-01 ユーザー） |
| ws322-p009 | Wave420L の bring-up と probe | planned | p008 |
| ws322-p010 | libvulkan の encode の転送 | planned | WS318 p011b か先に着手（Q322-4） |
| ws322-p011 | executor の H.265 encode | planned | p009、p010 |
| ws322-p012 | 実機の受け入れ（Video Player の GPU の道での再生） | planned | p006・p007・p011 |
| ws322-p013 | 規約の全文の見直し | planned | 全部 |

### 実装の Phase の案（2026-11-01 P2、design.md §15。Q1 の受け入れの後に表へ）

| Phase | 目的 | 依存 |
| --- | --- | --- |
| p002 | VF2 の VPU の glue と Wave5 の core の boot、firmware の package（wave511）、実機の probe | p001、Q322-1、Q322-6、VF2 の板 |
| p003 | PowerVR の executor: NV12 の image・plane の view・query pool・capset・queue family | p001 |
| p004 | H.264 decode の道（engine の口、Wave5 の decoder、parameter set の書き出し、executor。第 3 版で COINCIDE にしたので vkvideo-probe の DISTINCT は不要） | p002、p003 |
| p005 | libvulkan の H.265 decode の転送 | WS318 p011a、Q322-4 |
| p006 | H.265 decode | p004、p005 |
| p007 | 速さ（非同期、SLC、命令の queue） | p004 |
| p008 | encode の設計の詳細（Wave420L） | Q322-2、WS318 p011b |
| p009 | Wave420L の bring-up と probe | p008、VF2 の板 |
| p010 | libvulkan の encode の転送 | WS318 p011b、Q322-4 |
| p011 | executor の H.265 encode | p009、p010 |
| p012 | 実機の受け入れ | p006・p007・p011、WS318 p009・p020・p022 |
| p013 | 規約の全文の見直し | 全部 |

## Q1 の判断（2026-11-01 21:35、p001 の問い）

- Q322-1 firmware（chagall.bin・monet.bin）は zedBSD-firmware で配る（A）。PowerVR の firmware と同じ条件（binary の再配布可・逆解析禁止）。
- Q322-4 libvulkan の H.265 と encode の転送は、WS322 と WS319 のうち先に着手する方が持つ。
- Q322-5 題の「H.264 encode は libavcodec」を「独自の software（WS329）」に直した（WS318 の Q318-2）。
- Q322-6 実装の Phase も WS318 の completed を待たずに始める（A）。2026-11-01 のユーザーの「P2 で WS322 を実行する」は WS 全体の実行と読む。
- Q322-7 libmedia の DISTINCT の道に bug が出たら、WS322 p004 の中で最小に直し、WS318 の担当に知らせる（A）。
  - （2026-11-01 Q1）design.md 第 3 版で D2 を DPB_AND_OUTPUT_COINCIDE に改めるのを認めた（pool から GPU の転送で写すので宛先はどの image でもよく、libmedia を変えずに Video Player の GPU の道に載る）。DISTINCT の道を使わないので Q322-7 の前提と vkvideo-probe の DISTINCT の対応は要らなくなった。
- （2026-11-01 21:41）Q322-2: ユーザーから、Wave420L の参考の source は file の SPDX の BSD-3-Clause を採って使う（A）、という趣旨の答えを受けた。値と手順だけを出典つきで使い、code・comment・構造・名前は写さない。食い違いは記録に残し、権利者の異議があれば encode の部分を外す。
- （2026-11-01 21:41）Q322-3: ユーザーから、GPU で直に表示する道（WS318 の p006b・p007a・p009、decode の絵を CPU に読み出さずに表示する）を先に実装する、という趣旨の答えを受けた。WS322 の受け入れ（p012）はその道の上で決め直す。
- （2026-11-01 21:42）上の Q322-3 の前提を直した: GPU で直に表示する道は WS243 で Video Player に既にあった（VP_GPU_PRESENT、RTX で 1080p60、arm64・amd64 で有効）。ユーザーから、WS318 を始めず、WS322 の p003・p004 で VF2 をこの既存の道に載せる（riscv64 の VP_GPU_PRESENT、PowerVR の NV12 の export・import と plane の view）、という趣旨の答えを受けた。受け入れはこの道での再生で決める。

## WS324 との揃え（2026-11-01 23:08 Q1）

- libvulkan の H.265・encode の転送、capset の bit、命令の番号は WS324 が持つ（Q324-3）。p005・p010 の libvulkan の部分は WS324 の物を使い、capset の bit は 2 = H.265 decode、3 = H.264 encode、4 = H.265 encode に揃える。

## WS324 の H.265 の解析（2026-11-02 00:10 Q1）

- p006（H.265 decode）は WS324 の h265-std.h の約束（StdVideoH265 の埋め方）を入力にする。

## Resume point

- 2026-11-02 03:45 P2（q1896-i01）: p007 を実装し VF2 で確かめた（[phase007](phase007/phase.md)）。次: Q1 の受け入れ、`CONFIG_DRIVER_VF2_VPU` の既定の判断（材料は phase007 の問い）、JVT の不一致 13 本の解析（新しい Phase か p012 の前）、p005・p006。

- 2026-11-02 01:53 P2（q1869-i01）: p004 を実装し VF2 で確かめた（[phase004](phase004/phase.md)）。task buffer を vendor の式の 2 倍にした（足りないと約 2 MB で firmware が止まる）。次: Q1 の受け入れ、`CONFIG_DRIVER_VF2_VPU` の既定の判断、p005〜p007。

- 2026-11-01 23:42 P2（q1863-i01）: p003 を実装し VF2 で確かめた（[phase003](phase003/phase.md)）。次: Q1 の受け入れ、p004（engine の口を広げ VF2 の glue から登録、COINCIDE で H.264 decode、pool から GPU の transfer）。
- 2026-11-01 P2（q1863-i01）: design.md を第 3 版に直した（p002 の観測: ring を続けて使う、pool と GPU の transfer を正の道、index は生きた slot から厳密に、VPU は CPU の cache と非一貫、配るのは chagall.bin。Video Player の GPU の道に載せるため DPB_AND_OUTPUT_COINCIDE に改めた（libmedia の GPU の道は DISTINCT を断る）、R13 と Q322-3 の訂正）。p003 を実装中（[phase003](phase003/phase.md)）。

- 2026-11-01 P2（q1852-i01）: p002 を実装し VF2 で probe した（[phase002](phase002/phase.md)）。次: Q1 の受け入れ、design.md の §7.3・§7.5・§9.2・R11・R13 の直し（p002 の観測を反映、後の Queue）、p003・p004。

- 2026-11-01 P2（q1845）: p001 の設計（design.md 第 2 版）と review 1（design-review.md、27 件）の反映が済んだ。次: Q1 の受け入れ、Q322-1〜7 の答え（Q322-2・Q322-3 はユーザー）、その後 p002・p003 から。

- 2026-11-01 20:43 Q1: ユーザーの指示で前倒し、p001 を P2（q1845）に。
- 2026-10-30 Q1: 作成。WS318 の completed の後。
