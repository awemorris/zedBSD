<!-- awesome-plan project=zedbsd record=ws330-design -->

# WS330 libmedia の H.265 の software の decoder と encoder の設計（p001）

- 親: [ws.md](ws.md)。作成: ws330-p001（P5、q1763-i01、2026-10-31）。調べた tree: main 901f5b5fc（worktree `p5-ws330`）。
- 正本: 各 module の仕様は [proposed/](proposed/) の header の comment（decoder の 29 個・encoder の公開の 1 個・共通の 6 個。p003 で source に置き、その commit を実装の Phase の基点 BASE にする）。この文書は全体の地図・決定・H.264 との共通化・試験の道・Phase の割り振り。
- ユーザーの判断を待つ点は [user-questions.md](user-questions.md)（Q330-1〜4）。敵対的レビューと反映は [design-review.md](design-review.md)。
- HAL（`include/hal/hal.h`）は関係しない（§12）。toolchain も変えない。source は変えていない（この Queue は plan/ws330/ だけ）。

## 1. 出典と引用

- 仕様: **ITU-T H.265 (V11) (01/2026)**（ITU の site で 2026-10-31 に in force の版。本文は ISO/IEC 23008-2 と共通）。
  - 取得: `https://www.itu.int/rec/T-REC-H.265-202601-I/en` の PDF の link をパラメータの無い GET 1 回（bot の確かめ無し）。
  - 今の置き場所: `/home/awe/zedBSD-worktrees/p5-ws330/build/tmp/ws330-spec/itu-t-h265-202601.pdf`（12 MB、734 頁、git に入れない）。**SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`**。実装の Phase が読む消えない場所として、Q1 が main の `build/sources/specs/itu-t-h265-202601.pdf` へ写す（§14-1）。phase.md はその path を書く。
  - **印刷の頁 N = PDF の頁 N + 18**（例: 8.6.4.2 は印刷 179 頁 = PDF 197 頁、7.4.7.1 は印刷 95 頁 = PDF 113 頁。2 か所で確かめた）。
  - 読み方: `pdftotext -layout -f N -l M`。式の上付き（2^15 が「215」になる）と表の折り返し（Table 9-4、Table 8-2 の列）は崩れるので、値は PDF の頁の画（Read の tool の `pages`）で確かめる。WS326 の教訓（pdftotext が表の値を落とす）をそのまま適用する。
- header と source の節・表・式の番号はこの版の物（`h265sw.h` の定数の block に書いた。file の先頭には書かない）。旧い版（V1〜V10）と番号が違う所がある（例: CABAC の文脈の表は V11 で Table 9-5〜9-42、encoder の engine は 9.3.5）。
- 他の実装（HM・FFmpeg・x265・libde265・openHEVC・Mesa など）の code・comment・構造・名前を写さない（[docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md) §7、[guardrail](../guardrail.md) の出典）。試験で host の ffmpeg（libavcodec の HEVC decoder、libx265）を**副の oracle と比べる物差し**として使うだけ（§10）。
- 表の値（initValue、transMatrix、chroma の filter の係数、intraPredAngle・invAngle、β′・tC′、既定の scaling list、走査の配列、Table E.1）は data として、block ごとに直前の複数行の comment で「Values referenced: ITU-T H.265 (01/2026) Table …」（header にその形で書いた）。source の comment に WS・Phase の番号・plan/ への参照・日付を書かない。
- 適合の data: ITU-T H.265.1 の HEVC_v1 の bitstream（JCT-VC の配布、ITU の公開の mirror）。zip の中は bitstream（`.bit`・`.bin`）、復号の出力全体の MD5（`.md5`・`_yuv.md5`）、説明（`.txt`）、一部は yuv と trace。2026-10-31 に一覧を取得（147 本、zip で 129 MB）、3 本を取得して確かめた（§10.3）。

## 2. 範囲

| 項目 | decoder | encoder | 範囲の外（理由・扱い） |
| --- | --- | --- | --- |
| 形式 | progressive、8 bit、4:2:0、base layer（nuh_layer_id 0） | 同じ | 10 bit 以上（Main 10、Q330-1）、4:0:0・4:2:2・4:4:4、field_seq_flag 1（field の列）: decoder は ENOTSUP で次の backend へ。他の layer の NAL は読み飛ばす |
| profile | Main の tool の全部（A.3.2）。profile の名前が Main 10・Main Still Picture・RExt でも SPS の中身が上の範囲なら受ける（H.264 の WS326 の決定 3 と同じ考え） | Main（general_profile_idc 1）を書く | RExt の tool（transform skip の 4x4 より大・rdpcm・cross component・extended precision・persistent rice・bypass alignment・chroma QP の list）、SCC、3D・multilayer の拡張: ENOTSUP |
| 大きさ | level 6.2 の上限（MaxLumaPs 35 651 584、各辺 16 888）まで。それより上の level でもこの大きさの中なら受ける | 同じ。偶数の幅と高さ、8 の倍数に詰めて conformance window | 上限を超える: ENOTSUP |
| 構造 | CTB 16〜64、AMP、PCM、transform skip（4x4）、scaling list、sign data hiding、weighted prediction、long-term、TMVP、list の modification、slice・dependent slice segment、**tile と WPP（1 thread で順に読む）**、SAO、deblocking の制御、cu_qp_delta、transquant bypass | CTB 32（FAST）か 64、CU 8〜、2Nx2N・2NxN・Nx2N（AMP 無し）、intra 2Nx2N・NxN（8x8）、TU 4〜32、merge・skip・AMVP、TMVP、SAO、deblocking、I・P・参照にしない B（Q329-3 と同じ扱い）、WPP は任意（並列の encode 用） | encoder は AMP・transform skip・scaling list・PCM・weighted・long-term・tile・dependent slice を使わない（画質の効きが小さいか試験が重い。後で足せる形） |
| 出力 | 表示の順の frame（crop は libmedia の backend が行う）。decoded picture hash の SEI を照合して数える | Annex B の access unit（IRAP の前に VPS・SPS・PPS） | — |

範囲の外の stream は decoder が ENOTSUP を返し、libmedia の層が次の backend（libavcodec）へ回す（WS326 p023 の遅れた拒否の切り替えをそのまま使う）。

## 3. 決定（根拠と、選ばなかった案）

| # | 決定 | 根拠 | 選ばなかった案と理由 |
| --- | --- | --- | --- |
| D1 | **decoder は Main の tool を全部持つ（tile と WPP を含む）** | Main の適合の decoder は tile・WPP・dependent slice を decode できる必要がある（A.3.2）。x265 は既定で WPP を使い、実際の HEVC の file の多くが WPP か tile を持つ。1 thread で順に読むなら、tile は CTB の走査の順と文脈の初期化、WPP は文脈の保存と同期と substream の切り替えだけで、費用は小さい | tile・WPP を範囲の外にして次の backend へ（実際の stream の多くを libavcodec に回すことになり、WS の目的に反する） |
| D2 | 範囲は Main（8 bit）。Main 10 は後の WS（Q330-1） | WS318 Q318-8 の推しと同じ（P010 の出力と表示の道が libmedia に無い）。8 bit の型で全 module を書ける。header は BitDepth を定数 `H265SW_BIT_DEPTH` で式に残し、後で 16 bit の sample に広げる時に式を読み直さずに済む形にした | 最初から 16 bit の sample（8 bit の stream で memory の帯域が倍、全 module の試験が倍） |
| D3 | 名前: H.265 の decoder は `h265sw-`、encoder は `h265enc-`、codec に依らない物は `vcodec-` | WS326 の決定 3（共通部分は汎用の名前）、WS329 の E4（`vcodec-` を書き側に使った）に揃える | 全部 `hevc-`（既存の名前の組と揃わない） |
| D4 | **H.265 の parameter set・slice header・POC・RPS・DPB の marking の解析は Vulkan の header に依らない構造体で 1 つだけ作り、WS318 の Vulkan Video の H.265 の道もそれを使う**（StdVideoH265 へ写す module だけ WS318 が持つ）（Q330-2） | H.265 の SPS・PPS・slice header・RPS は H.264 よりずっと大きく（st_ref_pic_set の予測、tile、long-term、VUI と HRD）、2 つ書くと誤りも 2 倍。H.264 で分けた理由（既に h264.c があり、Vulkan の道を壊す危険）は H.265 には無い（まだどちらも無い） | WS318 の D19 のまま 2 つの解析（重複。同じ仕様の節を 2 回 Sonnet に実装させ、2 回照合する） |
| D5 | CTU を先に全部 parse し（`h265sw-ctu`）、次に再構成する（`h265sw-recon`） | H.265 の syntax の解析は画素に依らない（intra の mode の導出は要るが画素は要らない）ので分けても正確。module の境がはっきりし、parse と再構成を別の Phase で試験できる。CTU 1 つの struct は約 30 KB | CU ごとに parse と再構成を交互（module が大きくなり、Sonnet の 1 回に収まらない） |
| D6 | picture の状態は 4x4 の block ごとの entry（`struct h265sw_blk`・`struct h265sw_mv_field`）と CTB ごと（slice・tile・SAO）と slice segment ごと（filter の値と参照の id と POC） | 最小の PU（8x4・4x8）と TU（4x4）が 4x4 の格子。deblocking・SAO・AMVP・merge・文脈の選びが同じ表を読む | MinTbAddrZs の表を持つ（8K で 2 M 個の uint32）。z 順の番号は 6-10 の式で計算する（`h265sw_pic_zscan`） |
| D7 | 参照画の motion は 16x16 ごとに 1 つ（左上の 4x4 の値）を、参照の POC と long-term の印と一緒に frame の side data に持つ | 8.5.3.2.8 が `( ( x >> 4 ) << 4, ( y >> 4 ) << 4 )` しか読まない。参照の POC と印を一緒に持てば、collocated の picture の参照の list を残さずに済む（LongTermRefPic と DiffPicOrderCnt は「その picture を decode した時の」値） | 4x4 の motion を全部残す（16 倍の memory）、参照の list を frame に持つ |
| D8 | frame は共通の `struct vcodec_frame`（plane・参照の数・id・POC・時刻・表示の値）を最初の member に埋め込み、codec の data は side data | H.264 の frame と pool（420 行）は codec に依らない部分が大きい。埋め込みなら H.264 の field（frame_num など）を残したまま pool を共通にできる | H.265 だけ別の pool（重複）、汎用の `void *` の field の束（型が弱い） |
| D9 | deblocking と SAO は picture の最後に picture 全体で。SAO は deblocking の後の plane の写しから読む | 8.7.2.1・8.7.3 の書き方そのもので、試験と照合が簡単。CTU の行ごとに遅らせる方法は速さの Phase で（同じ結果を MD5 で確かめる） | 最初から CTU の行ごと（境界の扱いが複雑で、最初の正しさの確かめが難しい） |
| D10 | decoded picture hash の SEI（D.3.20、MD5・CRC・checksum）を decoder が照合して数える | 適合の bitstream の多くと HM・x265 の出力が持つ。他の decoder に頼らない oracle で、frame ごとに誤りの位置が分かる。MD5 は RFC 1321 から書く | 照合しない（ffmpeg と zip の MD5 だけ: どの frame で誤ったか分からない） |
| D11 | end of sequence の NAL で DPB を出力付きで空にする | 再生の道では EOS の後の frame を捨てたくない。C.5.2.2 の CRA の NoOutputOfPriorPicsFlag 1 は空の DPB に当たるので結果は同じ。HM の参照の出力（適合の MD5）も EOS で全部を出す | 規定の通り CRA で捨てる（適合の MD5 と frame 数が合わなくなる流れがある） |
| D12 | 開始と seek の後は IRAP を待つ。CRA から始めたら NoRaslOutputFlag 1（HandleCraAsBla）で、その RASL は decode せず出力しない | 8.1.3 の外部の手段の使い方そのもの。RASL は前の picture を参照するので開始時は正しく作れない | RASL を stand-in で decode して出す（壊れた画を見せる） |
| D13 | 参照の欠け（壊れた stream、途中から）は 8.3.3 と同じ形の灰の picture で埋め、数える | どの欠けでも decoder が止まらない。8.3.3 が BLA・CRA に対して定める形をそのまま他にも使う | slice を捨てる（画が大きく崩れる） |
| D14 | substream（tile・WPP の行）は slice data の中を順に読む。entry_point_offset は読み飛ばす | 1 thread なら次の substream は end_of_subset_one_bit の後の byte 境界から始まる。entry point は emulation prevention を含む NAL の byte 数で、RBSP の位置にすると変換が要る | entry point で位置を決める（変換が要り、並列にしない間は利点が無い） |
| D15 | 最初は 1 thread。WPP・tile の並列の decode と SIMD は速さの Phase（p031）で | 正しさが先（WS326 と同じ順）。構造（CTU の parse と再構成、文脈の保存）は並列にできる形 | 最初から並列 |
| D16 | VPS は読まない | base layer の decode は VPS の値を使わない（7.4.3.1） | 全部読む（使わない code と試験） |
| D17 | SPS の VUI が読めない時、profile が Main・Main 10・Main Still Picture なら VUI だけ捨てて SPS を残し、拡張の flag を 0 とみなす。他の profile では EINVAL | 古い encoder の VUI の誤り（default display window の位置）で再生を止めない。Main の系は拡張の flag が 0 と A.3.2〜A.3.4 が定めるので、捨てても誤った decode にならない | 全部 EINVAL（実際の file が再生できない）、全部許す（RExt の stream を誤って decode する） |
| D18 | 算術の未定義の動作の禁止を `vcodec.h` の補助（asr・shl・clip3）で統一 | H.265 は負の値の shift が多い（MV の scaling、intra の index、filter、文脈の初期化の `( m * qp ) >> 4`）。H.264 の decoder の規則を共通の補助にして両方が使う | module ごとに書く（同じ誤りを何度も） |
| D19 | **encoder の module の header は decoder の header が実装で確かめられた後（p033、Opus）に書く**。この Phase は encoder の公開の header（`h265enc.h`）・module の地図・決定・Phase の案まで | encoder は decoder の状態と導出をそのまま使う（WS329 E1）。decoder の header は実装の Phase で直ることがある（WS326 p021 で reflist の header を直した）。先に encoder の header を固めると直しが連鎖する | 今 encoder の header も全部書く（decoder の header の直しのたびに encoder の header も直す） |
| D20 | encoder の並列は WPP（CTU の行）で、slice では分けない | HEVC の並列の道具。slice は header と予測の切れで bit が増える。WPP は約 1〜2% の bit で行を並べられ、decoder も D1 で持つ | slice の並列（WS329 と同じ形だが H.265 では効率が悪い） |
| D21 | libmedia の backend は、WS318 p006a の後の `h264sw-backend.c` から codec に依らない部分（packet の変換、NAL の分割、出力の frame の複写、時刻、切り替えの規則への応え）を共通の核に出し、H.265 はその上に載せる（p030、Opus） | WS318 が backend の ops と出力の形（staging の CPU の frame）を変えるので、その後でないと形が決まらない。核を出せば H.264 と H.265 の backend が同じ規則で動く | 今 header を書く（WS318 の変更で書き直し） |
| D22 | 適合の試験は HEVC_v1 の 147 本のうち名前から範囲に入る 136 本を候補にし、decoder 自身の SPS の判定で最終の分類をする。zip は展開せず `unzip -p` で読む | disk が少ない（約 7.7 GB）。zip だけなら 129 MB。展開すると yuv で数 GB | 全部展開（disk が足りない） |

## 4. H.264 の decoder・encoder の module の洗い出し（H.265 で使える物）

WS326（main 901f5b5fc の `userland/desktop/libmedia/h264sw*.h`、全部読んだ）と WS329（[../ws329/proposed/](../ws329/proposed/)、今日 main に入った設計）の各 module と、H.265 での使い方。「共通化」は p002（decoder 側）と p032（encoder 側）で行い、H.264 の単体試験と JVT の適合の試験（encoder は roundtrip の MD5）が前後で同じであることで確かめる。

### 4.1 decoder（WS326）

| H.264 の module | H.265 での使い方 | 共通化 |
| --- | --- | --- |
| h264sw.h（上限・誤りの約束・算術の規則） | 誤りの値と算術の規則は同じ。上限は codec ごと | 誤りの約束・算術の規則・補助を `vcodec.h` へ（[proposed/common/vcodec.h](proposed/common/vcodec.h)）。h264sw.h は上限だけ残し vcodec.h を include |
| h264sw-bits（u・ue・se・te・more_rbsp_data・trailing） | そのまま（H.265 7.2・9.2 は同じ記述子）。H.265 の `byte_alignment()` を足す | `vcodec-bits`（[proposed/common/vcodec-bits.h](proposed/common/vcodec-bits.h)）。WS326 p026 の範囲 |
| h264sw-nal（Annex B の分割・NAL header・RBSP） | 分割と emulation prevention の除去は同じ（H.265 B.2・7.4.2）。header は 2 byte で別 | 分割と除去を `vcodec-nal`（[proposed/common/vcodec-nal.h](proposed/common/vcodec-nal.h)）へ。header の読みは codec ごと（`h265sw-nal`）。WS326 p026 の範囲 |
| h264sw-cabac（engine・文脈の初期化・表 9-44・9-45） | engine と表は同じ（H.265 Table 9-52・9-53 は H.264 の 9-44・9-45 と同じ値）。文脈の初期化の式も同じ（9-6 と 9-5）。文脈の数と (m, n) の出し方（initValue）は別 | engine を文脈の byte の pointer を受ける形で `vcodec-cabac`（[proposed/common/vcodec-cabac.h](proposed/common/vcodec-cabac.h)）へ、表を共通の名前で。H.265 用に bypass の複数 bin と terminate の後の位置（PCM と substream）を足す。H.264 の `struct h264sw_cabac` は engine と 460 の文脈を並べる形になる。WS326 p026 の範囲（足りなければ p002） |
| h264sw-cabac-init（(m, n) の表） | 使わない（H.265 は initValue の表、`h265sw-cabac-init`） | 無し |
| h264sw-frame（frame と pool） | 共通の部分を使う | `vcodec-frame`（[proposed/common/vcodec-frame.h](proposed/common/vcodec-frame.h)）。H.264 の frame は `struct vcodec_frame` を最初の member に埋め込み、motion を side data に（D8）。p002 |
| h264sw-dpb の出力の queue と時刻の入れ替え | 同じ | `vcodec-output`（[proposed/common/vcodec-output.h](proposed/common/vcodec-output.h)）。marking・bumping の規則は codec ごと（H.265 は C.5.2 と RPS）。p002 |
| h264sw-poc・dpb の marking・reflist | 使わない（H.265 は RPS・C.5.2・8.3.4、規則が別） | 無し |
| h264sw-tables・ps・slice | 使わない（syntax が別） | 無し |
| h264sw-mb・context・macroblock・cavlc・cabac-se | 使わない（CTU・CU・PU・TU の四分木と別の syntax） | 無し |
| h264sw-transform・intra・interp・weight・mvpred・direct・recon・deblock | 使わない（変換は 4〜32 の DCT と DST、intra は 35 mode、補間は 8 と 4 tap、MV は merge と AMVP、deblocking は 8x8 の格子と bS 0〜2）。式が似ていても値と範囲が違い、共通にすると両方の試験が複雑になる | 無し（判断の記録: 共通にしない） |
| h264sw-decoder | 流れの形（NAL → AU の境 → picture の始まりと終わり → 出力、開始と回復、時刻）は同じ考え。code は別 | 無し（形を写すだけ） |
| h264sw-backend | libmedia の ops の大半（packet の変換、NAL の分割、出力の複写、時刻、遅れた拒否への応え）は codec に依らない | WS318 p006a の後に共通の核を出す（D21、p030） |
| bits.h（`media_rbsp_unescape`） | 使わない（`vcodec_nal_unescape` が末尾の 0x03 を扱う） | 無し |
| 試験の道具（`plan/ws326/tests/unit/check.h`・`bitwriter.h`・`cabacwriter.h`） | `check.h` を使う。書きの逆の実装は WS329 の製品の `vcodec-bitwriter`・`vcodec-cabac-enc` を使う（§10.1） | 無し |

### 4.2 encoder（WS329）

| H.264 の encoder の module | H.265 での使い方 | 共通化 |
| --- | --- | --- |
| vcodec-bitwriter・vcodec-nalwriter・vcodec-cabac-enc | そのまま（WS329 E4 の目的）。nalwriter は 2 byte の header を受ける形で設計済み。cabac-enc の comment の「H.265 9.3.4.3」は V11 では 9.3.5（encoder の engine）、表の名前は共通化の後の `vcodec_cabac_*` | 無し（WS329 p003 で表の名前を `vcodec_` に合わせる。§14-3） |
| h264enc-cost（SAD・SATD・SSD・λ・mvd の bit） | 大きさを 64 まで、SATD に 8x8 を足せば同じ。λ の式（Wiegand）は HEVC でも使える | `vcodec-cost` へ（p032）。mvd の bit の見積もりは codec ごと（H.265 は CABAC の EG1 の bin 数） |
| h264enc-ratecontrol（CQP・CBR・VBR、picture 単位） | QP の範囲（0〜51）も同じ。そのまま | `vcodec-ratecontrol` へ（p032） |
| h264enc-source（入力の copy と端の詰め） | 詰める倍数が 16 でなく 8（MinCbSizeY）。倍数を引数に | `vcodec-source` へ（p032） |
| h264enc-gop（種類・順・frame_num・POC・時刻） | 種類・順・時刻・B の待ちは同じ。frame_num と POC type 0 は H.264 だけ | 種類と順と時刻を `vcodec-gop` へ、番号付けを codec ごと（p032）。H.265 は POC の lsb と RPS（`h265enc-gop`） |
| h264enc-me（候補・diamond・1/2・1/4） | 探索の形は同じ。1/2 画素の面は 8 tap（H.265）と 6 tap（H.264）で別、1/4 画素は H.265 では平均で作れない | 探索の核を予測の関数を受ける形で `vcodec-me` へ（p032）。面と分数の予測は codec ごと |
| h264enc-refplanes・quant・intra・cavlc・cabac-se・cabac-res・mbwrite・mb・mode・mode-b・ps・slice・encoder | syntax・変換・予測が別 | 無し（H.265 の encoder の module、§9） |
| h264enc.h の config | 同じ field の並び（`h265enc.h`） | 共通の config の構造体に出せるかは p032 で判断（WS318 の encoder の層の写しを 1 つにできる） |

## 5. 共通化の設計（順と名前）

### 5.1 順

1. WS326 p026（codec に依らない名前への移し、Opus）: [proposed/common/](proposed/common/) の `vcodec-bits`・`vcodec-nal`・`vcodec-cabac` を p026 の設計の入力として渡す（Q1 経由、§14-2）。p026 が別の名前・形を選べば、WS330 p003 が proposed の header の参照を機械的に置き換える。
2. WS326 completed → WS318 → WS329 completed（ユーザーの決定: WS330 の実装は WS329 の completed の後）。
3. **WS330 p002**（Opus）: decoder 側の残りの共通化。`vcodec.h`（誤りの約束・算術の補助）、`vcodec-frame`、`vcodec-output`、p026 がしなかった物。H.264 の decoder を移す。
4. WS330 p003（Opus）: H.265 の decoder の header と骨格を source に、BASE。
5. decoder の Phase（p004〜p031）。
6. **WS330 p032**（Opus）: encoder 側の共通化（`vcodec-cost`・`-ratecontrol`・`-source`・`-gop`・`-me` の核）。WS329 の encoder を移す。
7. WS330 p033（Opus）: encoder の header の設計（D19）。

### 5.2 p002 の変更（H.264 の decoder の振る舞いを変えない refactor）

| 変更 | 内容 | 確かめ |
| --- | --- | --- |
| C1 vcodec.h | h264sw.h の誤りの約束・算術の規則の文を vcodec.h へ、asr・shl・clip3・clip_pixel を vcodec.c に。h264sw.h は vcodec.h を include。既存の h264sw の .c の局所の補助を置き換えるかは任意（振る舞いが同じなら） | WS326 run-unit all、check-skeleton |
| C2 vcodec-frame | h264sw-frame.c の pool・参照の数・確保・灰の塗りを vcodec-frame.c へ。`struct h264sw_frame` は `struct vcodec_frame base` を最初の member に、motion は side data に（side_init が全部 intra・ref_idx -1）。h264sw の frame の field の参照（`frame->plane` など）を `frame->base.plane` に（機械的） | test-frame、run-unit all、適合（JVT の `in` の全部が前と同じ MD5） |
| C3 vcodec-output | h264sw-dpb の出力の queue と h264sw-decoder の時刻の入れ替えを vcodec-output へ | test-dpb、run-stream、適合 |
| C4 p026 の残り | bits・nal・cabac のうち p026 がしなかった物を proposed/common の形に | 該当の単体試験、適合 |

C1〜C4 は WS326 の header を変えるので、WS326 の `check-skeleton.sh` の BASE はこの Phase の commit に移る（Q1 が WS326 の記録に書く）。WS329 の `vcodec-*` と `h264enc-*` は decoder の名前を参照するので、WS329 の試験（run-unit all と roundtrip）も前後で同じことを確かめる。

### 5.3 p032 の変更（WS329 の encoder の振る舞いを変えない refactor）

| 変更 | 内容 | 確かめ |
| --- | --- | --- |
| E1 vcodec-cost | h264enc-cost の SAD・SATD・SSD・λ を大きさ 4〜64、SATD は 4x4 と 8x8 の Hadamard に。mvd の bit は codec ごとの関数 | WS329 run-unit、roundtrip の MD5 が前後で同じ（encoder は決定的） |
| E2 vcodec-ratecontrol | h264enc-ratecontrol をそのまま改名、picture の種類を codec に依らない値に | 同上 |
| E3 vcodec-source | 詰める倍数を引数に | 同上 |
| E4 vcodec-gop | 種類・順・時刻・B の待ちを vcodec-gop へ、frame_num と POC を h264enc-gop に残す | 同上 |
| E5 vcodec-me | 探索の核（候補・大小の diamond・分数の refine）を、整数の面と分数の予測の関数（codec ごと）を受ける形に | 同上（探索の結果が同じ） |

### 5.4 名前

- codec に依らない物: `vcodec-<名>.h`、関数 `vcodec_`、定数 `VCODEC_`（WS329 §5.2 の提案と同じ）。
- H.265 の decoder: `h265sw-<名>.h`、`h265sw_`、`H265SW_`。encoder: `h265enc-<名>.h`、`h265enc_`、`H265ENC_`。
- WS318 の Vulkan Video の H.265 の解析（WS318 §5.1 の `h265*.c`）: Q330-2 の答えが推しなら、WS318 p016 は解析を `h265sw-nal`・`-ps`・`-vui`・`-rps`・`-scaling`・`-slice`・`-poc`・`-dpb` の header（この proposed）で作り、StdVideoH265 へ写す module（例 `h265-std.c`）だけを WS318 が持つ（§11）。

## 6. H.265 の decoder の module

全部 `userland/desktop/libmedia/`。各 header の comment が仕様（入力・出力・前提・誤り・節・表・式）。案は [proposed/](proposed/)。

| module（.h/.c） | 内容（節） | 使う module |
| --- | --- | --- |
| h265sw.h | 上限・範囲・番号の版・mode と slice の型の定数 | vcodec.h |
| h265sw-nal | NAL header・型と分類・AU の始まり（7.3.1、7.4.2、Table 7-1） | vcodec-nal |
| h265sw-sei | decoded picture hash（D.2.20、D.3.20）、MD5（RFC 1321） | vcodec-frame |
| h265sw-scan | tile の配置・CTB の走査の変換（6.5.1）・係数の走査の表（6.5.3〜6.5.5） | — |
| h265sw-pic | picture の状態（4x4・CTB・slice segment）・可用性（6.4.1、6.4.2）・z 順（6-10） | scan、ps |
| h265sw-scaling | scaling_list_data・既定の list・ScalingFactor（7.3.4、7.4.5、Table 7-5・7-6） | scan |
| h265sw-rps | st_ref_pic_set と予測（7.3.7、7.4.8） | vcodec-bits |
| h265sw-vui | VUI と HRD（読み飛ばし）（E.2、Table E.1） | vcodec-bits |
| h265sw-ps | profile_tier_level・SPS・PPS・store・組の確かめ（7.3.2.2、7.3.2.3、7.3.3） | rps、scaling、vui |
| h265sw-slice | slice segment header・list の modification・weights（7.3.6、7.4.7） | nal、ps、rps |
| h265sw-poc | POC（8.3.1）・RPS の POC の list（8-5） | slice |
| h265sw-dpb | RPS の marking（8.3.2）・灰の picture（8.3.3）・C.5.2 の出力と bumping | poc、vcodec-frame、vcodec-output |
| h265sw-reflist | 参照の list（8.3.4）・ColPic と NoBackwardPredFlag（8.3.5） | dpb、slice |
| h265sw-cabac-init | 文脈の配置・initValue の表（Table 9-4〜9-31）・初期化（9-4〜9-7） | vcodec-cabac |
| h265sw-cabac-se | residual 以外の syntax element の 2 値化と ctxInc（9.3.3、Table 9-43〜9-49） | cabac-init、vcodec-cabac |
| h265sw-residual | residual_coding（7.3.8.11、9.3.3.11、9.3.4.2.3〜9.3.4.2.7） | cabac-se、scan |
| h265sw-intra | MPM・chroma の mode・参照の置き換え・filter・planar・DC・angular（8.4.2〜8.4.4.2） | — |
| h265sw-transform | QP（8-282〜8-290）・scaling（8.6.3）・逆変換（8.6.4）・skip と bypass（8.6.2） | — |
| h265sw-interp | 8 tap と 4 tap の補間・重み（8.5.3.3.3、8.5.3.3.4） | — |
| h265sw-tmvp | 参照画の motion の保存・時間の候補（8.5.3.2.8、8.5.3.2.9）・MV の scaling | pic、vcodec-frame |
| h265sw-mvctx.h | PU と slice の motion の文脈（構造体だけ） | pic、tmvp |
| h265sw-merge | merge の候補と選び（8.5.3.2.2〜8.5.3.2.5） | mvctx |
| h265sw-amvp | MV の予測（8.5.3.2.6、8.5.3.2.7）・mvd の加算（8-94〜8-97） | mvctx |
| h265sw-ctu | CTU・SAO・CQT・CU・PU・TT・TU の syntax（7.3.8.2〜7.3.8.14）・intra の mode の導出 | cabac-se、residual、intra、pic、ps、slice |
| h265sw-recon | QP の予測（8.6.1）・CU の再構成（8.4、8.5、8.6） | ctu、mvctx、merge、amvp、intra、transform、interp、reflist |
| h265sw-deblock | deblocking（8.7.2、Table 8-12） | pic、transform（QpC） |
| h265sw-sao | SAO（8.7.3、Table 8-13） | pic |
| h265sw-slicedata | slice_segment_data・文脈の保存と同期・substream（7.3.8.1、9.3.1、9.3.2） | ctu、recon、cabac-init |
| h265sw-decoder | 全体の流れ（8.1.3、C.5.2） | 全部 |
| h265sw-backend.c | libmedia の decoder の ops（p030、header は無し、WS318 の後） | decoder、共通の backend の核 |

依存の順（実装の Phase の順）: nal・sei → scan・pic → scaling・rps・vui → ps → slice → poc → dpb → reflist；cabac-init → cabac-se → residual；intra・transform・interp（独立）；tmvp → merge・amvp；ctu → recon → slicedata；deblock・sao（scan・pic の後）→ decoder → 適合 → backend → 速さ。

### 6.1 1 つの picture の流れ

`h265sw-decoder.h` の comment が正。要点: NAL → AU の境で前の picture を終える → slice の最初の部分で PPS・SPS → 新しい picture なら SPS の活性化・NoRaslOutputFlag・RASL の skip・POC・RPS の marking・灰の picture・C.5.2.2・frame・picture の状態・scaling の行列 → 残りの slice header → 参照の list → slice data（CTU ごとに parse → 再構成、substream の切り替え、文脈の保存と同期）→ picture の終わりに concealment・deblocking・SAO・motion の保存・hash の照合・C.5.2.3。

## 7. 所有と寿命

- `struct h265sw_decoder` が全部を持つ: parameter set の store（埋め込み、SPS は約 6 KB × 16）、RBSP の buffer（NAL の大きさまで伸びる、上限 64 MiB）、frame の pool、DPB と出力の queue（埋め込み）、picture の状態の配列（sequence の大きさで確保、8K で約 35 MB）、SAO の写しの plane（1 frame 分）、CTU の scratch（約 30 KB）、CABAC の store。
- frame は参照の数で生きる（今の picture・DPB の entry・出力の queue・最後の出力・呼び手）。pool は decoder の close で dead になり、最後の frame と一緒に消える。
- 参照の list の frame は DPB から借りる（slice の間。marking は次の picture の始まりなので消えない）。
- `struct h265sw_blk` と `struct h265sw_mv_field` の書き手: **parse**（h265sw-ctu）が flags（DECODED・SKIP・PCM・NOFILTER）・pred_mode・depth・intra_mode を CU ごとにすぐ、**再構成**（h265sw-recon）が qp_y・coded・edge の flags・motion を。CTB の SAO は parse、CTB の decoded・slice・segment は slicedata。
- picture の終わりに motion を frame の side data へ（h265sw_tmvp_store）。

## 8. 並行性・誤り

- global の状態を持たない（表は const の data）。1 つの decoder は 1 thread から呼ぶ。2 つの decoder は別の thread で lock 無しに動く。
- bitstream は信用しない。範囲外の値は配列の添字に使う前に検査、loop は buffer か上限で必ず終わる（各 header に条件を書いた: RPS の予測で 1 つ増える entry、last significant の位置、EGk の prefix の長さ、POC の int64、MV の scaling の td 0、CTU の配列の上限）。
- 誤りの範囲: slice segment の中の誤りはその segment を止め、残りの CTB は picture の終わりに concealment。picture を始められない誤り（無い PPS、壊れた header）は segment を捨てる。decoder は次の IRAP を必ず受けられる。
- 算術: vcodec.h の規則（D18）。

## 9. encoder の設計（地図と決定。module の header は p033）

### 9.1 module（案、p033 で確定）

| module | 内容 | 使う物 |
| --- | --- | --- |
| h265enc.h | 公開の核の API（[proposed/h265enc.h](proposed/h265enc.h)、この Phase で書いた） | — |
| h265enc-ps | VPS・SPS・PPS・VUI の書き、level の選び（Table A.8、A.4.2 の MaxDpbSize）、書いた物を h265sw_ps_read_* で読み戻して使う | vcodec-bitwriter、h265sw-ps |
| h265enc-slice | NAL header（2 byte）・slice segment header（RPS は SPS の set を使う）・entry point（WPP）・cabac_zero_word | vcodec-bitwriter・nalwriter、h265sw-slice |
| h265enc-cabac | CTU の syntax element の書き（cabac-se の逆、同じ 2 値化と ctxInc） | vcodec-cabac-enc、h265sw-cabac-init |
| h265enc-residual | residual_coding の書き（last の位置、sub-block、greater1/2、Rice、sign data hiding の判定） | 同上、h265sw-scan |
| h265enc-quant | 前向きの DCT・DST（4〜32、transMatrix の転置）、量子化（丸めの offset intra 1/3・inter 1/6、HEVC の MF は levelScale と 2^14 から式で） | h265sw-transform |
| h265enc-refplanes | 1/2 画素の 3 面（8 tap、decoder の補間で）、1/4 画素は候補ごとに h265sw_interp_luma | h265sw-interp、vcodec-me |
| h265enc-intra | 35 mode の選び（粗い SATD で候補を絞り、MPM を加えて bit 込みで決める） | h265sw-intra、vcodec-cost |
| h265enc-ctuwrite | CQT・CU・PU・TT・TU の書き | cabac、residual |
| h265enc-cu | CU の符号化（決定 → syntax → decoder の h265sw_recon_ctu で再構成、TU の分け） | h265sw-recon、quant、ctuwrite |
| h265enc-mode | CU の分割・skip・merge・AMVP・intra の決定、preset の表 | me、intra、cu、vcodec-cost |
| h265enc-sao | SAO の parameter の決定（band と edge の歪みの差） | h265sw-sao |
| h265enc-gop | RPS（SPS に置く set の組）・POC・時刻 | vcodec-gop |
| h265enc-encoder | 全体の流れ | 全部 |
| h265enc-backend.c | libmedia の encoder の ops（WS318 p012a の形） | encoder |

### 9.2 決定（encoder）

- E1（WS329 E1・E2 と同じ）: encoder は decoder の状態を鏡のように持ち、予測の値を decoder の関数で導き、参照画を decoder の再構成・deblocking・SAO だけで作る。drift が構造的に起きない。
- E2: CTB は FAST 32・BALANCED と QUALITY 64。CU の分割は上から（64 → 8）で、各 depth で skip・merge・2Nx2N の inter・intra を比べ、分割の和と比べる（SATD + λ·bits、最後の depth だけ SSD）。2NxN・Nx2N は BALANCED 以上。AMP は使わない。
- E3: TU は intra は CU と同じ大きさから 1 段まで（RQT の深さ 1）、inter は 2Nx2N で深さ 1。transform skip・scaling list を使わない。
- E4: B は参照にしない（WS329 と同じ規則、Q329-3 の答えに従う）。RPS は GOP の形ごとに SPS に置き、slice は index で指す。
- E5: rate control と GOP の種類・順は WS329 の物を共通にして使う（§5.3）。
- E6: 並列は WPP（D20）。最初は 1 thread、速さの Phase で CTU の行を thread に。
- E7: 試験の口: 再構成の hook と stats（WS329 E20 と同じ形）。

### 9.3 速さと画質の目標（Q330-3 で確かめる）

見積もり（scalar C、1 thread、5330 の P core を 3 G 演算/秒として）:

| 対象 | 目安 | 根拠 |
| --- | --- | --- |
| decoder | 1 thread で 1080p 約 15〜25 fps、720p30 は実時間 | HEVC の decode は H.264 の約 1.5〜2 倍の演算（大きな変換と 8 tap の補間、SAO）。WS326 の H.264 の software decoder の測定（p024）に倍率を掛けて p031 で確かめる |
| encoder FAST 720p | 約 8〜12 fps（1 thread）、WPP 4 thread で 720p30 に近づく | CU の分割の比べで H.264 の FAST の約 2 倍の演算 |
| 画質 | BALANCED が同じ preset の自前の H.264 encoder より BD-rate で 15% 以上良い、x265 の `-preset veryfast -tune psnr` の +60% 以内 | HEVC の利点（大きな CU・変換、merge、SAO）を mode の選びの単純さが削る |

正しさの門（全 preset・全組）: 出力が host の ffmpeg（厳しい誤りの検出）と自前の decoder の両方で誤り無く decode でき、復号の frame が encoder の再構成と全 frame で MD5 一致、decoded picture hash の SEI（encoder が書く）も一致。

## 10. 試験の道

### 10.1 単体試験（host、各実装の Phase）

- 置き場 `plan/ws330/tests/unit/test-<name>.c`、流し方 `sh plan/ws330/tests/run-unit.sh <name>...|all`（WS326 の形: 全部の h265sw-*.c・vcodec-*.c と bits.c を ASan・UBSan で link、未実装は ENOSYS の骨格、最後の行 `run-unit: PASS`）。`check.h` は WS326 の物を include path で使う。p003 で作る。
- **書きの逆の実装**: bit の列は WS329 の製品の `vcodec-bitwriter`、CABAC の bin の列は `vcodec-cabac-enc`（どちらも WS329 で decoder と往復で確かめ済みの、decoder の code を使わない独立の実装）。syntax element の 2 値化（TR・EGk・part_mode など）は試験の中で仕様の表から手で書く（decoder の 2 値化の code を使わない）。
- 期待の値の作り方: (a) 仕様の表の値（Table 8-10、8-12、9-45〜9-47 など）、(b) 手で計算した小さな例（式の番号を comment に）、(c) 書いて読む往復、(d) 性質（DC だけの係数の逆変換は平ら、DST・DCT の基底の直交性、merge の候補の数は常に MaxNumMergeCand、SAO の offset 0 は不変）。他の decoder の出力を期待の値にしない（全体の試験の副の oracle を除く）。
- 表の写し間違い: initValue（Table 9-5〜9-31）は WS326 p010 の `cabac-init-extract.py` と同じ形の独立の抜き出しと全値の照合、transMatrix は直交性（行の内積）と対称性、chroma の filter は係数の和 64、intraPredAngle は Table 8-5 の対称性。
- 誤りの試験: 各 module で、範囲外の値・途中で切れた入力・巨大な数・乱数の bit 列で、落ちず（ASan・UBSan が黙る）決められた誤りを返す。
- わざと壊す確かめ（標準廉価実装手順 §5.3）は必須。

### 10.2 全体の試験（p028、host）

- host の道具 `plan/ws330/tests/stream/decode-h265.c`（p028 で作る、WS326 の decode-stream.c の形）: Annex B の file を NAL ごとに decoder に与え、出力の frame を crop して I420 で並べ、frame ごとの MD5 と全体の MD5、stats（hash の一致・不一致・無し、concealed、generated）を出す。
- `plan/ws330/tests/run-stream.sh`: host の ffmpeg の libx265 で短い stream を作り（lavfi の `testsrc2`・`mandelbrot`、pipe で渡し大きな file を作らない）、自前の decoder と `ffmpeg -f framemd5` の frame ごとの MD5 が一致し、x265 が書く decoded picture hash の SEI（`--hash 1`）が全 picture で一致することを確かめる。組: CTU 16・32・64、WPP あり・無し、tile（`--tiles` は x265 に無いので conformance の TILES_*）、AMP、SAO あり・無し、B と参照の B（b-pyramid）、weighted、scaling list（`--scaling-list default`）、transform skip、sign hiding、PCM（conformance）、複数 slice（`--slices`）、長い GOP と短い GOP、open GOP（CRA）。最後の行 `run-stream: PASS`。
- 壊した stream: 乱数の byte の書き換え・切り詰め・NAL の欠けを 200 通り（WS326 p021 と同じ形）で、落ちず止まらず次の IRAP から戻ること。

### 10.3 適合の試験（p029）

- 一覧 [tests/conformance/list.txt](tests/conformance/list.txt)（147 本。名前から `in` 136 本・`out:main10` 11 本の推定。p029 で decoder 自身の SPS の判定で最終の分類をし、食い違えば調べて記録）。取得 [tests/conformance/fetch.sh](tests/conformance/fetch.sh)（パラメータの無い GET、SHA-256 を [tests/conformance/sha256.txt](tests/conformance/sha256.txt) に記録して照合、zip でない答えは即止める）。cache は `$H265SW_CONFORMANCE_DIR`（共有の推しは main の `build/sources/h265-conformance/`、WS318 §13.3 と同じ場所、§14-4）。
- p001 で 3 本（AMVP_A_MTK_4・SAO_A_MediaTek_4・WPP_B_ericsson_MAIN_2）を取得し、SHA-256 を記録、`fetch.sh` が cache の照合で PASS することを確かめた。AMVP_A の `.md5` は host の ffmpeg の全 frame の I420 の MD5 と一致した（`5dd1b109…`、30 frame、416x240）。SAO_A（60 個）と WPP_B（48 個）は decoded picture hash の SEI を持ち、AMVP_A は持たない。
- 判定: 道具が zip から `unzip -p` で bitstream と MD5 の file を読み（展開しない）、(1) 全 frame の I420（crop 後、出力の順）の MD5 が zip の MD5 と一致、(2) SEI の hash がある stream は全 picture で一致、(3) stats の concealed と generated が 0（RAP・NoOutPrior・CONFWIN などの意図した例外は readme で確かめて記録）。受け入れ: `in` の全部が一致。tile・WPP・dependent slice・PCM・transform skip・scaling list・weighted・long-term・TMVP・AMP・SAO・deblocking の制御・cu_qp_delta・sign hiding・CRA と RASL・BLA・NoOutPrior・bumping・POC・RPS・VPS id の各々が 1 本以上あること（名前から全部ある）。

### 10.4 encoder（p046〜p047）

WS329 §10.2 と同じ形: host の道具が raw の I420 を stdin から読み `.265` を書き、encoder の再構成の MD5 と PSNR を出す。`roundtrip.sh` が (a) ffmpeg（`-err_detect +crccheck+bitstream+buffer+explode`）、(b) 自前の decoder、(c) encoder の再構成の 3 者の MD5 と SEI の hash の一致を確かめる。HM の参照の decoder を厳しい oracle に足すかは Q1（§14-5）。

### 10.5 libmedia・QEMU・実機

- p030: WS318 の decoder の層の host 試験（遅れた拒否の切り替え、問い合わせの行）、`run-host-decode`（MP4・MKV・TS の HEVC を h265-software で ffmpeg と一致）。
- QEMU（Venus の amd64）: Video Player で HEVC の file を再生（console の log で判定しない、AGENTS §9）。
- 実機: 速さ（p031・p049）は Q1 が 5330・Pi 5 を割り当てる。

## 11. libmedia・WS318 との関係

- decode の backend の順（WS318 §5.4）: vulkan-video → **h265-software（この WS）** → libavcodec:hevc。WS326 p023 の遅れた拒否の切り替えをそのまま使う。
- encode: WS318 §5.3 の encoder の ops の表に `h265-software` を hardware の後・avcodec の前に（WS329 と同じ位置の規則）。問い合わせの行: kind SOFTWARE、名前 "h265-software"、profiles Main、max_level 186（6.2）、最大の大きさ 16888、rate_controls CQP・CBR・VBR、max_b_frames 3、max_references 4、memory CPU、formats I420・NV12。
- WS318 の H.265 の Vulkan Video の解析（p016〜p019）: Q330-2 の推しでは、WS318 p016 がこの proposed の `h265sw-nal`・`-ps`・`-vui`・`-rps`・`-scaling`・`-slice`・`-poc`・`-dpb` の header を設計の入力として採り、WS318 p017〜p019 がその .c を実装する（標準廉価実装手順、Sonnet）。WS330 の p004・p007〜p012 はその時 main にある実装を確かめ、無い物だけを実装する Phase になる（Phase の表の「WS318 で済めば確かめだけ」の印）。StdVideoH265 への写しは WS318 の module。
- 出力の形（staging の CPU の frame）・thread の台帳（WS318 p008）・packet の枠（hvcC、WS318 p012a の `packet-format.c`）は WS318 の物を使う。

## 12. HAL・toolchain・他の WS

- HAL: 変えない。decoder・encoder は libmedia の userland の C だけで、kernel・driver・HAL の契約に触れない。差分の案（`proposed/` の HAL）は無い。
- toolchain・共有の build/: 変えない。host の試験は host の cc・clang・ffmpeg（libx265 あり、確かめた）。
- WS326: p026 への名前と形の入力（§5.1）、p002 で header の基点が移る（§5.2）。
- WS329: p003 の表の名前（§14-3）、p032 で encoder の共通化（§5.3）。
- WS318: Q330-2（解析の共有）、p030・p048 の依存（backend の形）。
- WS320・WS321（Pi 4・Pi 5 の HEVC の Vulkan Video）: 実機の HEVC の hardware の試験に、この decoder の出力を比べる物差しとして使える（記録だけ）。

## 13. Phase の表

型: O = Opus（phase-runner）、S = Sonnet 5.5 high（phase-runner-low、標準廉価実装手順、1 回の起動）、D = conformance-reviewer。S の Phase は header 1〜2 個。各 phase.md は `phaseNNN/phase.md`（共通の節は §13.1）。encoder の Phase（p034〜p048）の phase.md は p033 が書く（D19）。

| Phase | 目的（module） | 型 | 依存 | 受け入れ |
| --- | --- | --- | --- | --- |
| p001 | 設計（この文書、proposed、問い、review） | O | — | Q1 の受け入れ、Q330 の回答 |
| p002 | decoder 側の共通化 C1〜C4（vcodec.h・vcodec-frame・vcodec-output、p026 の残り）、H.264 の decoder を移す | O | WS329 completed、WS326 p026 | WS326 の run-unit all・run-stream・適合と WS329 の run-unit・roundtrip が前後で同じ、新しい test-vcodec_frame・test-vcodec_output、amd64 build warning 0 |
| p003 | H.265 の decoder の header（proposed → source、p026・p002 の名前に合わせる）・.c の骨格（ENOSYS）・Makefile・run-unit.sh・check-skeleton.sh・試験の補助（syntax の書き、MD5）・BASE | O | p002 | check-skeleton PASS、amd64 build warning 0、H.264 の試験が変わらず PASS |
| p004 | h265sw-nal、h265sw-sei | S | p003 | run-unit（nal・sei）、MD5 の RFC 1321 の付録の値、CRC と checksum の手計算 |
| p005 | h265sw-scan、h265sw-pic | S | p003 | 走査の表を節の loop から導き直して全一致、tile の 6-3〜6-9 の手の例、可用性の境の例 |
| p006 | h265sw-scaling | S | p005 | Table 7-5・7-6 の照合、予測と DC、7-44〜7-49 の配置 |
| p007 | h265sw-rps | S | p003 | 7-61・7-62 の手の例、上限の境、乱数の入力 |
| p008 | h265sw-vui | S | p003 | E.2 の全 field を書いて読む、HRD の入れ子、Table E.1 |
| p009 | h265sw-ps | S | p006、p007、p008 | SPS・PPS の往復（vcodec-bitwriter で組む）、範囲の境、ENOTSUP の分類、VUI の誤りの規則 |
| p010 | h265sw-slice | S | p004、p009 | slice header の往復（全分岐）、dependent の写し、範囲の境 |
| p011 | h265sw-poc | S | p010 | 8-1・8-2 の折り返しの例、prevTid0Pic の規則、8-5 の LT の msb |
| p012 | h265sw-dpb | S | p011、p002 | RPS の marking の例、8.3.3、C.5.2.2・C.5.2.3 の bumping と latency の手の表 |
| p013 | h265sw-reflist | S | p012 | 8-8〜8-11 の例（modification・cycle）、ColPic、NoBackwardPredFlag |
| p014 | h265sw-cabac-init | S | p003 | initValue の全値を PDF の独立の抜き出しと照合、9-6 の手計算 |
| p015 | h265sw-cabac-se | S | p014 | 各要素を vcodec-cabac-enc で bin を書いて読む往復、Table 9-45〜9-47 |
| p016 | h265sw-residual | S | p015、p005 | 係数の block を書いて読む往復（全 scanIdx・大きさ・SDH・Rice の更新）、範囲外で EINVAL |
| p017 | h265sw-intra | S | p003 | MPM の全分岐、Table 8-2、置き換え・filter・各 mode の手計算と性質 |
| p018 | h265sw-transform | S | p003 | Table 8-10、8-309 の手計算、DST と DCT の直交性と既知の入力、skip と bypass |
| p019 | h265sw-interp | S | p003 | 8 tap・4 tap の全分数の手計算、端の clamp、重みの 8-262〜8-277 |
| p020 | h265sw-tmvp | S | p005、p002 | 8-205〜8-209 の手計算、候補の位置、LT の不一致、保存 |
| p021 | h265sw-merge | S | p020 | 空間の候補の各規則、並列の merge level、組み合わせの Table 8-7、0 の候補、8x4 の規則 |
| p022 | h265sw-amvp | S | p020 | A・B の scaling と isScaled、時間、重複の除去、8-94〜8-97 の折り返し |
| p023 | h265sw-ctu | S | p009、p010、p015、p016、p017 | CTU を書いて parse し syntax と pic の entry が一致（全 PartMode、PCM、TT の推定の分割、QG） |
| p024 | h265sw-recon | S | p013、p018、p019、p021、p022、p023 | 性質の試験（intra の平らな画、inter の整数 MV の複写、bypass、PCM）、QP の予測の例、edge と coded |
| p025 | h265sw-deblock | S | p005 | bS の各規則、Table 8-12、強弱の判定と filter の手計算、境界の除外 |
| p026 | h265sw-sao | S | p005 | band と edge の手計算、境界と NOFILTER の除外、写しからの読み |
| p027 | h265sw-slicedata | S | p023、p024 | WPP の保存と同期・tile の初期化・dependent・end_of_subset の作った stream、QP の reset |
| p028 | h265sw-decoder の全体、host の道具と run-stream.sh、最初の全体の試験 | O | p004〜p027 | libx265 の組（§10.2）で ffmpeg と全 frame 一致・SEI の hash 一致、壊した stream 200 通り |
| p029 | 適合の試験（HEVC_v1） | O | p028 | §10.3 の `in` の全部が一致 |
| p030 | libmedia の backend（共通の backend の核を出す） | O | p029、WS318 completed | WS318 の decoder の層の試験、run-host-decode（HEVC）、amd64 build warning 0、QEMU の Video Player |
| p031 | decoder の速さ（測り、近道、WPP・tile の thread、実機） | O | p029、p030 | Q330-3 の目標、近道の前後で MD5 が同じ |
| p032 | encoder 側の共通化 E1〜E5（WS329 の encoder を移す） | O | p029、WS329 completed | WS329 の run-unit・roundtrip の MD5 が前後で同じ |
| p033 | encoder の header の設計（h265enc-*）・骨格・試験の土台・p034〜p045 の phase.md・design-reviewer | O | p032 | design-reviewer の反映、check-skeleton PASS、BASE |
| p034 | h265enc-ps | S | p033 | 往復（h265sw_ps_read_*）、level の選び |
| p035 | h265enc-slice | S | p034 | 往復（h265sw_slice_read_*）、RPS の index |
| p036 | h265enc-cabac | S | p033 | 往復（h265sw-cabac-se の各要素） |
| p037 | h265enc-residual | S | p036 | 往復（h265sw_residual_decode）、SDH の判定 |
| p038 | h265enc-quant | S | p033 | 前向きと逆の誤差の性質、手の値 |
| p039 | h265enc-refplanes（と vcodec-me の H.265 の予測の関数） | S | p033 | 全分数で h265sw_interp_luma と一致 |
| p040 | h265enc-intra | S | p038 | 明らかな mode、再構成が recon と一致 |
| p041 | h265enc-ctuwrite | S | p035、p037 | CTU を書いて h265sw_ctu_parse で syntax が一致 |
| p042 | h265enc-cu | O | p040、p041 | 符号化 → 書き → decoder で再構成が一致 |
| p043 | h265enc-mode（preset、分割と mode） | S | p039、p042 | 合成の画で skip・merge・分割・intra の選び、一致 |
| p044 | h265enc-sao、h265enc-gop | S | p042 | SAO の決定の性質、RPS と POC の手の表 |
| p045 | h265enc-encoder の全体、host の道具と roundtrip.sh | O | p034〜p044 | §10.4 の基本の組で 3 者の MD5 と hash の一致 |
| p046 | 品質と適合の行列（全組、BD-rate） | O | p045 | §9.3 の正しさの門、Q330-3 の画質 |
| p047 | libmedia の encoder の backend | O | p045、WS318 p012a | WS318 の encoder の試験 |
| p048 | encoder の速さ（WPP の thread、実機） | O | p046、p047 | Q330-3 の速さ |
| p049 | 規約の全文の見直し、header の持ち越しの直し | D | p031、p046〜p048 | conformance-reviewer の checklist、全試験 PASS |

- 並べられる組（p003 の後）: {p004}、{p005 → (p006、p020 → (p021、p022)、p025、p026)}、{p007、p008} → p009 → p010 → p011 → p012 → p013、{p014 → p015 → p016}、p017、p018、p019。推しの起動の順: p004・p005・p007・p008・p014 を並べ（最大 5）、次に p006・p017・p018・p019・p020、…。
- p004・p007〜p012 は Q330-2 の答えが推しで WS318 p017〜p019 が先に実装した時は「確かめと足りない物だけ」の Phase になる。

### 13.1 実装の Phase（S）の共通の節

各 phase.md は次を共通に指す（標準廉価実装手順 §4.6）:

- **読む物（共通）**: AGENTS.md §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文、この文書の §1・§2・§3・§7・§8・§10.1、`vcodec.h`・`h265sw.h`、実装する header とそれが include する header、仕様の PDF（§1 の path と SHA-256、頁の換算 +18）。
- **変えてはいけない物**: 全部の `h265sw*.h`・`vcodec-*.h`・`h264sw*.h`・`h264enc*.h`、担当の外の `.c`、Makefile、`plan/ws330/tests/run-unit.sh`・`check-skeleton.sh`・共通の試験の補助（自分の test を除く）、HAL・toolchain・共有の build/。header の不備は直さず止めて Q1 へ（どの header のどの関数で何が足りないか）。
- **確かめの command**: `timeout 900 sh plan/ws330/tests/run-unit.sh <name>` と `all`、`BASE=<p003 の SHA> timeout 300 sh plan/ws330/tests/check-skeleton.sh`、WS326 の `sh plan/ws326/tests/run-unit.sh all`（H.264 が変わっていない）、zedBSD amd64 の libmedia.so の build（WS326 design §6 の command）で warning 0、`git diff --check`。
- **終わりの条件**: 上が全部 PASS（出力を自分で読む）、わざと壊す確かめ、規約の checklist、`git add` の後 `git commit -m WIP -- <path>`、phase.md の「結果」（日付は host の `date`、command と件数、自分で決めた点、未実施）、ws.md の行を「実装済み・試験 PASS」（cleared は Q1）。

## 14. Q1 への注意（ユーザーの判断ではない物）

1. 仕様の PDF を main の `build/sources/specs/itu-t-h265-202601.pdf` へ写してほしい（今は `/home/awe/zedBSD-worktrees/p5-ws330/build/tmp/ws330-spec/itu-t-h265-202601.pdf`、SHA-256 `78b09348…d4d2`）。phase.md はその path を書いた。worktree の `build/tmp/ws330-spec/`（PDF と index の html）と `build/tmp/ws330-conf/`（3 つの zip、約 7 MB）と `build/tmp/ws330-hdr/` は使い捨て（Q1 が消す）。
2. WS326 p026 の設計に [proposed/common/](proposed/common/) の `vcodec-bits`・`vcodec-nal`・`vcodec-cabac`（と `vcodec.h`）を入力として渡してほしい。p026 が frame と出力の queue まで移すなら `vcodec-frame`・`vcodec-output` も（その時 WS330 p002 は小さくなる）。
3. WS329 p003 が proposed の header を source に置く時、`vcodec-cabac-enc.h` の表の参照（`h264sw_cabac_range_lps` など）を共通化の後の名前（`vcodec_cabac_range_lps` など）に、comment の「H.265 9.3.4.3」を「H.265 9.3.5」（V11 の encoder の engine の節）にしてほしい。
4. 適合の cache: zip だけで 129 MB。main の `build/sources/h265-conformance/` に置くのを推す（WS318 §13.3 も同じ場所を予定、共有）。展開しない（`unzip -p`）ので disk はこれだけ。取得は p029 の前に Q1 が `H265SW_CONFORMANCE_DIR=… sh plan/ws330/tests/conformance/fetch.sh` で（パラメータの無い GET 147 回）。
5. HM の参照の decoder（ITU-T/ISO の配布の C++、host でだけ build）を encoder の厳しい oracle に足すか（WS329 §14-4 の JM と同じ問い）。推しは「足す」（p046 の前、取得は zip 1 つ、SHA-256 を記録）。master.md の web の取得の条件に照らして Q1 が決める。
6. WS318 の記録: Q330-2 が推しで決まれば、WS318 p016 の設計の入力にこの proposed の解析の header を、D19 を「H.265 の解析は WS330 の Vulkan に依らない module を共有」に直す記録を。
7. ws.md の「ユーザーの決定（2026-10-30、原文）」は Q1 が書いた原文の記録。AGENTS §8（原文を載せない）との整理は Q1（この Phase では変えていない）。
8. host の disk が少ない（2026-10-31 に約 7.7 GB 空き）。§10.2 の試験は YUV を pipe で渡し file を作らない形にした。

## 15. design-reviewer の結果と反映

（design-reviewer の後に書く。全件は [design-review.md](design-review.md)。）
