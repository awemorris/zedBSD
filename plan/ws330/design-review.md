<!-- awesome-plan project=zedbsd record=ws330-design-review -->

# WS330 p001 design-reviewer の結果と反映（2026-10-31）

- design-reviewer を 1 回（読むだけ、HEAD 5072c9469、仕様は ITU-T H.265 (V11) の pdftotext と main の PDF の SHA-256 の照合）。30 件: blocker 1・major 11・minor 16。反映は P5（commit は ws.md の Resume point）。
- 反映の確かめ: proposed の header 38 個を cc と clang の `-std=c89 -Wall -Wextra -Werror -Wpedantic` で単独に 2 回 include して compile（PASS）。

| # | 指摘（要点） | 反映 |
| --- | --- | --- |
| B1 | 可用性が「decode 済みの CTB」で判定され、CTB の印付けが parse と recon の後なので、CTU の中の隣が全部使えなくなる | h265sw-pic.h: 可用性は CTB の slice_addr_rs（未到達は UINT32_MAX）と tile で比べ、decoded は concealment の印だけに。h265sw-slicedata.h: slice_addr_rs と segment を parse の前に書く。phase005 の試験の項目を直した |
| M1 | 係数の走査の horizontal・vertical を log2 2・3 だけとした（8x8 の TB の sub-block の順は 2x2 の horizontal を使う） | h265sw-scan.h: log2 0〜3 の全部で 3 種。phase005 に log2 1 の horizontal の試験 |
| M2 | 同じ中身の SPS の再送で generation が増え、CVS の残りを捨てる | h265sw-ps.h: 中身が違う時だけ増やす（0 で消した構造体の byte の比較）。decoder.h の文も。phase009 に試験 |
| M3 | 再利用の frame が生成の picture になると古い motion が残る | vcodec-frame.h に `vcodec_frame_side_reset`、h265sw-dpb.h の generate が必ず呼ぶ。phase012 に試験 |
| M4 | p025（deblock）が transform の QpC を使うのに依存が p005 だけ | p025 の依存に p018（ws.md・design §13・phase025） |
| M5 | tile の幅の和が unsigned で折り返す | h265sw-ps.h: 各値を 1055 以下に読む時に限る、check_pair と scan_tiles は uint64 の和。phase005・009 に試験 |
| M6 | Exp-Golomb の上限が緩く int32 の溢れか 32 bit を超える読み | h265sw-cabac-se.h（cu_qp_delta の EG0 の prefix 4 以下）、h265sw-residual.h（prefix の 1 が 4 + 16 以下、32 bit ずつ uint32 で） |
| M7 | Q330-2 の推し（解析の共有）に header が足りない（範囲での拒否、VPS・HRD・生の RPS） | h265sw-ps.h: SPS は形式で拒まず読んで保ち（chroma・bit depth・range extension の flag の field）、`h265sw_ps_sps_supported` を別に。VPS・HRD・生の RPS は A に決まれば WS318 p016 が p003 の前に足す（design D4、Q330-2 に費用を書いた） |
| M8 | encoder に要る decoder の口（CU ごとの再構成、merge の全候補）が無い | h265sw-recon.h に `h265sw_recon_cu`、h265sw-merge.h に `h265sw_merge_candidates`・`h265sw_merge_restrict` |
| M9 | phase.md が WS326・WS329 の完了で消える path を指す | design §13.1 と各 phase.md: H.264 の回帰は plan/tools.md に登録した path、p003 が読み替える |
| M10 | p023（ctu）と p024（recon）が Sonnet の 1 回に入らない恐れ | CTU・slice data の書きの試験の補助 `h265write.h` を p003（Opus）に。再構成を h265sw-inter（新しい header、p024a）と h265sw-recon（p024b）に分けた。phase024/phase.md は取り下げ（Q1 が `git rm`） |
| M11 | h265enc.h が h264enc.h と契約が違う | 構造体・返り値・decode_time_us・type・qp・parameter_sets・profile_level・send の誤りを h264enc.h に揃えた |
| m1 | transMatrix の式の番号 8-323 は 8-321 の誤り | 直した |
| m2 | 6.4.2 の規則は NxN の物（Nx2N でない） | h265sw-pic.h・phase005 を直した |
| m3 | vcodec_cabac_position の誤りの条件を判定する field が無い | struct vcodec_cabac に terminated |
| m4 | initValue の配置の飛び（cbf の 12〜14、mvd の交互、sig の 126〜131）が書かれていない | h265sw-cabac-init.h の comment に書いた |
| m5 | AMVP に仕様に無い「long-term の不一致」の条件 | 削り、8-171・8-172 の DiffPicOrderCnt 0 に |
| m6 | 存在しない関数 h265sw_scan_diagonal の参照 | h265sw_scan_order( …, H265SW_SCAN_DIAG ) に |
| m7 | dependent segment の文脈の初期化の優先の順が不足 | h265sw-slicedata.h に 9.3.2.1 の順（tile → WPP の行 → Ds → 初期化） |
| m8 | concealment の last_output が別の大きさのことがある | 同じ大きさの時だけ、他は灰（decoder.h） |
| m9 | SPS の活性化は NoRaslOutputFlag 1 の IRAP に限る | decoder.h を直した |
| m10 | 代わりの picture の生成が除去の前で buffer が空かない | generate の前に vcodec_dpb_remove_unused（h265sw-dpb.h） |
| m11 | delta_poc_msb_cycle_lt の範囲の検査が無い | h265sw-slice.h: 0..2^( 32 - log2_max_poc_lsb )、和は uint64 |
| m12 | level の上限を誤りとして扱う | NumPicTotalCurr は H265SW_MAX_RPS まで受ける（h265sw-slice.h）、tile の列と行の超過は ENOTSUP（level 6.2 を超える）と明記。slice segment の 600 は H265SW_MAX_SLICE_SEGMENTS のまま（超えるのは level 7 以上、design の範囲の外） |
| m13 | greater1 の文脈の引き継ぎの書き方 | 「前の sub-block の最後の呼び出し」（9.3.4.2.6）に |
| m14 | D17 は読めてしまう誤った VUI を救えない | design D17 に記録 |
| m15 | 記録の不整合（PDF の写し、design-review.md が無い、ws.md の原文） | §14-1 を直した、この file を作った。ws.md の原文は Q1 の持ち物（design §14-7） |
| m16 | fetch.sh の初回の記録・main での書き換え・.part の残り | phase029 に 3 点を書いた |

レビューが「確かめて正しい」とした所: Table 9-4 と文脈の配置（157）、2 値化と ctxInc（part_mode・chroma・inter_pred_idc・cu_qp_delta・cbf・split_transform）、CABAC の terminate の後の位置、RPS の予測・POC・8.3.2 の印付け・C.5.2.2〜C.5.2.4、8.3.4 の無限 loop の防ぎ、merge・AMVP・TMVP、intra・transform・補間・重み、deblock と SAO の除外の規則、WPP と dependent slice の保存と同期、SEI の hash、NAL の分類、Table A.8 の値、約 70 の式の番号、p002 の共通化の上限と bump の同点の規則、方針（開発の管理情報が無い、出典は block ごと、HAL・toolchain を変えない）。
確かめていない所: 表の値の頁の画との照合（initValue・Table 8-12・transMatrix は実装の Phase で）、D11（HM が EOS で全部を出す）の前提、適合の stream の個々の分類（p029）、WS318 の StdVideoH265 の全 field、phase.md の多くの本文。
