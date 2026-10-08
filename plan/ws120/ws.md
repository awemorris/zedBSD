<!-- awesome-plan project=zedbsd record=ws120 -->

# WS120: 音楽アプリ

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: p002〜p007 を canceled（2026-10-07 の決定で置き換え）。2026-10-08 q910 P2 の照合: p001・p008・p009 cleared。p002〜p007（2026-10-02 の WAV・FLAC・MP3・Ogg・独自 AAC の計画）は 2026-10-07 の決定（m4a＋libavcodec の add-in）で置き換わった → canceled にするかは Q1。service の連携はベータ4）（2026-10-07 q831 P2: p001 の決定で m4a＋libavcodec の add-in の local の player に。p008・p009 を実装）
Primary Milestone: MG006
Objectives: O2
Parent: [Master](../master.md)
Focused goal: fg019（ベータ1）
Queue: q831（2026-10-07、P2）
Resume point: p008（tags.c・library.c と host の試験）→ p009（app）。p001 の「2026-10-07 の決定と設計」に従う。以下の 2026-10-02・10-05 の行は経緯（形式と decoder は 2026-10-07 の決定で置き換え）。
2026-10-02 user:「対応する形式はベータ1ではm4aのみで開始します。decoder を独自に実装します。」→ ベータ1 の形式は m4a（MP4/ISO BMFF の container ＋ AAC）だけ。WAV・FLAC・MP3・Ogg の計画（p003・p004・p006）は置き換え、MP4 の demux と AAC-LC の decoder を独自（Zlib）に書く。p001 の設計で Phase を切り直す（AAC の profile の範囲、参照の decoder との一致の測り方、特許・license の確認）。
2026-10-05 user（方針の改訂、原文）:「音楽プレイヤーは、既存音楽サービスとの連携を重視します。ただし、YouTube Musicみたいなサービスもあるので、写真管理ソフトと動画プレイヤーよりも優先度を下げます。ローカルのファイルをコレクションにするプレイヤー機能も必要と思います。」→ (1) 既存の音楽の service との連携を重視する（どの service と、どう連携するか（公式の API・web の player・browser の中で再生）は p001 で調べてユーザーと決める）、(2) local の file を collection にする player の機能も要る（今の p001〜p006 の m4a の計画はこの部分）、(3) **優先度は写真の管理（WS157）と動画の player（WS122）より下**。
<!-- awesome-plan-current:end -->

## 目標（2026-10-02 ユーザー（ベータ1、リリース目標 10/17））

「音楽アプリを実装する。」

- Keiland の音楽アプリ（再生・一覧・音量は audiod/WS100 と共有）。対応形式・機能はユーザーと p001 で決める。

## 調べた現状（2026-10-02）

- **audiod**（`userland/base/audiod`、[設計](../ws035/audiod-design.md)）: mix する process。制御は `/run/audiod.sock`、音は stream ごとの共有メモリの ring（`AUDIOD_STREAM_CREATE` で SCM_RIGHTS の fd）。format は S16_LE・S32_LE・F32_LE、rate は線形補間で device（HDA は S16・2ch・48000 Hz）へ変換、stream の音量・drain・underrun の通知がある。**再生の stream を作る client の library は無い**（試験の `plan/ws035/tests/audiod-client.c` だけ）。
- **libkeiland の audio**（`userland/desktop/include/keiland/keiland.h` の `keiland_audio_*`、WS100 p003）: device の音量・mute・確かめの音・変化の通知だけ。zedBSD は audiod、Linux は ALSA の mixer の ioctl（PCM 再生なし）、FreeBSD は OSS の mixer。**PCM の再生の API は無い**。`KEILAND_VERSION` は 21。
- **音量**: system bar（WS100 p004）と Settings の Sound（p005）が `keiland_audio_set_volume` で device の音量を共有し、`desktop.conf` の `sound.volume`・`sound.muted` に保存。
- **既存の app の構成**: `userland/desktop/<app>/`（imageview・textedit・pdfviewer・notes など。`Makefile`・`Makefile.linux`・`Makefile.freebsd` で 3 OS を build、libkeiui の canvas・titlebar・menu・touch）。Files の関連付けは `userland/desktop/files/apps.c`（`APPS_IMAGE_TYPES` 等）と `mime.c`。
- **decoder の先例**: base の `libpng-compat`・`libjpeg-compat`・`libgif-compat`・`libz-compat` は、外部の code を使わない独自実装（Zlib）で、3 OS で build される。画像 viewer はこれを使う。
- QEMU の音の試験: `intel-hda`＋`hda-duplex` を WAV に録る仕組み（WS100 p002 の `audiod-qemu.sh`、`hda-wav-check.py`）。host に flac・lame・ffmpeg・vorbis-tools は無い（試験の素材を作るなら sudo で導入する）。

