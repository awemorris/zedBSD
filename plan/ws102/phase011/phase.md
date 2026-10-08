<!-- awesome-plan project=zedbsd record=ws102-p011 -->

# ws102-p011: L3 の数値目標への直し（遅れ・動き）

Parent: [WS102](../ws.md)
Status: planned（2026-10-08 夜 P1 q913: p010 の基準値を待つ）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q913（P1）
依存: [ws102-p010](../phase010/phase.md) の基準値（T1）。

## 範囲

p010 の `latency` の段の基準値で目標（design.md §3 の L3 (a)(b)）を外れた所を直す。直す所は基準値を見て決める。読みで分かっている候補:

- (b) 開く動き: QEMU の Venus は 7 枚/秒前後（p008: frame の間隔 130〜143 ms）しか描けないので、20 ms は QEMU では満たせない。滑り（200 ms、
  時間で決まる）は frame が粗くても終わりの位置は正しい。合否は 5330 の実機（i915）か Windows の QEMU で `slide end` の行を見る（p008 の
  Q1 の判断と同じ扱い）。
- (a) 送出: 離しから送出までは compositor の中の処理だけ（key の event の作成と送り）。p95 が 5 ms を越えたら、送出の後の client への flush
  の時期（event loop の回りの最後でまとめて flush するか）を見る。
- (a) app の frame: Text Editor の再描画の時間に依る。50 ms を越えたら、compositor の frame の callback の時期（client の描画の開始）と
  Text Editor（libkeiui）の描画を分けて測る。

## 確認の予定

`osk-guest.sh latency` を直しの前後で（T1）、`osk-latency.py --check` で (a) が met。(b) は実機の値を記録。
