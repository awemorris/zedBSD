<!-- awesome-plan project=zedbsd record=ws202 -->

# WS202: libavcodec なしの H.264＋AAC の mp4 と .m4a の再生（libmedia の自前の decoder、Video Player・Music）

<!-- awesome-plan-current:start -->
Status: planning（2026-10-10 設計の初版。design-reviewer の review と、下の人の判断 H1〜H6 の回答の後に planned）
Primary Milestone: MG006
Related Milestones: —
Objectives: O2
Parent: [Master](../master.md)
Target: **ベータ3**（判断の点 H4「ベータ2 に入れるか」）
Queue: —
Resume point: [design.md](design.md) 第 1 版の review（design-reviewer）→ H1〜H6 の回答 → p002 から。実装は別のセッション（2026-10-11 ユーザー）。
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

## 範囲

- libmedia（`userland/desktop/libmedia/`）: 自前の back end 2 つ（Vulkan Video の H.264、AAC-LC）、共通の部品（bit の reader、NV12 の picture と
  BGRA への scaler、音の downmix・resample）、back end の表と問題の code。
- mediafile（`userland/desktop/mediafile/mp4.c`）: pasp・colr・表示の終わりの読み（mp4 の demuxer は既にあり、fragmented も読む）。
- Video Player・Music: notice の文、起動の門の除去、縦横比、log。
- 試験: host 試験、試験の道具 `media-probe`（`userland/tests/`）、試験の stream、QEMU と 5330 の確認（T1）、5330 の UAT。
- 利用者の文書（`docs/reference/` に libmedia の再生の範囲）。

## 非範囲

H.264 の CPU の decoder（H1）、HE-AAC の SBR・PS の再現（H3）、他の codec（libavcodec の add-in のまま）、encode、interlaced・10 bit、AAC の Main・
LTP・SSR・ER 系、DRM、streaming、GPU の image の共有の表示（H6）、browser の `<video>` の UAT（engine は自動で同じ decoder を使うが、WS074 は
ベータ3 で止めている）、libvulkan・i915 の変更（WS083 の範囲）。

## 完了の条件

1. libavcodec を入れない image（`plan/ws202/tests/config-media-hw.mk`）の 5330 で、`media-probe --video-hash --expect` が H.264 の試験の
   stream の全部（WS083 の 6 本の mp4、B・open GOP・crop・SAR）と 1080p の sample の全 frame で参照と一致し、`--audio-rms` が AAC の全 stream で
   基準の中（design §10.3）。
2. host 試験（design §10.2）が全部 PASS: AAC の float の出力が host の ffmpeg と基準の中（max 2^-14・RMS 2^-17、PNS は ±1 dB）、H.264 の parser と
   DPB が vkvideo-probe と全 picture で一致、vkvideo の back end の偽の Vulkan の試験。
3. libavcodec を入れない QEMU の image で Music の scenario（`tests/scenarios/apps/music/play.md`、`backend=libmedia`）が PASS、Video Player が
   DEVICE の notice を出す。libavcodec を入れた回帰の image で `tests/scenarios/apps/videoplayer/play.md` が PASS。
4. 5330 の UAT（ユーザー）で、Video Player の音と絵の同期・seek・一時停止・全画面・終わり、Music の .m4a の再生・曲の切り替え・seek に問題が無い。
5. build（`make -j16`、warning 0）。全文規約の見直し（p014）。

## 依存

- [WS083](../ws083/ws.md)（Vulkan Video、H.264 の decode。p001〜p006・p008 cleared、既定で ON）。WS083 の p007（hang の回復の実機）は依存しない
  （hang の時は DEVICE_LOST で decoder を失敗にするだけ）。
- WS122（Video Player）・WS120（Music）・WS191（音の stream）・ws177-p031（libmedia の decoder の表）の今の code の上に作る。
- 人の判断 H1・H2・H3・H5 の回答（p005・p009 の前）。

## Phase

見積もりの LW は 1 LW ≈ エージェントの実時間 20 分。合計 **62 LW**（約 21 時間）。p003 の後、AAC の列（p005→p006→p007）と H.264 の列
（p008→p009→p010）は独立で、2 人の担当で並べられる。

