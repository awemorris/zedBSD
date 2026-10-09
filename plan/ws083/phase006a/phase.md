<!-- awesome-plan project=zedbsd record=ws083-p006a -->

# ws083-p006a: P・B と DPB、scaling list、複数 slice の host の試験

Status: cleared（2026-10-10 Q1 判定: 5330 の実機の p005・p006b と照合）（旧: cleared 候補（2026-10-10 P2 の照合: design §9 の受け入れ（§8.1 の P・B PASS）を満たし、P・B の実機の hash（p006b）も 5330 で一致。判定は Q1）（旧: in-progress（P2、2026-10-08 Q1 の指示「待つ間に ws083-p006a を進めてよい」。host の範囲は実装と試験済み））
Disposition: normal
Parent: [WS083](../ws.md)

## 範囲

[design.md](../design.md) §9 の p006a: P・B と DPB、scaling list の fall-back、複数 slice の host の golden（実機は要らない、実機の hash は p006b）。

## 実装

- kernel の側: p004 の builder が参照（DPB_STATE・PICID・参照の address・direct の MV・POC list）、scaling list の fall-back（Table 7-2 の A・B、default の flag）、複数 slice（SLICEADDR の先読み）を既に書く。p004 の host の golden（`host-mfx-avc.c` の references・intra-cqm・flat・defaults）がそれを genxml で確かめている。変更は無し。
- `userland/tests/vkvideo-probe/h264.c`: slice header を参照の marking まで読む（redundant_pic_cnt、direct の flag、active の参照の数、list の modification、weight table、dec_ref_pic_marking と MMCO）。access unit の開始の位置（試験で ffmpeg と比べる）。
- `userland/tests/vkvideo-probe/dpb.[ch]`（新）: slot ごとに 1 枚の image、H.264 8.2.5 の marking（IDR、sliding window、MMCO 1〜4・6、MMCO 5 と frame_num の gap は扱わず停止）、decode ごとの計画（最初の decode の reset、DPB の全ての参照とその StdVideoDecodeH264ReferenceInfo、参照でない空きの slot を setup に、もう参照でない slot を begin で無効に）。
- `userland/tests/vkvideo-probe/main.c`: session は max_num_ref_frames ＋ 1 の slot、slot ごとの image、DPB の計画どおりに begin（参照は slot つき、無効の slot は NULL、書く picture は −1）・decode（参照と setup）、表示順の出力（IDR の間で POC 順に並べて hash を出し、`--expect` と比べる）。I だけの制限を外した。
- 試験の stream（`make-streams.sh` に追加、合計 約 115 KiB）: p-baseline-64（P だけ、POC type 2、参照 3、10 frame）、pb-main-352（B 2 枚、POC type 0、weighted P、list の modification、2 slice、15 frame）、pb-high-352-pyramid（B pyramid（参照の B）で MMCO 1、weighted、8x8、参照 4、15 frame）。

## 確認

| コマンド | 結果 |
| --- | --- |
| `sh plan/ws083/tests/run-host-vkvideo-probe.sh` | PASS: p004 の 3 本の de-tile・hash に加え、6 本全部（I 3 本・P/B 3 本）の DPB の計画（参照が active で重複なし、max_num_ref_frames 以内、書く slot が参照に無い、P・B は参照を持つ、無効にする slot は decoder が持つ空きの slot）と marking が通り、POC 順の出力の access unit の位置が ffprobe の表示順の frame の pkt_pos と一致（ASan/UBSan） |
| 同じ試験を tree の外の `/home/awe/zedbsd-media/sample-h264-{baseline,main,high}.h264`（1920x1080、150 frame） | 3 本とも計画と marking が通り、表示順が ffprobe と一致（444・482・482 の参照の受け渡し） |
| `make … build/p2-k/bin/vkvideo-probe` | 成功 warning 0 |

未実施: 実機の decode（p006b）。参照の正しさ（hardware が正しい参照を読むか）は実機の hash でしか確かめられない。

## 残り

- p006b: 5330 で P・B の stream の hash（`vkvideo-probe --expect`、tests/streams の 3 本と sample-h264 の 3 本、sample の参照は host の ffmpeg で作る）。

## 2026-10-10 の照合（P2、実機の結果で）

- 受け入れ（design §9 の p006a）: §8.1（P・B）PASS → `run-host-vkvideo-probe.sh` の 6 本の DPB の計画・marking・表示順と、p004 の host の golden（references・intra-cqm・flat・defaults）で満たした（上の表）。
- 実機（p006b の受け入れ、この Phase の受け入れの外だが参照の正しさの確かめ）: 5330 の実機（2026-10-10 Q1、image 588c5cd、boot に `i915.debug=video`、SSH。ws.md の p005・p006b の行）で p-baseline-64 10/10・pb-main-352 15/15・pb-high-352-pyramid 15/15 match、hang・reset の行なし。上の「未実施: 参照の正しさは実機の hash でしか確かめられない」は満たした。
- 588c5cd と照合の時の main（1b08d90b9）の間に `src/drivers/gpu/i915`・`userland/desktop/libvulkan`・`userland/tests/vkvideo-probe`・`src/kern/boot.c`・`include/uapi/gpu-op.h` の差は無い（`git diff --stat` が空）ので、実機の結果は今の code に当たる。host 試験（`plan/ws083/tests/run-host-*.sh` の 8 本: boot-video・libvulkan-native・libvulkan-status・libvulkan-video・mfx-avc・vcs-worker・video-roundtrip・vkvideo-probe）は 1b08d90b9 で全部 PASS（2026-10-10 P2）。
- 残り: 無し。tree の外の 1920x1080 の sample の全 frame の hash は受け入れの外（性能の E で decode を通す）。
