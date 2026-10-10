<!-- awesome-plan project=zedbsd record=ws202-p016 -->

# ws202-p016: 参照の list の計算と欠けた参照の判定（D25）

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 3 LW
依存: p015、J8（方式。推しの (a) を仮に採った）

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
3. 捨てた picture が参照なら、p015 の「decode しなかった参照の picture」として DPB に入れる。
4. host 試験（`run-host-h264.sh` に足す、design §10.2 の (3)・(4)）:
   - 手で作った DPB と slice header の列で、初期の順（P・B、短期・長期、L1 の入れ替え）・modification（idc 0・1・2）・`num_ref_idx_active` の切り詰めを、8.2.4 の式から手で
     計算した期待と比べる。
   - 欠けた参照が active の範囲の外（decode）・中（捨てる）・modification が指す（捨てる）の 3 通り。
   - `h264-gap.mp4`: D25 の判定の列（picture ごとの decode・捨てる）を記録し、次の I の後は全部 decode、捨てる picture の後で欠けた参照が sliding window から消えると
     また decode される。どれかの picture が「欠けた参照があるが範囲の外で decode」を通ったかを記録する（U17。通らなければ手の列だけが確かめる、と書く）。
   - WS083 と同じ素材の 6 本と `h264-high-b-aac.mp4` で、欠けた参照が無い時に計算を省いても、省かずに計算した (1) と (2) が全 slice で一致する（計算の自己の確かめ）。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/run-host-h264.sh` | p008・p015・p016 の全項目 PASS（ASan/UBSan） |
| design §10.6 の build（`ZEDBSD_CONFIG=config/ci/config-amd64.mk`、libmedia・libbrowser・videoplayer・music） | exit 0、`grep -c 'warning:'` が 0 |

## 注意

- MFD が規格と同じ手順で list を作ることは前提（U16）。WS083 の実機の hash（B・長期・modification の stream）の一致から推し、p012 の 5330 の `h264-gap.mp4` の hash
  （出た frame が元の stream と一致）で確かめる。
- J8 の答えが (b)・(c) ならこの Phase は取り消す（canceled、理由を書く）。(b) で i915 の変更が要ると分かったら WS083 の範囲なので Q1 に戻す。
