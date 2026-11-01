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
| [ws324-p001](phase001/phase.md) | 調べと設計: hardware の能力の確認、firmware、出典と license、Vulkan Video の対応の範囲、試験の道 | cleared（2026-11-01 23:08 Q1、main a0fd88ee8。design.md 第 2 版、review 31 件の反映） | （WS318 を待たない、2026-11-01 ユーザー） |
| [ws324-p006a](phase006a/phase.md) | H.265 の解析の設計（標準廉価実装手順 §4、[h265-parse-design.md](h265-parse-design.md)、header は [proposed/h265/](proposed/h265/)）と、header を source に置く段 2 | in-progress（2026-11-02 P1 q1864-i01。段 1 の設計と review 2 の 20 件の反映済み、Q1 の受け入れと Q-A〜Q-E 待ち） | Q324-1 |

### 実装の Phase の案（2026-11-01 P1、[design.md](design.md) 第 2 版 §15。Q1 の受け入れの後に表へ）

| Phase | 目的 | 依存 |
| --- | --- | --- |
| p002 | Vulkan の header の段 1（H.265 decode の選び） | Q324-2（Q1 の toolchain の許可） |
| p003 | Vulkan の header の段 2（`vulkan_core.h` を 1.4.309 で作り直し、KHR の encode） | p002、Q324-2 |
| p004 | libvulkan の H.265 decode の転送、capset の bit 2、video の門を bit ごとに | p002、Q324-3 |
| p005 | nvrtx の H.265 decode（NVDEC の HEVC、executor、行の揃え 64、timeout の後の隔離） | p004 |
| p006a | H.265 の解析の設計（標準廉価実装手順）と header の配置（段 2） | Q324-1（p002 は要らない: host の Vulkan の header で試験） |
| p006b〜g | H.265 の解析の実装、Sonnet（nal・scaling・vui ／ rps・ps ／ slice・poc ／ dpb ／ std ／ stream）（p006a の案、[h265-parse-design.md](h265-parse-design.md) §9、Q-B） | p006a 段 2（b→c→d→g、e・f は並べて） |
| p006h | H.265 の解析の適合の試験（HEVC_v1、host） | p006e〜g |
| p006i | vkvideo-probe の H.265（前の案の p006e） | p006h、p002 |
| p007 | H.265 decode の実機の debug | p005、p006i、実機 |
| p008a | libvulkan の encode の転送（queue と H.264、命令の番号、video の query pool、capset の bit 3） | p003、Q324-3 |
| p008b | libvulkan の encode の転送（H.265、capset の bit 4） | p008a |
| p009a | NVENC の channel（段 nvenc-channel、RC の通知の記録） | 実機 |
| p009b | NVENC の関門（kernel の中の自己診断で I・P 絵、host の ffmpeg で decode、時間の上限） | p009a、Q324-4 |
| p010 | encode の共通の executor（NV12 の入力の copy、16x16 の DPB、session、feedback、parameter set の書き出し） | p008a、p009b |
| p011 | H.264 encode と vkencode-probe | p010 |
| p012 | H.265 encode | p011、p008b、p007 |
| p013 | rate control（CBR・VBR） | p011、p012 |
| p014 | 実機の受け入れ | p007、p011〜p013 |
| p015 | 規約の全文の見直し | p014 |
| p016 | （外した、2026-11-01 ユーザーの Q324-6。libmedia の道の RTX での確かめは WS318 の受け入れへ） | — |

## Q1 の判断（2026-11-01 23:08、p001 の問い）

- Q324-1: H.265 の解析は、WS318 p016〜p019 の範囲を標準廉価実装手順で先に作る（p006a の設計と Sonnet の実装）。libmedia の build と runtime には繋がない。WS318 の ws.md にその旨を書く。
- Q324-2: Vulkan の header を 2 段で更新し、段 2 で vulkan_core.h を 1.4.309 で作り直す事を許可する（2026-10-30 ユーザーの Q318-3 の A の範囲。toolchain の変更として Q1 が sysroot の作り直しを行う）。
- Q324-3: libvulkan の H.265・encode の転送、capset の bit、命令の番号は WS324 が持つ（Q322-4 の「先に着手する方」）。WS322 の p005・p010 の libvulkan の部分は WS324 の物を使う。capset の bit は WS324 の案（2 = H.265 decode、3 = H.264 encode、4 = H.265 encode）に WS322 を揃える。
- Q324-5: encode は decode と同じ queue family 0 に置く。
- （2026-11-01 23:11）Q324-4: ユーザーから、NVENC の関門（p009b）に時間の上限を設けない、という趣旨の答えを受けた。動くまで続ける。
- （2026-11-01 23:11）Q324-6: ユーザーから、libmedia の道を RTX で確かめる Phase（p016）を WS324 の完了の条件から外す、という趣旨の答えを受けた。WS324 は driver と Vulkan の口で完了とし、libmedia での確かめは WS318 の受け入れに任せる。
- ws.md の「ユーザーの決定（2026-10-30、原文）」の節の扱いは、既にある原文の扱いがユーザーの未決（master.md）なので、今は変えない。

## Resume point

- 2026-11-01 P1（q1864-i01）: p006a 段 1。[h265-parse-design.md](h265-parse-design.md)、[proposed/h265/](proposed/h265/) の header 11 個、phase006b〜h の phase.md、design-review.md の review 2（20 件を全部反映）。次は Q1 の受け入れと Q-A〜Q-E（[phase006a](phase006a/phase.md)）、その後に段 2。
- 2026-11-01 P1（q1854）: p001 の design.md 第 2 版（review 1 の 31 件を反映、design-review.md）・proposed/・問い Q324-1〜6（phase.md）。次は Q1 の受け入れと問いの答え。

- 2026-11-01 21:46 Q1: ユーザーの指示で前倒し、p001 を P1（q1854）に。WS291 の P1 の返却の後に起動。
- 2026-10-30 Q1: 作成。WS318 の completed の後。
