<!-- awesome-plan project=zedbsd record=ws035p063 -->

# ws035-p063: Wiseview（ウィンドウ一覧）の疎通確認

Phase ID: `ws035-p063`
Parent: [WS035](../ws.md)
Status: cleared（q464-i01、2026-09-26）
Phase disposition: normal
Queue: q464-i01
承認: 2026-09-26 ユーザーが Wiseman の操作体系と Wiseview の仕様・画像を示した（[wiseman-design.md](../wiseman-design.md)）。進め方はこれまでと同じく疎通確認（「まずは一本通す」、2026-09-26）。

## 範囲（`zwl --glass`、Wiseman Mode）

- 起動: 画面の下端 20 px 以内で押し、上へ drag（1:1 で追従する進み具合 = 上へ動いた距離 / 240 px）。離したとき 35 % を越えていれば開き、届かなければ閉じる（残りを 200 ms で）。
- 見た目（仕様の「実装の初めの目安」）: 背景は縮小ぼかしの壁紙と黒 8 %、今のデスクトップの窓を使った順（今の窓が先）にグリッド（1 枚 1 列、2〜4 枚 2 列、5〜9 枚 3 列、10 枚以上 4 列）、左右 56・上 40（説明帯の下）・下 40、ガター 24、縦横比を保って最大は幅 42 %・高さ 36 %、角丸 16。タイルの下にすりガラスの札（印と題名）。今の窓は 1.04 倍と青い glow、hover は青い glow と右上の閉じる。説明帯（「Wiseview · N windows」）、下に取っ手と「Swipe down to return to your window」。上部のバーは「Wiseview」を表示し、仮想デスクトップ列を強調。
- 遷移: 各窓が今の矩形からタイルへ進み具合で補間、背景のぼかしと札・説明帯を進み具合で fade in、タイトルバーは fade out。
- 操作: タイルの click で選択（最前面にして閉じる。ドッキング中の窓はドッキングに戻る）、閉じるで `xdg_toplevel.close`、背景の click で閉じる。開いている間、pointer の button は client へ送らない。
- 縮小の見た目のため、窓の画像に線形 sampling の descriptor set を足す。

範囲外（別 Phase）: 物理的に追従するドッキングの解除（p064 の候補）、左右の端のスワイプと仮想デスクトップの実体（p065 の候補）、タイルの drag でデスクトップ移動、キーボード、タッチパッド、縦スクロール、最小化の窓。

## 受け入れ

1. 下端からの drag で Wiseview が開き、窓がタイルに並ぶ（画面をユーザーに見せる）。途中の進み具合の frame も描かれる。短い drag では開かない。
2. タイルの click でその窓が最前面に戻り、背景の click で元に戻り、閉じるで client が閉じる。
3. p052・p059・p062 の試験が通る。build は warning 0、style-check の指摘 0、boot test。

## 結果（q464-i01、2026-09-26、QEMU の Venus guest（host の Lavapipe）。実機は未実施）

実装: `shell.c` に Wiseview（下端 20 px からの drag、進み具合 = 上へ動いた距離 / 240 px、離して 35 % を越えれば開き、届かなければ閉じる、残りを 200 ms で。グリッド、タイル、札、説明帯、下の取っ手と文言、上部のバーの「Wiseview」と仮想デスクトップ列の強調。タイルの click で選択、背景の click で閉じる、タイルの閉じる）。窓の画像に線形 sampling の descriptor set（`zwl_compose_linear_set`、import と host の画像の両方。descriptor pool を 512 に）。`zwl.h` に Wiseview の状態、`objects.c` で破棄された窓を外す。

試験: [zdesktop-p063.sh](../tests/zdesktop-p063.sh)（4 つの窓: wltest 2 つ、wl_shm、mview）。

| 受け入れ | 結果 |
| --- | --- |
| 1. 開く | 短い drag（26 px）は `cancel from=0.1x` で開かない。長い drag で `opening`、途中の frame（1 回の試験で 17 frame の log）、`open windows=4`。`open.png`: 2×2 のタイル（今の窓 mview は 1.04 倍と青い glow）、札、説明帯「Wiseview - 4 windows」、取っ手と「Swipe down to return to your window」、上部のバーは「Wiseview」、仮想デスクトップ列に青い縁。`peek.png`: 窓がタイルへ動く途中、背景がぼけ始め、タイトルバーが薄れる |
| 2. 操作 | wl_shm のタイルの hover（`hover.png`: glow と閉じる）→ click で `select`、閉じて wl_shm が最前面（画面で確認）。背景の click で `close`。wltest b のタイルの閉じるで `close-window`、b が終了 |
| 3. 回帰・規約 | p052・p053・p054・p059・p062・p063 PASS。build は warning 0、style-check は shell.c と変えた行で 0。boot test PASS（`build/ws035-p063-boot/login.png`） |

制限（後の Phase へ）: 下へのスワイプで閉じる（今は背景の click）、キーボード（Esc・Tab・矢印・Enter）、タイルの drag で仮想デスクトップへ、左右のスワイプで隣のデスクトップの Wiseview、縦スクロール、最小化の窓、2 段階の Peek の中間の止まり（今は連続）、MRU は map order（最前面にした順）で代用、背景は壁紙だけ（背後の窓はぼかさない）。
