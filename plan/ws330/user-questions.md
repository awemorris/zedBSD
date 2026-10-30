<!-- awesome-plan project=zedbsd record=ws330-user-questions -->

# WS330 ユーザーの判断を待つ点（p001、2026-10-31 P5）

- WS330（libmedia の H.265 の software の decoder と encoder、標準廉価実装手順）: 状態は planning。p001（設計）の成果は [design.md](design.md)・[proposed/](proposed/)。実装は WS329（H.264 の software の encoder）の completed の後（ユーザーの決定）。
- 決まっていない事: 下の 4 つ。どれも推しの案で設計を書いてあり、答えが違えば該当の Phase の範囲か受け入れを直す。

用語:
- **H.265（HEVC）**: H.264 の次の世代の動画の圧縮の規格。同じ画質で bit が約半分。
- **profile**: 規格の中で使ってよい道具の組。**Main** は 8 bit の色の深さ、**Main 10** は 10 bit（HDR の動画などが使う）。
- **software の decoder・encoder**: GPU の専用の回路（Vulkan Video）を使わず、CPU の計算で動画を復元・圧縮する部品。
- **WPP・tile**: 1 枚の画を行や矩形に分けて、並べて処理できるようにする H.265 の道具。多くの encoder（x265 など）が既定で使う。
- **parameter set・slice header**: 動画の流れの先頭や各画の先頭にある、大きさや参照の仕方を書いた部分。これを読む部分を「解析」と呼ぶ。
- **PSNR・BD-rate**: 画質の数と、同じ画質に要る bit の差（%）。+60% は「比べる相手より 60% 多く bit が要る」。
- **x265**: 広く使われる H.265 の encoder（GPL）。code は使わず、host の ffmpeg で画質の物差しにだけ使う。

## Q330-1 範囲: Main（8 bit）だけか、Main 10（10 bit）も入れるか

背景: 10 bit の動画（HDR や一部の配信）は、画素を 16 bit で持つ道と、libmedia の出力の形（P010）と表示の変換が要る。libmedia の作り直し（WS318）は Main 10 の出力を範囲の外にした（Q318-8 の推し）。10 bit の stream は今の設計でも libavcodec（libmedia の最後の backend）に回せば再生できる。

- **A（推し）**: decoder・encoder とも Main（8 bit 4:2:0）だけ。10 bit の stream は libavcodec に回す。Main 10 は WS318 が 10 bit の出力を持った後に別の WS で足す（式は bit depth を定数で残してあり、後で 16 bit に広げやすい形）。
- B: decoder だけ Main 10 も入れる。全 module が 16 bit の画素の道を持ち、試験が約 2 倍、WS318 に P010 の出力の追加が要る。
- C: decoder・encoder とも Main 10 も入れる。

## Q330-2 H.265 の解析を WS318 と共有するか

背景: WS318（libmedia の作り直し）は、GPU の Vulkan Video で H.265 を decode する道のために、H.265 の parameter set・slice header・参照の管理の解析を作る予定（WS318 p016〜p019）。この WS の software の decoder も同じ解析が要る。H.264 の時は既に GPU 用の解析があったので、壊さないために software 用を別に作った。H.265 はまだどちらも無い。

- **A（推し）**: 解析を 1 つにする。この設計の解析の header（GPU の型に依らない形）を WS318 p016 がそのまま採り、WS318 p017〜p019 で実装し、GPU 用の型（StdVideoH265）へ写す小さな部分だけを WS318 が持つ。WS330 はその実装を確かめて使う。同じ仕様の節を 2 回実装・照合しなくて済む。
- B: H.264 と同じく別々に作る（WS318 の今の設計のまま）。WS318 と WS330 が互いに待たない代わりに、似た解析が 2 つできる。

## Q330-3 速さと画質の目標

背景: 正しさ（どの組み合わせでも host の ffmpeg と自前の decoder が誤り無く復元し、全 frame が一致すること、と規格の適合の試験）は必須の門として設計した。その上の速さと画質の目標が、速さの Phase（decoder p031、encoder p048）と品質の Phase（p046）の受け入れになる。見積もり（design §9.3）: decoder は SIMD 無し 1 thread で 720p30 は実時間、1080p は 15〜25 fps（5330）。encoder の速い設定は 720p で 1 thread 8〜12 fps。

- **A（推し）**: decoder は 5330 で 1080p30 を WPP・tile の並列か行の並列で実時間（並列にできない stream は 720p30 まで）、Pi 5 は 720p30。encoder は 5330 で速い設定が 720p30 を 4 thread（WPP）で実時間。画質は釣り合いの設定が自前の H.264 encoder の同じ設定より BD-rate で 15% 以上良く、x265 の veryfast の +60% 以内。SIMD は目標にしない（要れば別の WS）。
- B: 目標を高くし（decoder の 1080p60、encoder の 1080p30）、SIMD（SSE2・NEON）を速さの Phase の範囲に入れる。Phase が数個増える。
- C: 速さと画質は測るだけで門にしない。

## Q330-4 decoder の部分を早く始めてよいか

背景: ユーザーの決定は「H.264 の encoder（WS329）が完了してから」。WS330 の decoder の部分（p002〜p031）は WS329 の file に触れず、WS326（H.264 の decoder）の共通化（p026）の後なら始められる。encoder の部分（p032 以降）は WS329 の encoder を共通の部品にするので WS329 の完了が要る。libmedia の WS の優先度を上げる指示（2026-10-31）を受けて聞く。

- **A（推し）**: 決定どおり WS329 の completed の後に始める（共通化の Phase を 1 回で済ませ、H.264 の decoder・encoder の試験を 1 度に確かめる）。
- B: decoder の部分は WS326 の completed（と WS318 の進み）の後に WS329 と並べて始め、encoder の部分だけ WS329 の completed を待つ。共通化の Phase が decoder 側（p002）と encoder 側（p032）に分かれたまま進むので手戻りは無いが、同時に動く担当が増える。
