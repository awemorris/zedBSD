<!-- awesome-plan project=zedbsd record=ws202-p015 -->

# ws202-p015: H.264 の frame_num の gap・MMCO 5・seek の後の DPB

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 4 LW
依存: p008、J4（試験の stream の出典）

## 目的

vkvideo-probe に無い参照の管理（design §5.3・§5.7）を足し、open GOP の seek の後も DPB が保たれ、空の slot を参照しないことを確かめる（review-001 H-02）。

## 成果（`userland/desktop/libmedia/h264*.c`）

1. frame_num の gap（8.2.5.2）: 欠けた frame_num ごとに non-existing の frame を短期の参照として sliding window に入れる（slot を使わず、参照の list・表示に出さない）。
   `gaps_in_frame_num_value_allowed_flag` 0 の stream の欠け（seek の後に leading を捨てた時、壊れた file）も同じ処理。
2. MMCO 5（8.2.1・8.2.5.4.6）: 全部の参照を unused に、POC の状態（prevPicOrderCntMsb・Lsb、prevFrameNumOffset、prevFrameNum）の reset、tempPicOrderCnt、
   表示順の待ち行列を全部出す。
3. seek の後の開始（design §5.7）: `h264_dpb_restart`、最初の I（IDR か全 slice が I）だけを受け、非 IDR の I の時は `prevRefFrameNum` と POC の状態をその I から。
   leading の B（I より POC が小さい）は捨てる。
4. D18 の検べ: 計画の参照の slot が picture を持たない（non-existing の frame を含む）時は、その picture を「捨てる」と返す。
5. host 試験（`run-host-h264.sh` に足す）:
   - `h264-high-b-aac.mp4` の各 sync sample（非 IDR の I、design §15 E3）から始めて最後まで: 失敗 0、捨てた数は leading の B だけ、後の P・B の参照の list が全部 slot を
     持つ（D18 で捨てる物が 0）、表示の POC の順が ffmpeg の同じ区間の表示順と一致。
   - J4 の回答に従い、MMCO 5・gap（`gaps_in_frame_num_value_allowed_flag` 1）・冗長 slice の stream で POC・marking・表示順を確かめる:
     - (推し) ITU-T H.264.1 の conformance の bitstream: `plan/ws202/tests/fetch-conformance.sh` が取得して SHA-256 を確かめ、`build/` に置く（tree に入れない）。
       入手先と利用の条件を読み phase.md に記録（U13）。POC・表示順の期待は package の参照（yuv の md5 の frame の順）か ffmpeg の `-show_frames` から。
     - 手の bit 列の時は slice header の列だけで POC と marking を確かめる（decode はしない）。
   - ASO の stream があれば（Baseline の conformance に含まれる見込み）、parser が受けることだけを確かめる（hardware で扱えるかは U9、5330 で）。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/run-host-h264.sh` | p008 と p015 の全項目 PASS |
| libmedia の build | warning 0 |

## 注意

- conformance の stream は tree に入れない。5330 の hash の確かめに使うなら p012 の依頼に（T1 が host から scp）。
