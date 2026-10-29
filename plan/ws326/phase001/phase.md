<!-- awesome-plan project=zedbsd record=ws326-p001 -->

# ws326-p001 module の設計（header の interface と仕様、骨格、試験の道、実装の Phase の割り振り）

- 親: [WS326](../ws.md)。Queue: q1695-i01（P1、Opus）。worktree `/home/awe/zedBSD-worktrees/p1-ws326`、branch `agent/p1-ws326`（基点 main 6b8929fff、途中で main e35523984 を fast-forward で取り込み）。
- 状態: in-progress（2026-10-30 設計・骨格・review の反映が済み、Q1 の受け入れ待ち。cleared は Q1 が決める）。
- 成果: [design.md](../design.md)、`userland/desktop/libmedia/h264sw*.h`（26 個）と `h264sw-*.c`（25 個の骨格、backend の .c は open が MEDIA_PROBLEM_FORMAT を返すだけで decoder.c の表には未登録）、libmedia の Makefile への source の追加、
  `plan/ws326/tests/`（run-unit.sh・check-skeleton.sh・unit/check.h・unit/bitwriter.h・unit/test-bitwriter.c・conformance/list.txt・conformance/fetch.sh・conformance/sha256.txt）、
  phase002〜phase025 の phase.md、ws.md の Phase の表。

## ユーザーの決定と Q1 の追加（原文）

- ws.md の決定 2（Sonnet 5.5 High への外注の構成）。
- 2026-10-30 ユーザー「libmedia の backend の順番は、Vulkan Video --> 独自ソフトウェアdecoder --> libavcodecです。」（Q1 経由）
- Q1（2026-10-30）: Vulkan Video が open の後の最初の packet で profile を拒む「遅れた拒否」も設計に入れる（decoder 層が track と private data を持ち、次の backend に切り替え、同じ packet から。時刻と DPB に注意）。
- Q1（2026-10-30）: key frame 以外の packet での拒否でも止めずに切り替え、次の IDR（か recovery point）まで packet を捨てて続ける。捨てた数を counter か log に。IDR が来ないまま stream が終わった時だけ誤り。
- Q1（2026-10-30）: 仕様の PDF を `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf` に置いた。各 phase.md に path と SHA-256 を書く。git に入れない。

## 結果（2026-10-30）

- module: 26 の header（h264sw.h、bits、nal、tables、ps、slice、frame、poc、dpb、reflist、mb、context（構造体だけ）、cavlc、cabac-init、cabac、cabac-se、macroblock、transform、intra、mvpred、direct、interp、weight、recon、deblock、decoder、backend）。表は [design.md](../design.md) §3。
- 決定: 既存の h264.c（StdVideo の構造体）と分ける、`media_rbsp_unescape` は使い回す（末尾の 0x000003 は nal が先に落とす）、frame だけ、出力は media_picture へ複写、1 thread、backend の順 Vulkan Video → この decoder → libavcodec、遅れた拒否の切り替え（design.md §2）。
- design-reviewer: 1 本、指摘 23 件（blocker 2・major 10・minor 11）を全部反映（design.md §9 の表）。header の基点は fd1d6f88c。
- 実装の Phase: p002〜p025（24 個）。p021（最初の全体の試験）・p022（適合）・p024（速さの設計）は Opus を推す。

## 確かめ

- `sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（各 header の単独の compile と全 .c の compile、cc と clang、`-std=c89 -Wall -Wextra -Werror -Wpedantic`）。
- zedBSD amd64: `make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、log の warning 0（h264sw の 25 個の .c を compile して link）。
- host の libmedia の試験 `sh plan/tools/media/run-host-codec.sh` → `run-host-codec: PASS`（既存の libmedia の回帰が無いこと。この script の source の一覧は固定で h264sw の file を含まないので、h264sw の host の compile は上の check-skeleton で確かめた）。
- `sh plan/ws326/tests/run-unit.sh all` → `test-bitwriter: PASS (14 checks)`、`run-unit: PASS`（試験の補助が表 9-2・9-3 と 7.4.1 の escape どおり）。
- 適合の bitstream の入手: `sh plan/ws326/tests/conformance/fetch.sh AVCv1/BA1_Sony_D AVCv1/CABA1_SVA_B AVCv1/BA_MW_D` → `fetch: PASS (3 zips)`（ITU の公開の FTP の mirror、パラメータの無い GET、bot の確認なし）、SHA-256 を `conformance/sha256.txt` に記録。全 204 本の取得（約 3.1 GB）は p022 で（cache の置き場所は Q1 の判断）。
- `git diff --check` PASS。
- 未実施: QEMU・実機（この Phase は設計だけ）、表の値の入力と照合（p003・p009・p010・p011・p020）、header の規約の全文の照合（p025 で .c と一緒に）。

## Q1 への判断の依頼

- h264sw.h の仕様の版の記述（「節の番号は ITU-T H.264 (08/2021) の物」）を file の先頭の説明から外し、定数の block の comment に置いた。guardrail の「file の上部に出典を書かない」に当たるかの判断（design-reviewer の指摘 20）。
- 適合の bitstream の cache（約 3.1 GB）の置き場所（既定は worktree の `build/tmp/ws326-conformance/zips`、`H264SW_CONFORMANCE_DIR` で変えられる）。
- 範囲の広げ（指摘 22）: Extended（constraint_set0/1 が 1）と High 10・4:2:2・4:4:4 Predictive の profile_idc でも中身が 8 bit 4:2:0 の frame なら受ける形にした。ユーザーの範囲（Constrained Baseline・Main・High）の解釈として良いか。
