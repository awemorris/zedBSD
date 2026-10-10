<!-- awesome-plan project=zedbsd record=ws202 -->

# WS202: libavcodec なしの H.264＋AAC の mp4 と .m4a の再生（libmedia の自前の decoder、Video Player・Music）

<!-- awesome-plan-current:start -->
Status: planning（2026-10-10 設計の第 2 版（[review-001](review-001.md) を反映）。人の判断 H1〜H6・J1〜J6 の回答の後に planned）
Primary Milestone: MG006
Related Milestones: —
Objectives: O2
Parent: [Master](../master.md)
Target: **ベータ3**（判断の点 H4「ベータ2 に入れるか」）
Queue: —
Resume point: [design.md](design.md) 第 2 版 → H1〜H6・J1〜J6 の回答（必要なら 2 回目の review）→ p002 から。実装は別のセッション（2026-10-11 ユーザー）。
<!-- awesome-plan-current:end -->

## 由来（2026-10-11 ユーザー）

「libavcodecなしで、我々の独自のlibmediaの機能だけで、H.264とAACのmp4動画を再生できるようにしてください。また、libavcodecなしで、音楽アプリがlibmediaを使って.m4aを再生できるようにしてください。これはWSを立てて、設計をかいて」
「libmediaと動画アプリ、音楽アプリは、WSを立てて設計だけ書いてください。実装は別なセッションで行います。」

関係する前の決定: 2026-10-07 ユーザー（WS120 音楽）「AAC は今は libavcodec、独自は後」（独自の AAC-LC は後の Phase で表の出典を決める → H5）。
2026-10-11 ユーザー: Vulkan Video（WS083）は既定で ON。

## 目的

- libavcodec の無い image で、Video Player が H.264（Baseline・Main・High、8 bit 4:2:0 progressive）＋AAC-LC の mp4 を、音つき・同期・seek・
  終わりまで再生する。H.264 は Vulkan Video（WS083、i915 の VCS0）で decode する。
- libavcodec の無い image で、Music が .m4a（AAC-LC）を libmedia の自前の decoder で再生する。
- libavcodec の入った image で、今再生できる file（TS・mkv・AVI の H.264・AAC、ctts の無い mp4）を悪くしない（自前の back end が表の先頭に入るため）。

## 範囲

- libmedia（`userland/desktop/libmedia/`）: 自前の back end 2 つ（Vulkan Video の H.264、AAC-LC）、共通の部品（bit の reader、NV12 の picture と scaler、
  音の downmix・resample）、back end の表・問題の code・2 段の試し・音の約束・切り詰め。
- mediafile（`mp4.c`）: pasp・colr・表示の終わり。
- Video Player・Music: notice、起動の門の除去、縦横比、log、seek の後の切り詰め。
- 試験: host 試験、試験の道具 `media-probe`（`userland/tests/`）、試験の stream、QEMU と 5330 の確認（T1、p010 の小さい確認と p012）、5330 の UAT。
- 利用者の文書（`docs/reference/media-playback.md`）。

## 非範囲

H.264 の CPU の decoder（H1）、HE-AAC の SBR・PS の再現（H3）、他の codec、encode、interlaced・10 bit、AAC の Main・LTP・SSR・ER 系、DRM、streaming、
GPU の image の共有の表示（H6）、browser の `<video>` の UAT、libvulkan・i915 の変更（WS083 の範囲）、`iTunSMPB`、再生の途中の libavcodec への切り替え（J6）。

## 完了の条件

1. libavcodec を入れない image（`plan/ws202/tests/config-media.mk`）の 5330（USB で起動、SSH）で、`media-probe --video-hash --expect` が H.264 の試験の
   stream（WS083 と同じ素材の 6 本の mp4、B・open GOP・crop・SAR・ctts 無し）と合成の 1080p（J5）の全 frame で参照と一致し、`--seek` の後の frame も一致。
   `--audio-rms` が AAC の全 stream で基準の中（design §10.3）。
2. host 試験（design §10.2）が全部 PASS: AAC の精度（max 2^-14・RMS 2^-17、PNS・intensity の band の比べ）、H.264 の parser・DPB（probe との一致と、gap・
   MMCO 5・POC type 1・表示順の試験）、vkvideo の偽の Vulkan の試験、`run-host-codec.sh` の回帰（TS・mkv・AVI・ctts 無しの mp4）。
3. libavcodec を入れない QEMU の image で Music の scenario（`backend=libmedia`）が PASS、Video Player が DEVICE の notice を出す。libavcodec を入れた回帰の image で
   `tests/scenarios/apps/videoplayer/play.md` が PASS。
4. 5330 で 1080p30 の再生中の compositor の fps が再生していない時の 90% 以上（満たさない時は Q1 がユーザーに示す）。
5. 5330 の UAT（ユーザー）で、Video Player の同期・seek・一時停止・全画面・終わり、Music の .m4a の再生・曲の切り替え・seek に問題が無い。
6. build（warning 0）。全文規約の見直し（p014）。残す回帰の試験を master.md の Tools・試験の一覧に登録した（p014）。

## 依存

