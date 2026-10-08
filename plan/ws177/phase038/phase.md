<!-- awesome-plan project=zedbsd record=ws177-p038 -->

# ws177-p038: Home が開く・閉じる途中の bar の fade、Home の上の status の隙間の press、白の文字

Parent: [WS177](../ws.md)
Status: test-wait（T1-475、2026-10-08 夜 Q1）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q908 / q908-i01
Origin: [backlog-p2](../backlog-p2.md) の WS181 ws181-p005 の行。[案 U](../phasing-20261008.md)。

## 範囲と決め事

1. Home が開く・閉じる途中（progress が 0 と 1 の間）: 今は home > 0 で即座に bar が消えて白の status だけになった → bar は desktop の層と同じ割合（`kwl_home_layer` の opacity、progress 0.2〜0.9 で 1→0）で薄れ、白の status は残りの割合で現れる。
2. Home の上で status の pill の中の icon の無い所（隙間・端の余白）や時計の press: 今は bar_press が何もしなかった → Home の press として Home に渡す（log `KWL HOME bar gap x=N`）。時計は ws181-p009 の決定どおり何も開かない。
3. 白の文字が明るい壁紙の Home で読めるか: Home の地は壁紙の blur の上の黒の glass（`HOME_STAGE_DARK` 0.82）。白い壁紙でも地の明るさは約 0.18 で、白（不透明度 0.96）との差は大きい（計算の上で読める）→ code は変えず、白い壁紙での QEMU の撮影で確かめる。

## 実装（2026-10-08 P2）

- `shell.c` `draw_system_bar`: Home の progress から層の opacity を求め、白の status をその残りで描き（`draw_home_status` に opacity）、bar は位置と大きさを変えない layer（opacity だけ）を通して描く。layer の値は描いた後に戻す。
- `bar_press(server, button)`: Home の上では `kwl_home_button` に渡す。
- 試験のための log: system bar の status の pill と時計の位置 `KWL GLASS status left=… width=… clock=…`（変わった時だけ）。

## 確認

| 確認 | 結果 |
| --- | --- |
| build（p035 と同じ） | 成功、warning 0 |
| style-check（shell.c） | 新しい指摘 0（既存 5 件） |
| QEMU（T1、u-guest.sh の U2: 隙間・時計の press、白い壁紙での u2-home-white.png、半分の u2-bar-fade.png） | 未実施 |
| 実機（5330、明るい壁紙、指で Home を開く途中の見え方） | 未実施（UAT） |