## ベータ1 の到達目標と受け入れ条件（案、D1〜D4 の既定案による）

| # | 基準 | 確かめ方 |
| --- | --- | --- |
| M1 | Files で `.wav`・`.flac`・`.mp3` を開くと Music が起動し再生する。録った WAV で、試験の信号（例 440 Hz の正弦波 3 秒）の始まり・長さ（±50 ms）・周波数（±1 Hz）が合う | QEMU（`intel-hda` を WAV に録る） |
| M2 | 再生・一時停止・次・前・seek（指定位置から ±0.5 s で再開）・経過と全体の時間の表示 | QEMU の自動の試験（入力の注入と WAV） |
| M3 | folder（既定 `~/Music`）の曲の一覧と metadata（題・artist・album・長さ）。ID3v2・Vorbis comment を読む | QEMU の画面と自動の試験 |
| M4 | 音量は system bar・Settings と同じ device の音量を共有する（app に別の音量 slider を置くなら同じ値を動かす） | QEMU の自動の試験 |
| M5 | decoder の host 試験: WAV・FLAC は参照の PCM と bit で一致、MP3 は参照の decoder との差が規定以内（p001 で数値を決める） | host 試験（`plan/ws120/tests/`） |
| M6 | audiod が無い・音の device が無い・壊れた file のとき、落ちずに理由を表示する | QEMU |
| M7 | Latitude 5330 の実機で鳴る（WS100 A7 に依存。デモの扱いと同じく必須ではない。未実施なら未実施と書く） | 実機（ユーザーの耳） |

## ユーザーの判断が要る点（p001 で選択肢を提示）

- **D1（対応形式）**: 候補 WAV（PCM 16/24 bit・float）・FLAC・MP3・Ogg Vorbis・Opus・AAC/M4A。**既定案: ベータ1 は WAV・FLAC・MP3、Ogg Vorbis は努力目標（p006、drop 可）**。Opus・AAC は範囲外（AAC は特許と仕様の大きさ、Opus は実装の大きさ）。MP3 の特許は 2017 年までに切れている。
- **D2（decoder の実装方針）**:
  - (A) **独自実装（Zlib、外部の code を参照・転記しない）**。base の `lib*-compat` と同じ方針。目安: WAV 約 300 行、FLAC 約 1500 行、MP3（Layer III）約 2500〜3000 行、Vorbis 約 3000〜4000 行。3 OS で build でき、license の境界が単純。仕様書（FLAC は RFC 9639、MP3 は ISO/IEC 11172-3 の公開の解説、Vorbis I の仕様）に基づく。
  - (B) **寛容な license の小さな外部 library を package にする**（dr_wav・dr_flac・dr_mp3（public domain／MIT-0）、stb_vorbis（public domain／MIT）、minimp3（CC0））。早いが、Keiland の desktop の app が外部 package に依存する（[設計方針 §2.1](../master-design-policy.md) の base と package の境界、3 OS の build への影響）。
  - (C) **参照実装を package にする**（libFLAC・libogg・libvorbis（BSD-3）、libopus（BSD-3）、mpg123（LGPL-2.1））。品質は高いが (B) と同じ境界の問題に加え、依存が増え LGPL を含む。
  - **既定案: (A)**。WAV・FLAC・MP3 をベータ1 の Phase に、Vorbis を努力目標に置く。decoder は app の中ではなく、画像と同じく base の library（案 `userland/base/libaudio-decode`、名前は p001）にして、将来の動画プレーヤ（WS122）の音声とも共有できる形にする。
