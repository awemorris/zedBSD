<!-- awesome-plan project=zedbsd record=ws202-p016 -->

# ws202-p016: 参照の list の計算と欠けた参照の判定（D25）

Status: uncleared（規格ref-list実装/host証拠あり、missing-packet実画素等は未確認）
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 4 LW
依存: p015、J8（方式。推しの (a) を仮に採った）。p009 と並べて進めてよい（L3-06）

## 目的

欠けた参照（p015）を Vulkan に渡さずに decode してよい picture を決める（design §5.3.1 D25、review-002 H2-01）。i915 は picture を持たない slot の参照で submit 全体を
DEVICE_LOST にし（`render/video.c` 2621〜2631）、MFD は渡した DPB から hardware で list を作る（`video-mfx.c` 412〜417）ので、欠けた参照を除いた DPB から作る list が
規格の list と同じ時だけ decode する。

## 成果（`userland/desktop/libmedia/h264-refs.c`、新）

1. 8.2.4 の参照の list の構成:
   - PicNum・LongTermPicNum（8.2.4.1、frame だけ）。
   - 初期の順（8.2.4.2）: P・SP は短期を PicNum の降順、続いて長期を LongTermPicNum の昇順。B は L0 = 短期の POC が今より小さい物の降順・大きい物の昇順・長期、L1 = その逆の順・
     長期。L1 が L0 と同じで 2 枚以上なら L1 の先頭 2 枚を入れ替える。`num_ref_idx_lX_active_minus1 + 1` より長ければ切る。
   - modification（8.2.4.3）: `modification_of_pic_nums_idc` 0・1（短期、picNumPred）、2（長期）、3（終わり）。
2. D25 の判定 `h264_refs_check(dpb, picture, &verdict)`: 各 slice・各 list で、(1) 欠けた参照を含む DPB の list（規格）と (2) 欠けた参照を除いた DPB の list（hardware）を
   計算し、先頭 `num_ref_idx_active` 個が全部一致し (1) に欠けた参照が無い時だけ「decode」。modification が欠けた参照の PicNum を指す slice は「捨てる」。DPB に欠けた参照が
   無い時は計算を省いて「decode」。
   - **POC の不明な non-existing の frame（type 0）が DPB にある時の B の slice**（H3-01、design §5.3.1）: 規格の list が作れないので、各 list（L0・L1）の先頭
     `num_ref_idx_active` 個が modification の命令で全部決まり（N 個の命令がそれぞれ slot を持つ別々の picture を指す）時だけ「decode」、他は「捨てる」。P・SP は今のまま。
   - 規格の 8.2.4.2.3（B の L1 の入れ替えが切り詰めの前か後か）を読んで照らす（U20）。
3. 捨てた picture が参照なら、p015 の「decode しなかった参照の picture」として DPB に入れる。
4. host 試験（`run-host-h264.sh` に足す、design §10.2 の (3)・(4)）:
   - 手で作った DPB と slice header の列で、初期の順（P・B、短期・長期、L1 の入れ替え）・modification（idc 0・1・2）・`num_ref_idx_active` の切り詰めを、8.2.4 の式から手で
     計算した期待と比べる。
   - 欠けた参照が active の範囲の外（decode）・中（捨てる）・modification が指す（捨てる）の 3 通り。
   - POC の不明な non-existing がある B の保守的な判定: 手の列で、modification で list が決まる B は decode、決まらない B は捨てる。
   - **`h264-gap.mp4` の正解との比べ**（M3-03）: D25 が「decode」とした各 picture について、gap の stream の hardware の list（欠けを除いた DPB）の先頭 N 個が、
     **`h264-gap-orig.mp4` の同じ picture（pts で合わせる）の規格の list の先頭 N 個**と同じ picture（POC で同定）を指す。全部の「decode」が一致、後の非 IDR の I の後は全部
     decode。どれかの picture が「欠けた参照があるが範囲の外で decode」を通ったか、B が何枚捨てられたかを記録する（U17・J8 の材料）。
   - （第 3 版の「欠けた参照が無い時に (1) と (2) が一致」の項は、同じ DPB から同じ手順で計算するので何も確かめない。上の比べに置き換えた、L3-02。）

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/run-host-h264.sh` | p008・p015・p016 の全項目 PASS（ASan/UBSan） |
| design §10.6 の build（`ZEDBSD_CONFIG=config/ci/config-amd64.mk`、libmedia・libbrowser・videoplayer・music） | exit 0、`grep -c 'warning:'` が 0 |

## 注意

- MFD が規格と同じ手順で list を作ることは前提（U16）。WS083 の実機の hash（B・長期・modification の stream）の一致から推し、p012 の 5330 の `h264-gap.mp4` の hash
  （出た frame が元の stream と一致）で確かめる。
- J8 の答えが (b)・(c) ならこの Phase は取り消す（canceled、理由を書く）。(b) で i915 の変更が要ると分かったら WS083 の範囲なので Q1 に戻す。


## 規格照合・software実装・main統合（2026-10-10）

Event: `ws202-main-integration-20261010-p016`。mainのcanonical ID/参照listの目的は維持。独立branchの同ID（標準readback）は別の[p017](../phase017/phase.md)へmapping修復し、両方の履歴を保存した。

ユーザーの自走承認i08で、全sliceの初期list/modification/active-prefixをlogical DPBと実GPU DPBで比較する処理を`libmedia/h264-dpb.c`に実装。POC type0のgapで推定したnon-existing frameはITU-T H.264 8.2.4.2.3に従いBの初期listから除外する。第4版の「不明なPOCがあるBは一律保守的に捨てる」という手順をこの規格上の扱いへ訂正し、modificationが存在しない参照を要求するpictureはdropする。scope/規格照合の詳細は[H.264記録](../h264-progress-20261010.md)、実装とactual host/限界は[software結果](../playback-result-20261010.md)。p008/p015/p009とWS/designにもこの改訂を記録した。

hand vectorと独立4-slice/B bitstreamはPASS、実行部分はhost GPU stand-in。`h264-gap-orig.mp4`とのmissing-packet実decode画素比較、ITU conformance全matrixは未実施。whole uncleared。再開はT1のi915画素結果と未実施matrix。標準readbackだけの成功をref-listのclearanceとしない。
