<!-- awesome-plan project=zedbsd record=ws322 -->

# WS322 VisionFive 2（JH7110） の Vulkan Video: H.264・H.265 decode（Wave511）、H.265 encode（Wave420L）を PowerVR の Vulkan の executor の Vulkan Video として。H.264 encode は hardware に無く libavcodec

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
| [ws322-p001](phase001/phase.md) | 調べと設計: hardware の能力の確認、firmware、出典と license、Vulkan Video の対応の範囲、試験の道 | in-progress（2026-11-01 P2 q1845。design.md 第 2 版と review 1 の反映は済み、Q1 の受け入れと Q322-1〜7 の答え待ち） | （WS318 の completed を待たない、2026-11-01 ユーザー） |

### 実装の Phase の案（2026-11-01 P2、design.md §15。Q1 の受け入れの後に表へ）

| Phase | 目的 | 依存 |
| --- | --- | --- |
| p002 | VF2 の VPU の glue と Wave5 の core の boot、firmware の package（wave511）、実機の probe | p001、Q322-1、Q322-6、VF2 の板 |
| p003 | PowerVR の executor: NV12 の image・plane の view・query pool・capset・queue family | p001 |
| p004 | H.264 decode の道（engine の口、Wave5 の decoder、parameter set の書き出し、executor、vkvideo-probe の DISTINCT） | p002、p003、Q322-7 |
| p005 | libvulkan の H.265 decode の転送 | WS318 p011a、Q322-4 |
| p006 | H.265 decode | p004、p005 |
| p007 | 速さ（非同期、SLC、命令の queue） | p004 |
| p008 | encode の設計の詳細（Wave420L） | Q322-2、WS318 p011b |
| p009 | Wave420L の bring-up と probe | p008、VF2 の板 |
| p010 | libvulkan の encode の転送 | WS318 p011b、Q322-4 |
| p011 | executor の H.265 encode | p009、p010 |
| p012 | 実機の受け入れ | p006・p007・p011、WS318 p009・p020・p022 |
| p013 | 規約の全文の見直し | 全部 |

## Resume point

- 2026-11-01 P2（q1845）: p001 の設計（design.md 第 2 版）と review 1（design-review.md、27 件）の反映が済んだ。次: Q1 の受け入れ、Q322-1〜7 の答え（Q322-2・Q322-3 はユーザー）、その後 p002・p003 から。

- 2026-11-01 20:43 Q1: ユーザーの指示で前倒し、p001 を P2（q1845）に。
- 2026-10-30 Q1: 作成。WS318 の completed の後。