- **D3（再生の API の場所）**: libkeiland に PCM の再生（`keiland_audio_stream_*`: open・write・pause・drain・位置）を足す。zedBSD は audiod の共有メモリの ring。Linux（ALSA の PCM の ioctl）・FreeBSD（OSS の `/dev/dsp`）を**ベータ1 で実装するか、ENOTSUP を返す stub にするか**。既定案: zedBSD を実装、Linux・FreeBSD は build が通る stub（音が出ないことを表示）にし、実装は後の Phase。
- **D4（機能の範囲）**: 既定案: file・folder を開く、一覧と metadata、再生操作、seek、cover 画像（埋め込みの PNG/JPEG を既存の compat の library で）、Files からの起動。playlist の保存・shuffle・repeat・gapless・window を閉じた後の背景再生・media key は範囲外（後で）。

## 他 WS との接点

- libkeiland・keiland.h（WS104 で OS module に分けた。`KEILAND_VERSION` を上げる）: p002 で変更する。Linux・FreeBSD の build（WS105・WS109 の成果）を壊さない。
- Files の関連付け（`userland/desktop/files/apps.c`・`mime.c`、WS127 が Files を磨いている）: p005 で audio の MIME と app を足す。WS127 の作業と衝突しないよう main が順を調整する。
- WS122（動画プレーヤ、別 session）: decoder の library と再生の API を共有できる。WS122 の設計を縛らないよう、API は p001 で最小にする。
- audiod は変えない見込み（変えるなら main に依頼）。

## Phase

| Phase | 目的 | Status | 依存 | 目安 |
| --- | --- | --- | --- | --- |
| [ws120-p001](phase001/phase.md) | 設計: 2026-10-07 のユーザーの決定（m4a、libavcodec の add-in、service の連携はベータ4）と app の構成 | cleared（q831、P2） | — | — |
| [ws120-p002](phase002/phase.md) | libkeiland の PCM 再生の API | canceled（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: 2026-10-07 の決定で置き換え。旧: planning、**canceled**（2026-10-07: videoplayer の audiod の client を使う、共有…） | — | — |
| [ws120-p003](phase003/phase.md) | decoder: WAV・FLAC | canceled（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: 2026-10-07 の決定で置き換え。旧: planning、**canceled**（2026-10-07: 形式は m4a だけ）） | — | — |
| [ws120-p004](phase004/phase.md) | decoder: MP3 | canceled（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: 2026-10-07 の決定で置き換え。旧: planning、**canceled**（2026-10-07: 形式は m4a だけ）） | — | — |
| [ws120-p005](phase005/phase.md) | Music の app の MVP（WAV・FLAC・MP3） | canceled（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: 2026-10-07 の決定で置き換え。旧: planning、**canceled**（2026-10-07: p008・p009 に置き換え）） | — | — |
| [ws120-p006](phase006/phase.md) | Ogg Vorbis | canceled（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: 2026-10-07 の決定で置き換え。旧: planning、**canceled**（2026-10-07: 形式は m4a だけ）） | — | — |
| [ws120-p008](phase008/phase.md) | m4a の metadata と `~/Music` の collection（tags.c・library.c）、host の試験 | cleared（2026-10-07、T1-315） | p001 | — |
| [ws120-p009](phase009/phase.md) | Music の app（一覧・再生・seek・次・Files・App Home）、libavcodec の add-in と audiod | cleared（2026-10-07、T1-315） | p008 | — |
| [ws120-p007](phase007/phase.md) | 全文規約と回帰、制限の整理（必須の最終確認、後回し） | canceled（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: 2026-10-07 の決定で置き換え。旧: planning） | p009 | 2h |

Graph（2026-10-07）: p001 → p008 → p009 → p007。p002〜p006 は canceled。

## Event history

2026-10-02 / ws120-plan-detail-20261002: 計画担当が audiod・libkeiland の audio・WS100・既存の app と compat の decoder を調べ、D1〜D4 の選択肢と既定案、受け入れ条件、Phase を詳細化。p001 planned、Queue none。実装はしていない。