| Phase | 目的 | 見積もり | Status | 依存 |
| --- | --- | --- | --- | --- |
| [ws202-p001](phase001/phase.md) | 設計（[design.md](design.md)）と review | 3 | in-progress（第 1 版を書いた、review 待ち） | — |
| [ws202-p002](phase002/phase.md) | 試験の stream と参照（`make-streams.sh`）、host 試験の共通の枠 | 3 | planned | p001 |
| [ws202-p003](phase003/phase.md) | libmedia の共通の部品: bit の reader、NV12 の picture と pool と scaler、音の変換、back end の表・問題・2 段の試し・`media_decoder_backend`・`media_frame_aspect` | 6 | planned | p001、H2 |
| [ws202-p004](phase004/phase.md) | mediafile: pasp・colr・表示の終わり（`end_us`） | 2 | planned | p001 |
| [ws202-p005](phase005/phase.md) | AAC の構文: ASC、要素、ICS、section・scalefactor・pulse・TNS の data、spectral の Huffman、表の生成 | 7 | planned | p002、p003、H3、H5 |
| [ws202-p006](phase006/phase.md) | AAC の信号処理と back end: 逆量子化・PNS・M/S・intensity・TNS・IMDCT・窓・overlap、切り詰め、ffmpeg との精度の試験 | 8 | planned | p004、p005 |
| [ws202-p007](phase007/phase.md) | Music: 起動の門の除去、notice、log の backend、host の codec 試験と scenario の更新 | 2 | planned | p006 |
| [ws202-p008](phase008/phase.md) | H.264 の parser と DPB（probe から、AU 単位、VUI、POC type 1、表示順の深さ）、probe との一致の試験 | 7 | planned | p002、p003 |
| [ws202-p009](phase009/phase.md) | Vulkan Video の back end (1): dlopen、共有の device、capability、session・parameters・image・buffer、decode、result status、de-tile | 7 | planned | p008、H1 |
| [ws202-p010](phase010/phase.md) | Vulkan Video の back end (2): 表示順の待ち行列と pts、flush・seek・先頭の picture、SPS の変更、失敗の扱い、偽の Vulkan の試験の仕上げ | 5 | planned | p009 |
| [ws202-p011](phase011/phase.md) | Video Player（notice・縦横比・log・late の数）、試験の道具 `media-probe`、試験の image の config、利用者の文書 | 4 | planned | p007、p010 |
| [ws202-p012](phase012/phase.md) | T1: QEMU（libavcodec 無し・有り）と 5330 の実機の hash・RMS・時間を 1 回で | 3 | planned | p011 |
| [ws202-p013](phase013/phase.md) | 5330 の UAT（ユーザー）と結果の反映 | 2 | planned | p012 |
| [ws202-p014](phase014/phase.md) | 全文規約の見直し（`plan/coding-style.md` の全文と WS の全 C の差分） | 3 | planned | p013 |

## 人の判断の点（各々に推し、詳しくは design §13）

| ID | 問い | 選択肢 | 推し |
| --- | --- | --- | --- |
| H1 | Vulkan Video の無い機械（Venus の QEMU、他の GPU）の H.264 | (a) エラーの表示、libavcodec があればそれ／(b) WS202 に CPU の H.264 decoder（+45〜60 LW、1080p30 の性能は不明） | **(a)**。CPU の decoder は別の WS（Future）。Windows の QEMU の配布物は libavcodec を入れているので今まで通り再生できる |
| H2 | libavcodec の入った build | (a) 両方、自前が先、release は libavcodec のまま／(b) libmedia だけ（HEVC・VP9・MP3・Opus と QEMU の H.264 が再生できなくなる）／(c) 両方、libavcodec が先 | **(a)**。試験の image は libavcodec を入れずに自前の経路を確かめる |
| H3 | HE-AAC（SBR・PS） | (a) core の AAC-LC だけ鳴らす（帯域が半分）、libavcodec があればそちら／(b) SBR（+14 LW）・PS（+7 LW）を足す／(c) 断る | **(a)**。.m4a の大半は AAC-LC。host の ffmpeg は HE-AAC を作れず試験の stream が手元に無い |
| H4 | ベータ2 に入れるか | 入れる／入れない（ベータ3） | **入れない（ベータ3）**。62 LW と review・T1・UAT で RC（10/13）に間に合わない。ベータ2 は libavcodec で今まで通り再生できる |
| H5 | AAC の data の表（Huffman・scalefactor band）の出典 | (a) FFmpeg 9.0.2 の tarball から値だけを script で生成（WS141 と同じ「値は事実」）／(b) ユーザーが ISO/IEC 14496-3 を用意／(c) Apache-2.0 の別の実装から | **(a)**。code は写さず、名前・並び・型は自前、生成の file に出典と SHA-256。窓・FFT・`x^(4/3)` は計算で作る |
| H6 | 表示を GPU の image の共有にするか | 今は CPU への写し（de-tile して NV12、scaler で BGRA）／GPU の経路（libvulkan・i915 に SAMPLED、libkeiland の描画の変更） | **CPU**。性能が足りなければ別の WS |

## 制限（設計の時点の見込み）

- H.264 は 5330（i915 Gen12）だけ。Venus の QEMU では libavcodec があればそれ、無ければ notice。
- de-tile は zedBSD の libvulkan の約束（OPTIMAL の NV12 が Intel Tile Y）に頼る。他の Vulkan の実装では使えない。
- 途中で解像度・codec が変わる mp4（stsd の複数の entry）は扱わない。
