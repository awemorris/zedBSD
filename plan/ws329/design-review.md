<!-- awesome-plan project=zedbsd record=ws329-design-review -->

# WS329 p001 の設計の敵対的レビュー（2026-10-31）

- 対象: [design.md](design.md)・[proposed/](proposed/)・phase002〜026 の phase.md・[user-questions.md](user-questions.md)・[ws.md](ws.md)（agent/p5-ws329 e0469576f）。
- レビュー: design-reviewer を 1 本（decoder の code `userland/desktop/libmedia/h264sw-*.c` と仕様の PDF の文字起こしを照合）。29 件（blocker 0・major 10・minor 19）。
- 反映: P5（同日）。下の表の「反映」が設計の変更。見送りは理由を書いた。

## 指摘と反映

| # | 重さ | 指摘（要点） | 反映 |
| --- | --- | --- | --- |
| 1 | major | h264enc-mb.h が entry の kind・qp・transform_8x8 を手順 7 で書き、それを読む手順 2（intra の予測 mode、`intra_neighbour_mode` は kind が I_4X4・I_8X8 でなければ DC）と手順 3（recon は `mb->qp` を読む）より後。決定と違う mode の予測に levels を足すことになり、画質が黙って落ちる | 手順 1 の直後に kind・transform_8x8・qp・motion.intra を置くと header に書いた。p017 の試験に「decode した mode・MV が決定と一致」 |
| 2 | major | 「CABAC でも decoder が total_coeff を書く」は誤り（`h264sw-macroblock.c` の residual は CAVLC で total_coeff だけ、CABAC で coded_flags だけ、I_PCM は両方） | h264enc-mb.h の手順 7 を entropy ごとの規則に直し、p016・p017 の試験もそれに |
| 3 | major | skip の決定に残差がある時の扱いが無い（P_SKIP のまま残差を作ると drift） | skip の kind は残差を作らない（E19）。mode・mode-b・mb の header に明記 |
| 4 | major | Constrained Baseline と Main では I_PCM の標本 0 が禁止（A.2.1・A.2.2） | mbctx に pcm_nonzero、`h264enc_mb_code_pcm` で 0 を 1 に（再構成も送った値）。p017 の試験 |
| 5 | major | CQP と高い bitrate で access unit の大きさの上限（A.3.1 c)・d)、MinCR）を守る仕組みが無い | `h264enc_level_max_au_bytes` を ps.h に、encoder の手順 3a で超えたら QP + 6 で符号化し直す（3 回まで、超過は stats）。E15 |
| 6 | major | encoder の再構成を取り出す口が無く「3 者の MD5」の試験が組めない（header は p003 で凍結） | `h264enc_set_recon_hook` を h264enc-encoder.h に（E20）。p022 の道具が使う |
| 7 | major | 自前の decoder の往復では、意図と違う syntax と decoder が調べない制約（PCM の 0、MB の bit、bin の数、MV の範囲、level_prefix、AU の大きさ）が見えない | p017〜p019 の試験に「decode した mode・MV・ref が決定と一致」、`h264enc_get_stats`（E20）で p022 の道具が制約を確かめる。design §10.2 に往復の限りを明記 |
| 8 | major | 文献の出典を file の先頭の comment に書いている（guardrail は block ごとの `Procedure referenced:`）、「the usual dead zone of H.264 encoders」 | cost（λ の struct）・me（search の関数）・ratecontrol（rc_qp の関数）の直前の block に `Procedure referenced:`。dead zone は「encoder の選択」に。design §1 の書式も guardrail の語に。CABAC の見積もりの表（Marpe の出典）は #17 で外した |
| 9 | major | p016〜p019 の試験の土台が無く、p017 が Sonnet の 1 回に重い | p003 に `fixture.h`（試験の画・frame・list・context・decode し戻し・比較の補助）。p017 を Opus に |
| 10 | major | CAVLC の cbp の表の添字が共有の表と逆 | h264enc-cavlc.h を共有の表の列の意味（[0] intra、[1] inter）に揃えた |
| 11 | minor | 1 MB の bit の上限の節の番号の誤り（A.3.1 n)・A.3.2 j)） | h264enc.h・mbwrite.h・design を直した |
| 12 | minor | decode_time_us の規則が時刻の揺れる入力で dts > pts になる | dts[0] = t0 − 1 frame、dts[n] = (n − 1) 番目に小さい時刻（gop.h・h264enc.h）。p021 の試験に揺れる例 |
| 13 | minor | NAL header の nal_ref_idc（SPS・PPS は 0 以外、filler は 0）と PPS の pic_init_qs・id が決まっていない | ps.h（SPS・PPS は 3、id 0、pic_init_qs_minus26 0）と slice.h（filler は 0）に |
| 14 | minor | `vcodec_bitmark` に error が無く rewind の規則が実装できない、mbwrite の mark に QPY が無い | bitmark に error、rewind は mark の error に戻す。mbwrite の mark に qp |
| 15 | minor | B_8x8 の direct の quadrant の後に decoded_4x4 の印を付けるか書いていない | mb.h の手順 2 と mode-b.h に「direct の quadrant の後に印」 |
| 16 | minor | early skip の chroma の予測の出どころ、refplanes の窓の外の skip・direct の vector | chroma は `h264sw_interp_chroma`（B は平均）と mode・mode-b に明記。refplanes は窓の外を `h264sw_interp_luma` に落とす（frame を借りて持つ） |
| 17 | minor | `h264enc_cabac_residual_cost`・`vcodec_cabac_cost` を使う所が無い | 外した（E9 は CAVLC の長さで近似）。p005・p011 の試験から見積もりを除いた |
| 18 | minor | POC が int32 で key_interval が無制限 | `H264ENC_MAX_KEY_INTERVAL`（2^30）を h264enc.h と gop の init に |
| 19 | minor | rate control の clamp の順、filler と SPS・PPS の数え方、CBR の level、機械による QP の違い | rc_qp に順（model → ±4 → buffer → 上下限）、X_t は filler を除き SPS・PPS を含む、R は filler も引く。機械をまたいで stream を比べないと明記。CBR の level は encoder.h の既存の記述（max_bitrate に bitrate）で足りる |
| 20 | minor | fixed_frame_rate_flag 1 は揺れる入力で偽、time_scale の溢れ | fixed_frame_rate_flag 0。rate_num・rate_den は 2^31 − 1 まで、300 fps まで（h264enc.h） |
| 21 | minor | mbctx の high が profile と PPS の flag を混ぜている | `transform_8x8_mode`（PPS の flag）に改め、intra・mode の参照も |
| 22 | minor | level の選びで fR（172 fps・300 fps）を見ていない | `h264enc_level_choose` の規則に足した。p006 の試験に |
| 23 | minor | mbwrite の ERANGE は clamp の後には通らない | header に「わざと壊した時だけ通る guard」と書いた |
| 24 | minor | design-review.md が無いのに記録が「反映は済み」、ws.md に原文の引用 | この file を作った。原文の扱いは Q1 の担当として design §14 の 9 に記録（ws.md の原文の節は変えていない） |
| 25 | minor | user-questions.md の専門語、Q329-1 が 3 つの決定をまとめている | 用語の説明を足し、画質（Q329-1）と速さ（Q329-2）を分け、5 問に |
| 26 | minor | WS326 p026 が frame・DPB まで移すと名前の置き換え以上の差分 | design §14 の 8（p003 で review） |
| 27 | minor | 大きな picture での参照の面の memory | design §14 の 7（p024 で問い合わせの最大の大きさに反映するか決める） |
| 28 | minor | WS318 の口に refs・entropy・slices の欄が無い | design §14 の 10、Q329-5 の背景に |
| 29 | minor | NAL・AU の buffer の limit を誰が決めるか無い | h264enc-encoder.h に limit の規則（slice 400 byte/MB + 4096、AU 600 byte/MB + 65536 + CBR の buffer） |

## レビューが確かめて正しいとした所

cabac_zero_word の式と NumBytesInVclNALunits、CABAC の encoder の engine（flush の stop bit、PCM の位置が decoder の `h264sw_cabac_pcm` と合う）、前向きの変換と量子化の MF の式と DC の道（代数で）、CAVLC の levelCode・suffixLength・±2063・8x8 の interleave、CABAC の 2 値化と ctxIdx の offset、slice header の順と override、frame_num・POC type 0・idr_pic_id、max_num_reorder_frames 1 と DPB の出力の順、B の list、MaxMvsPer2Mb・MinLumaBiPredSize・direct_8x8_inference、表 A-1 の使い方、I_16x16 の mb_type、skip と PCM の QP の持ち越し、2 回の再構成（#1 を直した後）、include の循環無し、λ の確かめの値、refplanes の 1/4 画素の平均。

## 確かめていない所

量子化の誤差の数値の再試験（レビューは代数だけ、p001 の python では確かめた）、phase.md の一部（p004〜p015・p018〜p021・p023〜p026）、速さの見積もりと画質の目標が届くか、表 9-36〜9-38 と表 A-1 の値（実装の Phase で照合）、WS326 p026 の結果。
