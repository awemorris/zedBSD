<!-- awesome-plan project=zedbsd record=ws318-questions -->

# WS318 ユーザーの判断を待つ点（p001、2026-10-31 P5）

- WS: [WS318](ws.md)「libmedia の作り直し: Vulkan の device の共有・codec の問い合わせと指定・encoder・camera・libavcodec の内部の fallback、Keiland の app を libmedia だけに」。
- 状態: planning。p001（設計）を P5 が書いた（[design.md](design.md)）。実装は WS326（H.264 の software の decoder）の completed の後にすぐ始める予定（WS326 に触れない Phase は先に始められる、design §14）。
- 決まっていない事: 下の Q318-1〜9。どれも推しの案があり、推しのままなら設計はそのまま実装に入れる。
- 用語:
  - **Vulkan Video**: GPU の動画の decode・encode の hardware を Vulkan の命令で使う標準の口。zedBSD は hardware の codec を全部これで見せる（ユーザーの決定 5）。
  - **driver の WS**: 各 GPU（i915・Pi 4・Pi 5・VF2・ODROID-C5・RTX・RDNA）で Vulkan Video の足りない codec を作る WS319〜WS325。WS318 の completed の後に始める（ユーザーの決定 6）。
  - **vkfake**: この WS で作る試験の道具。host の CPU で動く Vulkan（lavapipe）の上に、偽の「動画の queue」を差し込み、libmedia が正しい順序と値で Vulkan Video を呼ぶかを記録して確かめる。本物の hardware の画質や正しい bitstream は確かめられない。
  - **libavcodec**: FFmpeg の codec の library。zedBSD では別の package（LGPL）で、libmedia が有れば dlopen して使う。

## Q318-1 driver が無い機能の確かめ方

背景: libmedia に作る Vulkan Video の encode（H.264・H.265）と H.265 の decode は、今どの GPU の driver も持っていない（WS319〜WS325 で作る）。WS318 を先に完了させる決定（6）なので、WS318 の中では実の hardware で動かせない。

- **A（推し）**: WS318 では vkfake で呼び方と値を確かめて完了にする。実の hardware での確かめ（bitstream の正しさ・画質）は WS319（i915）の最初の受け入れにし、そこで見つかった libmedia の誤りは WS319 の範囲で直す。
- B: これらの backend を WS318 から外し、最初の driver の WS（WS319）で作る。
- C: WS318 を driver が 1 つできるまで完了にしない。

## Q318-2 libavcodec の encode の fallback と zedBSD の package

背景: 決定 4 で「hardware の encoder が無い時は libmedia の中で libavcodec に fallback」とした。しかし zedBSD の libavcodec の package は LGPL の build で encoder を全部無効にしている。また FFmpeg 自身の LGPL の encoder に H.264・H.265 は無い（使えるのは GPL の x264・x265 か、別の library の openh264）。

- **A（推し）**: package は今のまま（LGPL、encoder 無し）。libmedia の encode の fallback は作り、encoder を持つ libavcodec が入っている system（利用者が自分で入れた物、host の Debian など）で働く。zedBSD の標準の image では、H.264 の software の encode は WS329（独自）の完了までは無い。
- B: GPL の build の libavcodec（x264・x265 入り）を別の package として作る（package の license は GPL、libmedia は dlopen で使うだけ）。
- C: openh264（BSD、Cisco の特許の扱いに条件がある）を package にして libavcodec から使う。

## Q318-3 Vulkan の header の更新（承認）

背景: zedBSD の Vulkan の header（`include/libc/vulkan/`）は Khronos の 1.3.269 から作った物で、encode の正式の拡張（KHR）と H.265 の構造体が入っていない。encode と H.265 の decode には新しい header が要る。header は全 build が使う sysroot に入るので、作り直しは Q1 が行う（toolchain の扱い）。HAL は変えない。

