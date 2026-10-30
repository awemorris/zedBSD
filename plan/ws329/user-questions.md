<!-- awesome-plan project=zedbsd record=ws329-user-questions -->

# WS329 ユーザーの判断を待つ点（p001、2026-10-31 P5）

- WS329（libmedia の H.264 の software の encoder、標準廉価実装手順）: 状態は planning。p001（設計）の成果は [design.md](design.md)・[proposed/](proposed/)。実装は WS326（H.264 の software の decoder）の completed の後。
- 決まっていない事: 下の 4 つ。どれも推しの案で設計を書いてあり、答えが違えば該当の Phase の受け入れか header を直す。

用語:
- **preset**: encoder が 1 つの macroblock（16x16 画素の単位）をどれだけ丁寧に調べるかの組。速いほど同じ画質に bit が多く要る。
- **B-frame**: 前と後の両方の画を参照して作る画。圧縮が上がるが、並べ替えのために 1 frame の遅れと、encoder の仕事が増える。
- **BD-rate**: 同じ画質（PSNR）を出すのに要る bit の量の差（%）。+40% は「比べる相手より 40% 多く bit が要る」。
- **PSNR**: 元の画と復号した画の差の小ささを dB で表した数。大きいほど元に近い。
- **x264**: 広く使われる H.264 の encoder（GPL）。code は使わず、host の ffmpeg で画質の物差しにだけ使う。

## Q329-1 速さと画質の目標

背景: software の encoder は SIMD（CPU の並列の命令）無しの C で、1080p を実時間で encode するのは難しい（design §9 の見積もり: 720p で FAST 約 20 fps、BALANCED 約 5 fps、1 thread、5330）。目標は Phase の受け入れ（p023 の画質、p025 の速さ）に入る。

- **A（推し）**: 正しさの門（全組で ffmpeg と自前の decoder の両方が誤り無く decode し、encoder の再構成と全 frame で一致）＋画質は BALANCED が x264 veryfast の BD-rate +40% 以内、FAST が +80% 以内＋速さは 5330 で FAST が 720p30 を slice の 2〜4 thread で実時間、640x480 30 fps を 1 thread、Pi 5 は FAST で 480p30 を目安。1080p30 の実時間は目標にしない（SIMD の WS を別に立てる時に）。
- B: 速さを上げる（1080p30 の実時間を目標に、SIMD（SSE2・NEON）を p025 の範囲に入れる）。Phase が数個増える。
- C: 画質だけを門にし、速さは測るだけ。

## Q329-2 B-frame を入れるか

背景: WS318 の hardware の encoder（Vulkan Video）は最初 B-frame 無し（Q318-9 の推し）。software の encoder では B-frame で同じ画質に 1〜2 割少ない bit で済むが、決定の module が 1 つ（p019）と試験の組が増える。既定（config の b_frames が 0）では使わない。

- **A（推し）**: 入れる。Main と High で、参照にしない B を最大 3 枚続き（config の b_frames で指定、既定 0）。
- B: 入れない（I と P だけ）。p019 を外し、max_b_frames 0 と答える。後で別の WS で足す。

## Q329-3 WS318 と並べて進めてよいか

背景: 予定は WS326 → WS318（すぐ）→ WS329 の順。WS329 のうち libmedia の口に載せる p024 だけが WS318（p012a、encoder の層）に依り、他（共通化の p002 から全体の試験の p023 まで）は WS318 の file に触れない。WS326 の completed の後なら始められる。

- **A（推し）**: WS326 の completed の後、WS318 と並べて WS329 の p002〜p023 を進め、p024 は WS318 p012a の後、p025 は WS318 p008（thread の台帳）の後。
- B: 予定どおり WS318 の completed の後に WS329 を始める。

## Q329-4 encoder の速さの指定を libmedia の公開の口に足すか

背景: WS318 の `media_encoder_config`（設計中、まだ source に無い）には速さ・画質の指定の欄が無い。software の encoder は camera の録画（実時間が要る）と file の書き出し（画質が要る）で向く preset が違う。hardware の encoder（Vulkan Video）にも速さと画質の段の指定がある（Vulkan Video の encode の quality level）。

- **A（推し）**: WS318 の `media_encoder_config` に `unsigned speed`（0 = 実装の既定、1 = 速さ優先、2 = 釣り合い、3 = 画質優先）を足す。WS318 の p002（header を source に置く）の前なので手戻りは小さい。WS318 の Vulkan Video の encoder は quality level に写す。
- B: 足さない。software の encoder が大きさと frame rate から preset を自分で選ぶ（720p30 以上は FAST、それ未満は BALANCED）。
