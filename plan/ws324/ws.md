<!-- awesome-plan project=zedbsd record=ws324 -->

# WS324 RTX 2070（nvrtx） の Vulkan Video: H.264 encode（NVENC）、H.265 decode（NVDEC）・encode（NVENC）

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: incomplete（2026-10-30 Q1 が作成。2026-11-01 ユーザーから、WS291 をラップアップして RTX の H.264 encode・H.265 decode・H.265 encode の実装に移る、という趣旨の指示を受け、WS318 の completed を待たずに p001 を始める）
- 目標: RTX 2070（nvrtx） の video の hardware のうち zedBSD で未実装の物を、kernel の Vulkan の executor（`src/drivers/gpu/nvrtx/`）の Vulkan Video（`VK_KHR_video_decode_*`・`VK_KHR_video_encode_*`）として実装し、libmedia から使えるようにする。

## ユーザーの決定（2026-10-30、原文）

- 「…H.264とH.265のデコーダとエンコーダの表の中で未の部分を埋めるようにVulkan Video driverを実装して…ドライバは個別のWSに分けて依存関係にしてもかまいません。先にlibmediaの方を作りきってcompleteにしてから、個別のドライバを実装する流れにします。…」（全文は [WS318](../ws318/ws.md)）
- 「VPUが独立だとしても、我々はVulkan Video driverとして見せる必要があります。それがOSのAPIなので。」
- H.265（2026-10-30 ユーザー「H.265のパテントプールへの支払いは行う方針です。H.265の実装を行っていいです。」）: WS236（2026-10-14）の「HEVCはライセンスがないのでやらないです」を置き換え、H.265 の decode・encode を実装する。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [ws324-p001](phase001/phase.md) | 調べと設計: hardware の能力の確認、firmware、出典と license、Vulkan Video の対応の範囲、試験の道 | in-progress（P1 q1854。design.md 第 1 版、review の前） | （WS318 を待たない、2026-11-01 ユーザー） |

### 実装の Phase の案（2026-11-01 P1、[design.md](design.md) §15。Q1 の受け入れの後に表へ）

| Phase | 目的 | 依存 |
| --- | --- | --- |
| p002 | Vulkan の header の段 1（H.265 decode） | Q324-2（Q1 の toolchain の許可） |
| p003 | Vulkan の header の段 2（KHR の encode） | p002、Q324-2 |
| p004 | libvulkan の H.265 decode の転送、capset の bit 2 | p002、Q324-3 |
| p005 | nvrtx の H.265 decode（NVDEC の HEVC、executor） | p004 |
| p006 | H.265 の解析（Q324-1）と vkvideo-probe の H.265 | p002、Q324-1 |
| p007 | H.265 decode の実機の debug | p005、p006、実機 |
| p008 | libvulkan の encode の転送、命令の番号、capset の bit 3・4 | p003、Q324-3 |
| p009 | NVENC の関門（channel、kernel の中の probe で I・P 絵、host の ffmpeg で decode） | 実機、Q324-4 |
| p010 | encode の共通の executor（NV12 の入力の copy、16x16 の DPB、session、feedback、parameter set の書き出し） | p008、p009 |
| p011 | H.264 encode と vkencode-probe | p010 |
| p012 | H.265 encode | p011、p006 |
| p013 | rate control（CBR・VBR） | p011、p012 |
| p014 | 実機の受け入れ | p007、p011〜p013 |
| p015 | 規約の全文の見直し | 全部 |

## Resume point

- 2026-11-01 22:30 P1（q1854）: p001 の design.md 第 1 版・proposed/・問い Q324-1〜6（phase.md）。次は design-reviewer の review 1 と反映。

- 2026-11-01 21:46 Q1: ユーザーの指示で前倒し、p001 を P1（q1854）に。WS291 の P1 の返却の後に起動。
- 2026-10-30 Q1: 作成。WS318 の completed の後。