- **A（推し）**: Khronos の Vulkan-Headers の 1.3.274 以降の固定の tag（KHR の encode が正式になった版。具体の tag は p011 で一覧を見て決め、出典の記録 `API-PROVENANCE.md` に書く）で header を作り直す。今の decode の使い手（libvulkan・libmedia・i915 の executor）を build で確かめる。
- B: 更新しない（encode と H.265 の decode を WS318 から外す。Q318-1 の B と同じ結果）。

## Q318-4 MJPEG の camera の decode

背景: 多くの webcam は大きな画面・高い frame rate を MJPEG（JPEG の連続）でしか出さない。JPEG の decode は Vulkan Video に無いので libmedia が行う。2026-10-12 の決定（画像の処理は GPU で、毎 frame の経路で CPU で画素を触らない）との兼ね合いがある。

- **A（推し）**: 符号の解読（Huffman、順番に読むしかない部分）だけを CPU、逆変換（IDCT）と色の変換を GPU の compute で。camera が同じ大きさ・rate で YUYV・NV12 を出せる時はそちらを先に選ぶ。
- B: まず全部を CPU（既存の独自の libjpeg-compat）で decode し、GPU 化は後の Phase。Pi 4 などでは重い。
- C: MJPEG は今は扱わない（YUYV・NV12 の mode だけ）。

## Q318-5 AAC-LC の encoder に足す物

背景: 決定 6 で AAC-LC の encode を独自に（profile は decoder と同じ）。既存の encoder（WS205、Bluetooth 用）は長い窓だけ・TNS 無し・PNS 無し・1〜2 channel・32/44.1/48 kHz。用語: **短い窓（窓の切り替え）**は打楽器などの急な音の前に出る「前のにじみ」を防ぐ仕組み、**TNS** は時間方向の雑音の形を整える道具、**PNS** は雑音らしい帯域を「雑音」とだけ送って bit を節約する道具（低い bitrate で効く）。

- **A（推し）**: 窓の切り替え（短い窓）と TNS を足す。PNS は足さない。rate は decoder が受ける 8〜96 kHz、channel は 1〜2（5.1 などは後）。
- B: 窓の切り替えだけ。
- C: 今のまま（encoder の口に載せるだけ）。

## Q318-6 camera の実機の確かめと WS282

背景: camera の libmedia の口は作れるが、実の camera から frame を取る UVC の driver（WS282）は実装済み・実機の試験が未実施で保留中（webcam の購入も未決）。WS318 の completed を driver の WS（WS319〜）の前提にしているので、camera の実機を待つと driver の WS も遅れる。

- **A（推し）**: camera の実機の Phase（p027）だけを WS282 の実機の確かめの後に残し、それ以外の WS318 の Phase が終われば driver の WS（WS319〜）を始めてよいことにする。
- B: WS318 の受け入れから camera の実機を外し、WS282（か WS285）の受け入れに移す。
- C: WS282 の実機の確かめを先に行い、WS318 を全部終えてから driver の WS。

## Q318-7 file への書き出し（録画）

背景: camera → encoder の流れは作るが、encode した結果を MP4 などの file にする部分（muxer）は今の libmedia に無い（読む側だけ）。

- **A（推し）**: この WS の範囲の外。録画の app を作る時に新しい WS で。
- B: MP4 の書き出しをこの WS に足す。

## Q318-8 H.265 の範囲

背景: H.265 の decode・encode を Vulkan Video で扱う。10 bit（Main 10）は出力の形式（P010）と表示・変換の道が別に要る。

- **A（推し）**: Main（8 bit 4:2:0）だけ。Main 10 は後の WS。
- B: Main 10 も入れる。

## Q318-9 hardware の encode の B-frame

背景: B-frame は前後の frame を参照して圧縮を上げるが、並べ替えと遅れが増え、camera の生の配信には向かない。

- **A（推し）**: 最初は IDR と P だけ（B-frame 無し）。問い合わせには device の B-frame の数を出し、後で足せる形にする。
- B: B-frame も入れる。
