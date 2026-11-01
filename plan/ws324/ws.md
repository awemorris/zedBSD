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
| [ws324-p006a](phase006a/phase.md) | H.265 の解析の設計（標準廉価実装手順 §4、[h265-parse-design.md](h265-parse-design.md)、header は [proposed/h265/](proposed/h265/)）と、header を source に置く段 2 | cleared（2026-11-02 00:41 Q1、main e1eaa6761。段 1 の設計と段 2 の BASE 1f16db0e6、check-skeleton・run-unit を main で流し直して PASS、HEVC の適合の 147 本を取得） | Q324-1 |
| [ws324-p006b](phase006b/phase.md) | h265sw-nal・h265sw-scaling・h265sw-vui の実装（Sonnet、BASE `1f16db0e6`） | cleared（2026-11-02 01:03 Q1、main で merge。test-nal 816099・scaling 5499・vui 2432070 checks、壊す確かめ 37 か所が全部試験の失敗で落ちた） | p006a 段 2 |
| [ws324-p006c](phase006c/phase.md) | h265sw-rps・h265sw-ps の実装（Sonnet） | cleared（2026-11-02 02:46 Q1、main で test-rps 1891521・test-ps 1447981 PASS）。実装（2026-11-02 Sonnet q1883-i01、test-rps 1891521・ps 1447981 checks、壊す確かめ 142 か所中 140 が試験の失敗で落ち 2 は等価、適合 147 本の VPS・SPS・PPS の読みは失敗 0）。cleared は Q1 | p006b |
| [ws324-p006d](phase006d/phase.md) | h265sw-slice・h265sw-poc の実装（Sonnet） | cleared（2026-11-02 03:26 Q1、main で test-slice 3904937・test-poc 1788280 PASS）。実装（2026-11-02 Sonnet q1899-i01、test-slice 3904937・poc 1788280 checks、壊す確かめ 252 か所中 243 が試験の失敗で落ち 9 は等価）。cleared は Q1 | p006c |
| [ws324-p006e](phase006e/phase.md) | h265-dpb の実装（Sonnet） | cleared（2026-11-02 03:43 Q1、main で test-dpb 40617273 PASS）。実装（2026-11-02、cleared は Q1） | p006a 段 2 |
| [ws324-p006f](phase006f/phase.md) | h265-std の実装（Sonnet） | cleared（2026-11-02 03:15 Q1、main で test-std 1680 PASS。zedBSD の clang は H.265 の Vulkan header が入る p002 の後）。実装（2026-11-02 Sonnet q1901-i01、test-std 1680 checks、壊す確かめ 220 通りが全部試験の失敗で落ちた）。cleared は Q1 | p006a 段 2 |
| [ws324-p006g](phase006g/phase.md) | h265-stream の実装（Sonnet） | 実装済み・試験 PASS（2026-11-02 Sonnet q1906-i01、test-stream 1331307 checks、壊す確かめ 994 か所中 889 が試験の失敗で落ち、残り 105 は等価・観測できない物（ENOMEM の道、冗長な初期化、unreachable の守り）。cleared は Q1） | p006d |
| [ws324-p006h](phase006h/phase.md) | H.265 の解析の適合の試験（HEVC_v1、host、Opus） | planned | p006e〜g |
| [ws324-p009a](phase009a/phase.md) | NVENC の channel（段 nvenc-channel、class の bind、firmware の確かめ、RC の通知の記録） | cleared（2026-11-02 01:07 Q1、main a43959d8e。10.0.10.3 で NVENC の channel（class c4b7、chid 49）が開き engine 自身が semaphore を release、GSP-RM が NVENC の ucode を持つ（追加の firmware 不要）、channel error 0、NVDEC は前と同じ） | 実機 |
| [ws324-p009b](phase009b/phase.md) | NVENC の関門（kernel の中の自己診断で I・P 絵、host の ffmpeg で decode） | uncleared（2026-11-02 04:23 P1、q1884-i01。H.264・H.265 とも 1 枚目の encode が HW の誤り（RC 47）、25 起動・約 75 通りの値で同じ。GSP の log の書式の metadata の判断待ち Q-p009b-1） | p009a、Q324-4 |

### 実装の Phase の案（2026-11-01 P1、[design.md](design.md) 第 2 版 §15。Q1 の受け入れの後に表へ）

