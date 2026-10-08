<!-- awesome-plan project=zedbsd record=ws177-p036 -->

# ws177-p036: 出力の大きさの変更で整列し直す、メニューを先に閉じる、swap の取り消し

Parent: [WS177](../ws.md)
Status: test-wait（T1-475、2026-10-08 夜 Q1）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q908 / q908-i01
Origin: [backlog-p2](../backlog-p2.md) の WS181 ws181-p004 の行（整列中の回転・解像度の変更、メニューが開いている間の network・volume の icon の press、swap の途中の desktop の切り替え・lock）。[案 U](../phasing-20261008.md)。

## 範囲と決め事

- compositor には画面の回転の実装が無い（調べた範囲で displays・output-switch に rotation は無い）。出力の大きさが変わる道は `kwl_glass_output_resized`（解像度の変更・別の display への移動）だけ → その終わりで anchor の整列中の desktop を新しい作業の領域で同じ形に整列し直す（各窓は同じ番号の枠、glide）。新しい枠に入らない窓があればその desktop の整列を終える（`reason=output`）。回転が実装されたら同じ道を通る。
- 整列のメニューが開いている間に、同じ bar の他の widget（volume・network・Bluetooth など）を press したら、メニューを先に閉じてその press を widget へ流す（今までは 2 つの popup が重なった）。pill・メニューの中・bar の外は今どおり。
- swap（整列の窓の title の drag）の途中で表示の desktop が変わる・session が lock される・出力の大きさが変わったら、swap を取り消して窓を自分の枠へ戻す。後の release は swap でない。

## 実装（2026-10-08 P2）

- `arrange-shell.c`: `kwl_arrange_output_resized`（log `KWL ARRANGE output desktop=N layout=… windows=n area=…`）、`kwl_arrange_bar_press`（log `KWL ARRANGE menu close via=bar`）、`kwl_arrange_swap_cancel`（log `KWL ARRANGE swap-cancel surface=N slot=i reason=…`）。kwl.h に宣言。
- `shell.c`: `kwl_glass_output_resized` の終わり、`kwl_glass_button` の status の widget の前、`desktop_turn` の始めから呼ぶ。`greeter.c` `kwl_lock` から swap の取り消し。

## 確認

| 確認 | 結果 |
| --- | --- |
| build（p035 と同じ） | 成功、warning 0 |
| style-check | 新しい指摘 0 |
| QEMU（T1、u-guest.sh の U5: メニューを先に閉じる、desktop の切り替えで swap の取り消し） | 未実施 |
| 出力の大きさの変更（解像度・HDMI への移動）での整列し直し | 未実施（QEMU の 1 画面では起こせない。5330 の UAT: 整列中に HDMI へ出力を移す） |
| lock での swap の取り消し | 未実施（`--testing` の compositor は session manager が無く lock しない。5330 の UAT） |