- [WS083](../ws083/ws.md)（Vulkan Video、H.264 の decode。p001〜p006・p008 cleared、既定で ON）。
- WS083 p007（VCS の hang の回復、実機は未確認）: WS202 は hang の時に decoder を失敗にし、最後の close で instance を作り直す（design §5.4）。再生を続けられるか
  （新しい kernel session で video が戻るか）は p007 の実機の結果に依る。p007 が実機で動かなくても WS202 の完了の条件には入れない（hang は条件に無い）。
- WS122（Video Player）・WS120（Music）・WS191（音の stream）・ws177-p031（decoder の表）の今の code の上に作る。
- 人の判断: H2・J1（p003 の前）、H3・H5（p005 の前）、J4（p015 の前）、H1・J6（p009 の前）、J5（p002 の前）。

## Phase

見積もりの LW は 1 LW ≈ エージェントの実時間 20 分。合計 **75 LW**（約 25 時間）。p003 の後、AAC の列（p005→p006→p007）と H.264 の列
（p008→p015→p009→p010）は独立で、2 人の担当で並べられる。

| Phase | 目的 | 見積もり | Status | 依存 |
| --- | --- | --- | --- | --- |
| [ws202-p001](phase001/phase.md) | 設計（[design.md](design.md)）と review | 4 | in-progress（第 2 版、判断の回答待ち） | — |
| [ws202-p002](phase002/phase.md) | 試験の stream と参照（`make-streams.sh`、x264 で直に mp4、ADTS・mkv・TS・AVI・ctts 無し、合成の 1080p）、host 試験の枠 | 4 | planned | p001、J5 |
| [ws202-p003](phase003/phase.md) | 共通の部品: bits、picture と pool と scaler、sound（64 tap の resampler、音の約束、trim）、back end の表・問題・2 段の試し・新しい口 | 7 | planned | p001、H2、J1 |
| [ws202-p004](phase004/phase.md) | mediafile: pasp・colr（nclx・nclc）・`end_us` | 2 | planned | p001 |
| [ws202-p005](phase005/phase.md) | AAC の構文（ASC・ADTS・要素・ICS・Huffman）、表の生成、HE-AAC の signalling の試験 | 8 | planned | p002、p003、H3、H5 |
| [ws202-p006](phase006/phase.md) | AAC の信号処理と back end（M/S の除外、切り詰め、trim、ADTS の入力）、精度の試験 | 9 | planned | p004、p005 |
| [ws202-p007](phase007/phase.md) | Music（起動の門、notice、log、trim、AAT の helper と scenario） | 3 | planned | p006 |
| [ws202-p008](phase008/phase.md) | H.264 の parser と DPB の写し（AU 単位、VUI、POC type 1、表示順の深さ）、probe との一致の試験 | 7 | planned | p002、p003 |
| [ws202-p015](phase015/phase.md) | H.264 の frame_num の gap・MMCO 5・seek の後の DPB・冗長 slice、conformance の stream の試験 | 4 | planned | p008、J4 |
| [ws202-p009](phase009/phase.md) | Vulkan Video の back end (1): dlopen、共有の device（1.0・properties2、作り直せる）、capability・level、session・image・decode、D18、de-tile | 7 | planned | p015、H1、J6 |
| [ws202-p010](phase010/phase.md) | Vulkan Video の back end (2): POC の表示順と時刻、in-band だけの track、flush・seek、失敗と作り直し、表の先頭へ、media-probe の video、`config-media.mk`、5330 の小さい確認（T1） | 8 | planned | p009 |
| [ws202-p011](phase011/phase.md) | Video Player（notice・縦横比・log・trim）、media-probe の音、利用者の文書 | 3 | planned | p007、p010 |
| [ws202-p012](phase012/phase.md) | T1: QEMU（libavcodec 無し・有り）と 5330 の実機を 1 回で | 3 | planned | p011 |
| [ws202-p013](phase013/phase.md) | 5330 の UAT（ユーザー） | 2 | planned | p012 |
| [ws202-p014](phase014/phase.md) | 全文規約の見直しと、残す試験の登録（master.md の Tools）・開発だけの試験の削除の依頼 | 4 | planned | p013 |

### 並べて進める時の merge の順（L-13）

共有の file（`decoder.c` の表、`media-private.h`、`media-decoder.h`、`Makefile`、`plan/tools/media/run-host-codec.sh` の期待）を 2 つの列が変える。

1. p003 を先に merge（表は `{ avcodec }` のまま、ops の型・口・問題の code を確定）。p004 は独立に merge してよい。
2. AAC の列（p006）は表に `media_aac_ops` を足して merge。H.264 の列（p010）は p006 の merge の後の main に rebase し、表の先頭に `media_vkvideo_ops` を足す。
   p006 が先に終わらなければ、p010 は表の変更を Q1 に送り、Q1 が p006 の後に当てる。
3. `run-host-codec.sh` の期待（WS の外の file）は各列が差分を Q1 に送り、Q1 が merge の順に当てる。`Makefile` の `LIBMEDIA_SOURCES` は行ごとに足すので衝突は Q1 が解く。

