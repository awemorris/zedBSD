<!-- awesome-plan project=zedbsd record=ws177-p037 -->

# ws177-p037: 整列の記憶と、鍵盤だけの入れ替え（Super+矢印）

Parent: [WS177](../ws.md)
Status: in-progress（2026-10-08 P2 q908 実装・build。QEMU は T1）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q908 / q908-i01
Origin: [backlog-p2](../backlog-p2.md) の WS181 ws181-p004 の行（整列の再起動の後の記憶、鍵盤だけの入れ替え）。[案 U](../phasing-20261008.md)。

## 決定（2026-10-08）

- 記憶の意味: Q1 経由のユーザーの決定（クリック）**(b) 前の形と窓の位置を覚える** — 同じ desktop で整列を開き直して同じ形を選ぶと前と同じ窓を同じ枠に戻す。compositor の中だけで、再起動で消える。WS181 の設計 §5.3 の「整列の記録は残さない」を変える（[design.md](../../ws181/phase001/design.md) §5.3 に追記）。
- 鍵盤だけの入れ替え: Super+矢印（Super だけを押している時、整列中の desktop の前の窓だけ）。Super+Shift+矢印は display の移動で使用済み、Super+矢印は未使用だった。Q1 了解（2026-10-08）。

## 実装（2026-10-08 P2）

- `arrange-shell.c`: desktop・出力ごとの `arrange_memories`（形・枠ごとの窓）を `arrange_end` で記録、`kwl_arrange_forget` で消えた窓を NULL に。`arrange_recall`: 整列中で同じ形ならその枠、無ければ記憶。同じ形・同じ数・同じ窓の時だけ前の枠へ（log `KWL ARRANGE recall desktop=N layout=… windows=n`）。大きすぎる窓を外して作り直した後も試す。
- `kwl_arrange_key_swap`: 前の窓の枠から矢印の向きの枠（向きの先にある枠のうち、向きの距離＋横の距離×2 が最小）と入れ替え、両方 glide（log `KWL ARRANGE key-swap a=… b=… slots=i,j key=…`、向きに枠が無ければ `key-swap none`）。押した key の release も取る。整列中でなければ key は client へ。
- `shell.c` `kwl_glass_key`: Alt+Shift+矢印の後に Super だけの修飾で呼ぶ。

## 確認

| 確認 | 結果 |
| --- | --- |
| build（p035 と同じ） | 成功、warning 0 |
| style-check | 新しい指摘 0 |
| QEMU（T1、u-guest.sh の U6） | 未実施 |
