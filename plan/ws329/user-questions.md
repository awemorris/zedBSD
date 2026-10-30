<!-- awesome-plan project=zedbsd record=ws329-user-questions -->

# WS329 ユーザーの判断を待つ点（p001、2026-10-31 P5）

- WS329（libmedia の H.264 の software の encoder、標準廉価実装手順）: 状態は planning。p001（設計）の成果は [design.md](design.md)・[proposed/](proposed/)。実装は WS326（H.264 の software の decoder）の completed の後。
- 決まっていない事: 下の 5 つ。どれも推しの案で設計を書いてあり、答えが違えば該当の Phase の受け入れか header を直す。

用語:
- **encoder の preset**: encoder が画の 1 区画（16x16 画素の macroblock）をどれだけ丁寧に調べるかの組。速いほど同じ画質に要る bit が多い。
- **PSNR**: 元の画と復号した画の差の小ささを dB で表した数。大きいほど元に近い。
- **BD-rate**: 同じ画質（PSNR）を出すのに要る bit の量の差（%）。+40% は「比べる相手より 40% 多く bit が要る」。
- **x264**: 広く使われる H.264 の encoder（GPL）。code は使わず、host の ffmpeg で画質の物差しにだけ使う。
- **slice**: 1 枚の画を横の帯に分けた単位。帯ごとに別の CPU の core で並べて符号化できる。
- **thread**: CPU の上で並べて走る仕事の流れ。zedBSD では名前と本数を決めて作る方針（WS309）。
- **SIMD**: 1 つの命令で複数の画素をまとめて計算する CPU の命令（x86 の SSE2、ARM の NEON）。
- **B-frame**: 前と後の両方の画を参照して作る画。圧縮が上がるが、並べ替えのために 1 frame の遅れと encoder の仕事が増える。

## Q329-1 画質の目標

背景: 出力が正しいこと（どの組み合わせでも、host の ffmpeg と自前の decoder の両方が誤り無く復号し、encoder が予測に使った画と全 frame で一致すること）は必須の門として設計した。その上の画質の目標が p023 の受け入れになる。

- **A（推し）**: 釣り合いの preset が x264 の veryfast の BD-rate +40% 以内、速い preset が +80% 以内（host の試料 4 本）。
- B: 目標を厳しくする（釣り合いが +20% 以内）。mode の選びに RD 最適化（実際に符号化して bit と歪みで選ぶ）を足す Phase が増え、encode が遅くなる。
- C: 画質は測るだけで門にしない。

## Q329-2 速さの目標

背景: SIMD を使わない C の encoder の見積もり（design §9）は、720p（1280x720）で速い preset が 1 thread で約 20 fps、釣り合いが約 5 fps（5330）。1080p の実時間は SIMD 無しでは難しい。目標は p025 の受け入れになる。

- **A（推し）**: 5330 で速い preset が 720p30 を slice の 2〜4 thread で実時間、640x480 30 fps を 1 thread で。Pi 5 は速い preset で 480p30 を目安。1080p30 の実時間は目標にしない（SIMD は必要になった時に別の WS で）。
- B: 1080p30 の実時間を目標にし、SIMD（SSE2・NEON）を p025 の範囲に入れる。Phase が数個増える。
- C: 速さは測るだけで門にしない。

## Q329-3 B-frame を入れるか

背景: hardware の encoder（Vulkan Video、WS318）は最初 B-frame 無し（Q318-9 の推し）。software の encoder では B-frame で同じ画質に 1〜2 割少ない bit で済むが、決定の module が 1 つ（p019）と試験の組が増える。設定で b_frames を 0 にすれば使わない（既定は 0）。

- **A（推し）**: 入れる。Main と High の profile で、参照にしない B を最大 3 枚続き。
- B: 入れない（I と P だけ）。p019 を外す。後で別の WS で足す。

## Q329-4 WS318 と並べて進めてよいか

背景: 予定は WS326（H.264 decoder）→ WS318（libmedia の作り直し、WS326 の後すぐ）→ WS329 の順。WS329 のうち、libmedia の encoder の口に載せる p024 は WS318 の encoder の層ができてから（WS318 の p012a）、slice の並列の p025 は WS318 の名前付き thread の仕組みができてから（WS318 の p008）。それ以外（decoder との共通化の p002 から全体の試験の p023 まで）は WS318 の file に触れず、WS326 の completed の後なら始められる。

- **A（推し）**: WS326 の completed の後、WS318 と並べて WS329 の p002〜p023 を進め、p024・p025 だけ WS318 の該当の Phase を待つ。
- B: 予定どおり WS318 の completed の後に WS329 を始める。

## Q329-5 encoder の速さの指定を libmedia の公開の口に足すか

背景: WS318 で設計中の encoder の設定（`media_encoder_config`、まだ source に無い）には速さ・画質の指定の欄が無い。software の encoder は camera の録画（実時間が要る）と file の書き出し（画質が要る）で向く preset が違う。Vulkan Video の hardware の encoder にも速さと画質の段の指定がある。参照の枚数・entropy の種類・slice の数の指定も無いが、これらは preset と profile から encoder が決める。

- **A（推し）**: WS318 の `media_encoder_config` に速さの欄（0 = 実装の既定、1 = 速さ優先、2 = 釣り合い、3 = 画質優先）を 1 つ足す。WS318 の公開の header がまだ source に置かれる前なので手戻りは小さい。Vulkan Video の encoder はその段に写す。
- B: 足さない。software の encoder が大きさと frame rate から preset を自分で選ぶ（720p30 以上は速い preset、それ未満は釣り合い）。