### WS の終わり（L-11、2026-10-06 の試験の整理の基準）

p014 で: 今後も回帰に使う試験（`make-streams.sh`、`run-host-aac.sh`・`run-host-h264.sh`・`run-host-vkvideo.sh`・`run-host-picture.sh`・`run-host-sound.sh`、
`media-probe`）を `plan/tools/media/` へ移し master.md の Tools・試験の一覧に登録する依頼を Q1 に出す。開発の途中だけの試験は削除の path を Q1 に送る（rm は Q1）。
完了の後、ws.md を完了の形に書き直し、Phase の directory の削除は Q1。

## 人の判断の点（各々に推し、詳しくは design §13）

| ID | 問い | 選択肢 | 推し | 理由 |
| --- | --- | --- | --- | --- |
| H1 | Vulkan Video の無い機械（Venus の QEMU、他の GPU）の H.264 | (a) notice、libavcodec があればそれ／(b) WS202 に CPU の decoder（+45〜60 LW） | **(a)** | CPU の decoder は残り全部より大きく 1080p30 の性能が不明。Windows の QEMU の配布物は libavcodec を入れている |
| H2 | libavcodec の入った build | (a) 両方、自前が先、release は libavcodec のまま／(b) libmedia だけ／(c) 両方、libavcodec が先 | **(a)** | (b) は HEVC・VP9・MP3・Opus と QEMU の H.264 を失う。(c) は自前の経路が普段使われず試験が薄くなる |
| H3 | HE-AAC（SBR・PS） | (a) core の AAC-LC だけ鳴らす（2 段目だけ）、libavcodec があればそちら／(b) SBR（+14 LW）・PS（+7 LW）／(c) 断る | **(a)** | .m4a の大半は AAC-LC。host の ffmpeg は HE-AAC を作れず試験の stream が無い |
| H4 | ベータ2 に入れるか | 入れる／入れない | **入れない（ベータ3）** | 75 LW と review・T1・UAT で RC（10/13）に間に合わない。ベータ2 は libavcodec で今まで通り再生できる |
| H5 | AAC の data の表の出典 | (a) FFmpeg 9.0.2 の tarball の `aactab.c` から値だけを生成（WS141 と同じ「値は事実」）／(b) ユーザーが ISO/IEC 14496-3 を用意／(c) Apache-2.0 の別の実装から | **(a)** | aactab.c は data として読める（design §15 E6）。code は写さず名前・並び・型は自前。guardrail の「値は事実」は WS141・WS037 だけなので許可が要る |
| H6 | 表示を GPU の image の共有にするか | CPU への写し／GPU の経路（libvulkan・i915・libkeiland の変更） | **CPU** | 出力の image は SAMPLED を持たない（WS083 HD5）。性能が足りなければ別の WS |
| J1 | 自前の back end を全 container に使うか（H2 (a) の時） | 全 container（回帰の試験を受け入れに）／mp4・m4a だけ（他は libavcodec のまま） | **全 container** | 第 2 版で入力の形と表示順を直し回帰の原因を除いた。絞ると TS の AAC 等が libavcodec 無しで鳴らない |
| J2 | browser の engine も自前の H.264 を使うか | 使う／engine だけ libavcodec | **使う** | 同じ道で code が増えない。browser は止めていて UAT の外。video の context を 1 つ使うだけ |
| J3 | AAC・H.264 の decoder を base の libmedia に入れる特許の扱い | 進める（release の文書に記す）／任意の add-in に分けたまま | **進める** | 今の release は同じ decoder を持つ libavcodec を既に配っており、危険は増えない。法律の判断はユーザー |
| J4 | MMCO 5・frame_num の gap・冗長 slice の試験の stream | ITU-T H.264.1 の conformance の bitstream を tree の外で使う／手で bit 列を作る | **conformance を tree の外で** | 手の bit 列は slice の data が無く decode の確かめにならない。入手と利用の条件は p015 で確かめる（U13） |
| J5 | 完了の条件の 1080p の試料と参照 | 合成の 1080p を make-streams.sh が `build/` に作り T1 が scp／tree の外の sample と T1 がその場で作る参照 | **合成** | 完了の条件を tree の外の物と記録の無い参照に頼らない。sample は UAT に使う |
| J6 | 再生の途中の DEVICE_LOST | その file の再生を失敗にする／次の IDR から libavcodec へ切り替え（+2 LW） | **失敗にする** | 切り替えは時計の合わせが要り、hang は稀 |

## 制限（設計の時点の見込み）

- H.264 は 5330（i915 Gen12）だけ。Venus の QEMU では libavcodec があればそれ、無ければ notice。
- de-tile は zedBSD の libvulkan の約束に頼る。他の Vulkan の実装では使えない。
- 途中で解像度・codec が変わる mp4、`iTunSMPB` だけの priming は扱わない。in-band だけの H.264（TS・AVI・avc3）は libavcodec の無い時だけ自前が受け、範囲の外の
  profile は open の後に分かる（notice にならず再生の失敗）。
- kernel の video の context は 8 個で、9 個目の process は BUSY。