| Phase | 目的 | 依存 |
| --- | --- | --- |
| p002 | Vulkan の header の段 1（H.265 decode の選び） | Q324-2（Q1 の toolchain の許可） |
| p003 | Vulkan の header の段 2（`vulkan_core.h` を 1.4.309 で作り直し、KHR の encode） | p002、Q324-2 |
| p004 | libvulkan の H.265 decode の転送、capset の bit 2、video の門を bit ごとに | p002、Q324-3 |
| p005 | nvrtx の H.265 decode（NVDEC の HEVC、executor、行の揃え 64、timeout の後の隔離） | p004 |
| p006a | H.265 の解析の設計（標準廉価実装手順）と header の配置（段 2） | Q324-1（p002 は要らない: host の Vulkan の header で試験） |
| p006b〜h | （上の Phase の表へ移した、2026-11-02 p006a 段 2。Q-B の A） | — |
| p006i | vkvideo-probe の H.265（前の案の p006e） | p006h、p002 |
| p007 | H.265 decode の実機の debug | p005、p006i、実機 |
| p008a | libvulkan の encode の転送（queue と H.264、命令の番号、video の query pool、capset の bit 3） | p003、Q324-3 |
| p008b | libvulkan の encode の転送（H.265、capset の bit 4） | p008a |
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

## Q1 の判断（2026-11-02 00:10、p006a の問い）

- Q-A〜Q-E は全部 A。Q-A: WS330 の proposed の解析の header を採って足す。Q-B: Phase は h265-parse-design.md §9 の形（p006a〜p006i、p007 の依存は p006i）。Q-C: 段 2 は P1 が p006a の 2 回目の起動で行う。Q-D: WS330 の適合の道具を plan/tools/h265/conformance/ に移し tools.md に登録、cache は build/sources/h265-conformance（Q1 が済ませた）。Q-E: NumDeltaPocsOfRefRpsIdx の約束は設計の案。

## Resume point

- 2026-11-02 P1（q1884-i01）: p009b。nvenc-test の段（`nvrtx.nvenc.test=1`）で H.264 の IDR を encode。firmware は setup を受けて（magic・offset の検査は通る、誤りの番号は 2024 版の enum）、rc_process と入力の luma・chroma を読んだ後、出力に触る前に HW の誤り（error_status 1、約 20 µs、GSP の RC 47）。H.265・ME only・IPCM・640x480・monochrome でも同じ。channel は RC の後に作り直せるので 1 起動で 5〜7 通り試せる。GSP の LOGRM に falcon の dump らしい 24 値があるが書式が無い（Q-p009b-1）。次は Q1 の判断（[phase009b](phase009b/phase.md) §4〜§5）。
- 2026-11-02 P1（q1880-i01）: p009a。NVENC の channel を実機（10.0.10.3）で開いた: chid 49・runlist 2・class c4b7、NOP と NVENC 自身の semaphore の release が通り、RC の通知 0。R5 は「GSP-RM が ucode を持つ、別の firmware は要らない」。機械は Debian に戻し cksum 一致。次は Q1 の受け入れと p009b（関門）。
- 2026-11-02 P1（q1864-i02）: p006a 段 2。header 11 個を `userland/desktop/libmedia/` に置き、骨格 .c 10 個、`plan/ws324/tests/`（run-unit.sh・check-skeleton.sh・unit/h265write.h・unit/test-h265write.c）、基点 BASE `1f16db0e6`。p006b〜h の phase.md に BASE と道具の注意（design §8.1.1）。次は Q1 の受け入れと p006b・p006e・p006f の起動。
- 2026-11-01 P1（q1864-i01）: p006a 段 1。[h265-parse-design.md](h265-parse-design.md)、[proposed/h265/](proposed/h265/) の header 11 個、phase006b〜h の phase.md、design-review.md の review 2（20 件を全部反映）。次は Q1 の受け入れと Q-A〜Q-E（[phase006a](phase006a/phase.md)）、その後に段 2。
- 2026-11-01 P1（q1854）: p001 の design.md 第 2 版（review 1 の 31 件を反映、design-review.md）・proposed/・問い Q324-1〜6（phase.md）。次は Q1 の受け入れと問いの答え。

- 2026-11-01 21:46 Q1: ユーザーの指示で前倒し、p001 を P1（q1854）に。WS291 の P1 の返却の後に起動。
- 2026-10-30 Q1: 作成。WS318 の completed の後。
