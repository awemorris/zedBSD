<!-- awesome-plan project=zedbsd record=ws202-p015 -->

# ws202-p015: H.264 の欠けた参照・frame_num の gap・MMCO 5・seek の後の DPB

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 5 LW
依存: p008、J4（試験の stream の出典）

## 目的

vkvideo-probe に無い DPB の扱い（design §5.3・§5.7）を足す: 欠けた参照の entry（slot を持たない参照）、frame_num の gap、MMCO 5、seek の後の開始、MMCO の対象が
無い時の扱い。参照の list の計算と、欠けた参照を使う picture の判定（D25）は p016。

## 成果（`userland/desktop/libmedia/h264-dpb.c`・`h264.c`）

1. 欠けた参照の entry（design §5.3、H2-01・M3-04・L3-03）: DPB の entry の表（最大 16）を slot と別にし、entry は slot を持つ物と持たない物がある。持たない物は (1) gap の
   non-existing の frame、(2) decode しなかった参照の picture（D25 で捨てた参照、seek の後の leading の参照の picture、result status ERROR の参照）。marking（sliding window・
   MMCO・長期）は普通の参照と同じに数え、押し出されて消える。Vulkan の計画（setup・参照の slot）には slot を持つ entry だけが出る。probe の slot ごとの `device_active` と
   begin での deactivate（`dpb.c` 126〜133・160〜171）は slot の側に残す。seek の前の picture は entry にしない（parse していない）。
2. frame_num の gap（8.2.5.2）: 欠けた frame_num ごとに non-existing の frame を短期の参照として入れる（`gaps_in_frame_num_value_allowed_flag` 0 の stream の欠けも同じ）。
   - non-existing の POC と POC の状態（H3-01、design §5.3）: type 1・2 は 8.2.1.2・8.2.1.3 の式を nal_ref_idc ≠ 0 として当てて POC を持ち、prevFrameNum・prevFrameNumOffset を
     non-existing の frame ごとに進める。type 0 は POC を「不明」の印にし、prevPicOrderCntMsb・Lsb を変えない。
   - 規格（ITU-T H.264、無料で取れる）の 8.2.1・8.2.5.2 を読んで照らし、違えば design §5.3 を直す差分を Q1 に送る（U20）。
   - gap が `max_num_ref_frames` より大きい時は、短期の参照を全部外し、最後の `max_num_ref_frames −（長期の数）` 個の non-existing だけを入れる（L3-05）。
3. MMCO 5（8.2.1・8.2.5.4.6）: 全部の参照を unused に、POC の状態の reset、tempPicOrderCnt、表示順の待ち行列を全部出す。
4. MMCO の対象が DPB に無い時は何もしない（seek の後の P の MMCO 1 が前の GOP の参照を外す: review-002 R2。probe は止まる所）。
5. seek の後の開始（design §5.7）: `h264_dpb_restart`、最初の I（IDR か全 slice が I）だけを受け、非 IDR の I の時は `prevRefFrameNum` をその I の frame_num に。POC の状態
   （L2-16）: type 0 は prevPicOrderCntMsb = prevPicOrderCntLsb = 0、type 1・2 は prevFrameNumOffset = 0 と prevFrameNum = I の frame_num で FrameNumOffset 0 から。
   leading の picture（I より POC が小さい）は decode しない。**参照（nal_ref_idc ≠ 0）なら slot 無しの entry として marking に通す**（M3-04、design §5.7。通さないと次の
   frame_num が飛んで POC の不明な non-existing が入る）。
6. host 試験（`run-host-h264.sh` に足す、design §10.2 の (2)・(4)・(5)）:
   - seek の後（R2）: `h264-high-b-aac.mp4` の各 sync sample（非 IDR の I）から始めて最後まで: 捨てるのは leading の非参照の B 1 枚だけ、P の MMCO 1 の対象が無いのを無視、
     以後の DPB の計画が通しの decode の計画と同じ（slot の番号を除く）。x264 の open GOP では gap は起きない（design §5.7 の事実）ので、gap の確かめはこの stream ではしない。
   - gap（M2-09）: `h264-gap.mp4` で non-existing の frame が sliding window に入り、`max_num_ref_frames` 枚の後に押し出される。次の I の後は欠けた参照が DPB に無い。
     non-existing の POC は type 0 で不明の印、prevPicOrderCntMsb・Lsb が変わらない。（どの picture を decode するかの判定と正解との比べは p016 の試験。）
   - leading の参照の picture の marking: 手で作った slice header の列（非 IDR の I、参照の leading の B、P）で、leading の B が entry になり、次の P で gap と見なされない。
   - 大きな gap（L3-05）: frame_num が `max_num_ref_frames` より大きく飛ぶ手の列で、1 つずつ回した時と同じ DPB。
   - MMCO 5・冗長 slice: J4 の回答に従う。
     - (推し) ITU-T H.264.1 の conformance の bitstream: `plan/ws202/tests/fetch-conformance.sh` が取得して SHA-256 を確かめ、`build/` に置く（tree に入れない）。入手先と
       利用の条件を読み phase.md に記録（U13）。POC・表示順の期待は package の参照（yuv の md5 の frame の順）か ffmpeg の `-show_frames` から。
     - 手の bit 列の時は slice header の列だけで POC と marking を確かめる（decode はしない）。
   - POC type 1・2 の非 IDR の I からの開始と、gap の non-existing の POC・prevFrameNum・prevFrameNumOffset の進め方（手の bit 列、8.2.1.2・8.2.1.3 の式で期待を計算）。
   - ASO の stream があれば（Baseline の conformance）、parser が受けることだけ（hardware は U9、5330 で）。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/run-host-h264.sh` | p008 と p015 の全項目 PASS（ASan/UBSan） |
| design §10.6 の build（`ZEDBSD_CONFIG=config/ci/config-amd64.mk`、libmedia・libbrowser・videoplayer・music） | exit 0、`grep -c 'warning:'` が 0 |

## 注意

- conformance の stream は tree に入れない。5330 で使うなら p012 の依頼に（T1 が host から scp）。
