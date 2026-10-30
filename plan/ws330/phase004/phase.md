<!-- awesome-plan project=zedbsd record=ws330-p004 -->

# ws330-p004 h265sw-nal・h265sw-sei（NAL header、AU の始まり、decoded picture hash）

- 親: [ws.md](../ws.md)、設計 [design.md](../design.md)。型: S（Sonnet 5.5 high、phase-runner-low、標準廉価実装手順、1 回の起動）。
- 状態: planned（2026-10-31 p001 が作成）。依存: p003。BASE: p003 の commit（p003 が書く）。

## 1. 読む物

- 共通: [design.md](../design.md) §13.1 の「読む物（共通）」（AGENTS.md §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文、design.md §1・§2・§3・§7・§8・§10.1、`vcodec.h`・`h265sw.h`）。
- 仕様: ITU-T H.265 (V11) (01/2026)、`/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h265-202601.pdf`（SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`、git の外）。**PDF の頁 = 印刷の頁 + 18**。`pdftotext -layout -f N -l M` で読み、表と式（上付きが崩れる: 2^15 が「215」になる）は Read の tool で PDF の頁の画を見て確かめる。
- header（仕様の正本）: `userland/desktop/libmedia/h265sw-nal.h`、`userland/desktop/libmedia/h265sw-sei.h`、`userland/desktop/libmedia/vcodec-nal.h`、`userland/desktop/libmedia/vcodec-frame.h`（p003 の前は [proposed/](../proposed/)）。
- 仕様の節: 7.3.1・7.4.2（印刷 34・64〜72 = PDF 52・82〜90）、Table 7-1、7.4.2.4.4（印刷 70 = PDF 88）、7.3.5（SEI の payloadType・payloadSize）、D.2.20（印刷 303 = PDF 321）、D.3.20（印刷 359 = PDF 377、D-22〜D-25、Table D.10）、IETF RFC 1321（MD5、RFC の本文から。test vector は RFC の付録 A.5）。

## 2. 作る物

- `userland/desktop/libmedia/h265sw-nal.c`、`userland/desktop/libmedia/h265sw-sei.c`（header の関数を全部実装する）。
- 単体試験: `plan/ws330/tests/unit/test-h265_nal.c`、`plan/ws330/tests/unit/test-h265_sei.c`。
- この phase.md の「結果」、ws.md の表のこの行。

## 3. 試験の項目

- header の各 field（type・layer_id・temporal_id）を全値の組で（2 byte の header を手で組む）、forbidden_zero_bit・nuh_temporal_id_plus1 0・IRAP の TemporalId 0 以外・TSA の TemporalId 0 で EINVAL、size 0 と 1。
- emulation prevention: vcodec_nal_unescape を通ること（末尾の 0x000003、禁止の 3 byte）、capacity 不足で ENOBUFS。
- 分類の関数: 0..63 の全型で Table 7-1 の表と一致（表を試験にもう一度写すのでなく、各型の分類の理由を comment に書いた期待の配列）。
- AU の始まり: 35〜44・48〜55 と、そうでない 36・37・38・40・45〜47・56〜63。
- SEI: payloadType・payloadSize の 0xFF の延長（255 を超える大きさ）、複数の message、hash の 3 種、予約の hash_type は無視、大きさの不足で EINVAL。
- MD5: RFC 1321 A.5 の 7 つの例、64 byte の境の前後。CRC と checksum: 小さな画（2x2・16x8）の手計算（D-24・D-25）、幅 256 を超える x の xorMask の上位。
- h265sw_sei_check_hash: 一致・1 sample の違い・present 0。
- 乱数の byte 列で落ちない（ASan・UBSan）。
- 共通: 誤りの入力・途中で切れた入力・乱数の入力で落ちず（ASan・UBSan 0）決められた誤りを返す。

## 4. 注意

- MD5 は RFC の本文の手順から書く（他の library の code を写さない）。算術は uint32_t で、shift は 32 未満。
- hash は crop の前の decode の大きさ全体（D-22）。frame の width・height は pic_width_in_luma_samples と同じ。
- 算術は vcodec.h の規則（負の値の shift は vcodec_asr32・vcodec_shl32、積は広い型、狭い型へは clip）。

## 5. 変えてはいけない物

- design.md §13.1 の「変えてはいけない物」: 全部の `h265sw*.h`・`vcodec-*.h`・`h264sw*.h`・`h264enc*.h`、担当の外の `.c`、Makefile、`plan/ws330/tests/run-unit.sh`・`check-skeleton.sh`・共通の試験の補助（自分の test を除く）、HAL・toolchain・共有の build/。
- header の不備・仕様との食い違いを見つけたら header を直さず止めて Q1 に返す（どの header のどの関数で何が足りないか）。header が黙っている所は自分で決め、結果の節に列挙する。phase.md が仕様と食い違えば仕様に従い報告する。

## 6. 確かめの command

```
timeout 900 sh plan/ws330/tests/run-unit.sh h265_nal h265_sei
timeout 1800 sh plan/ws330/tests/run-unit.sh all
BASE=<p003 の SHA> timeout 300 sh plan/ws330/tests/check-skeleton.sh
（H.264 の回帰: p003 が plan/tools.md に登録した WS326・WS329 の単体試験の道具の all。WS326・WS329 の完了で plan/ws326・ws329 の tests は plan/tools へ移るので、p003 の結果の節の path を使う）
（amd64 の libmedia.so の build: plan/ws326/design.md §6 の command、warning 0）
git diff --check
```

## 7. 終わりの条件

- 上の command が全部 PASS（出力の最後の行を自分で読む）。
- わざと壊す確かめ（git の外で実装を数通り壊し、試験が FAIL することを確かめて戻す、`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 表の値は仕様から取り、独立した 2 つの方法で照合（`pdftotext -layout` と頁の画など）。試験の期待の値は仕様の表・式と手の計算だけから作る（他の実装の出力を使わない）。
- 他の実装（HM・FFmpeg・x265・libde265 など）の code・comment・構造・名前を写さない。source の comment に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。表の block に「Values referenced: ITU-T H.265 (01/2026) Table …」。
- 規約の checklist（plan/coding-style.md §14）で自分の diff を見直す。
- 新しい file は `git add` の後 `git commit -m WIP -- <path>...`。push しない。rm をしない。
- この phase.md の「結果」の節（日付は host の `date`、command と件数、自分で決めた点、未実施）と ws.md の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 結果

（実装の担当が書く）
