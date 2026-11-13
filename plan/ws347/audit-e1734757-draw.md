# 監査 e1734757: 描画が画素を書かない件（2026-11-14 Q1・P5）

Claude（P5）が値で観測して突き止めた（amaterrace-gpu e1734757、KC705 の形、三角形 1 つの scene で再現）。RTL は Claude が変えず、診断の写しで確かめた（/home/awe/amaterrace-worktrees/p5-drawdbg/p5own/）。

## A（RTL の誤り、agy に修正を依頼）

1. **atg_vf_fetch の line buffer が 32 byte の line を前提**（1d1b930e から）: `rtl/gpu/vf/atg_vf_fetch.v` 664（32 個の slice）、632-633（256 bit の pair）、714、414-431・1178-1210（整列が off_r[4:0] だけ）。LINE_BYTES 64 では line の後半 32 byte を失い、offset ≥ 32 の attribute が 32 byte 手前を読む（頂点 2 が頂点 0 になり三角形が退化）。
2. **atg_rx_clip の trivial accept を 1 cycle 早く取る**（d1004535 から、aafba516 の CPOLY=0 で致命的）: 601-605 の `triv_step == 2'd2` で r_accept を取るが、最後の頂点の段 2 の値は triv_step==3 で揃う。CPOLY=0 では不受理の三角形が捨てられる。
3. **FENCE が draw の終わりを待たない**: `rtl/gpu/rx/atg_rx.v` 277 の busy に入力の skid（145-166）の占有が入らない。VF→RX の handshake の次の cycle に全 unit の busy が 0 になり、その次に FENCE が出る（ghci.md:122 に反する）。
4. **atg_rx_scan が E を quad の原点での値として使う**（d1004535 で引き算が消えた）: 221-231。仕様（spec/gpu/rx/atg_rx_scan.md:34、atg_rx_setup.md:33）では E は bbox の原点の画素の中心での値。被覆が (−1,−1) ずれる。単体の atg_rx_scan も FAIL。

写しで 1〜4 を直すと atg_top_shade は三角形 6 を除いて PASS（2508 → 1168 word、残りは 6 のため）。

## B（仕様の問い・試験の判断）

5. **CTRL.RESET の replay が未初期化の STATE を配る**: `rtl/gpu/cp/atg_cp_state.v` 63-64。STATE の RAM は reset しない設計で、RESET の後に STATE[0..767] を再送する。書かれていない entry（例 0x1D0.. ATTR_LIMIT）が X で配られ、VF の FSM が全 0 になり atg_top_fault の draw が動かない。STATE を時刻 0 に 0 で埋めると e1734757 のままで atg_top_fault が全 PASS。ghci.md:858 は一部の既定値だけを書き、STATE 全体の power-on の値を決めていない（KC705 の BRAM は 0 で始まるが ASIC では不定）→ ledger の B36。
6. **近平面を貫く三角形の期待値**: tools/rx-shade-scene.py が M.clip（CPOLY あり）で期待値を作るが、minimal（CPOLY=0）は ghci.md:2415 のとおり棄却しうる。Q1 の判断: 試験の model を構成の CAP_CLIP_POLY に合わせる（試験の側の直し、agy に依頼）。

## 未実施

atg_top_tex・raster・msaa・core（同じ原因の見込み、未確認）。3 の他の skid・FIFO の隙間の監査。「e8262a9f から壊れた」という前の記録は誤りで、原因の commit は 1d1b930e・d1004535・aafba516。
