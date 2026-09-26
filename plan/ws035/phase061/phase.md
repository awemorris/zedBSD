<!-- awesome-plan project=zedbsd record=ws035p061 -->

# ws035-p061: 壁紙の画像と、すりガラスで透ける窓（glass の試作の続き）

Phase ID: `ws035-p061`
Parent: [WS035](../ws.md)
Status: cleared（q462-i01、2026-09-26）
Phase disposition: normal
Queue: q462-i01
承認: 2026-09-26 ユーザー「この背景を使えそうですか？ちょっとぼかしをかけて、シャープネスを下げることで、抽象度の高いイラストにしてから使うのがいいと思います。また、ウィンドウをすりガラスシェーダーで90%透過くらいで描画してみてほしいです。」（白樺と山と湖の絵、署名「LEEKING 26」）

## 範囲

- 壁紙: ユーザーの絵を host で 16:10 に切り（中央）、1280x800 にし、median 7・Gaussian 3.5 px・彩度 0.85・contrast 0.85・明るさ 1.06 で抽象度を上げる（`build/ws035-wallpaper/`、git に入れない）。署名は消さない。
- zwl `--wallpaper=PATH`（binary PPM、P6、255）。出力と大きさが違えば最近傍で合わせる。読めなければ今の手続きの壁紙。すりガラスの縮小ぼかしはこの壁紙から作る。
- zwl `--window-opacity=N`（1〜100 %、既定 100）: glass の look で、窓の本体をすりガラスの面の上に N % の不透明度で描く（ユーザーの「90% 透過」は N=10）。
- 画像は image へ試験の追加 file（`/usr/share/zdesktop/wallpaper.ppm`）で入れる。

範囲外: 画像の format（PNG 等）の decoder、背後の窓のぼかし（p057）。

絵の権利: ユーザーが著作権を持つ（2026-09-26 ユーザー「このファイルは私が著作権を持ちます。慎重にする必要はないです。ただ、まだGitに入れなくていいです。」）。まだ git に入れない。

## 受け入れ

1. 絵の壁紙と、90% 透過（N=10）の窓（mview と wltest）の画面を撮ってユーザーに見せる。比べるため N=60 も撮る。
2. 既定（option 無し）の p059・p052 の試験が通る。build は warning 0、変えた C の style-check の指摘 0。

## 結果（q462-i01、2026-09-26、QEMU の Venus guest（host の Lavapipe）。実機は未実施）

- 壁紙: 中央を 1690x1056 で切り、1280x800 に縮め、median 7・Gaussian 3.5 px・彩度 0.85・contrast 0.85・明るさ 1.06（`build/ws035-wallpaper/soft-b.png`、PPM は `wallpaper.ppm`。3 段の比較は `preview.png`）。署名は切り取りの端で一部欠けたが消していない。
- zwl: `--wallpaper=PATH`（P6・255 の PPM、最近傍で出力に合わせる。読めなければ手続きの壁紙）、`--window-opacity=1..100`（glass の look で、窓の本体の下にすりガラスの面（白 30 %、縁の光 0.75）を敷き、画像をその不透明度で重ねる）。panel.frag の画像の mode が不透明度（押し込み定数の最後の float）を掛ける。
- 画面: `--window-opacity=10`（90 % 透過）で mview の model はうっすら、すりガラス越しに壁紙の色が見える（`build/ws035-p061/opacity-10.png`）。比較の 60 %（`opacity-60.png`）は model がはっきり見え、背景が透ける。
- 回帰: option 無しの p052 PASS、`--glass` の p059 PASS。build は warning 0、style-check は glass.c と変えた行で 0。

制限: 窓の下のすりガラスが透かすのは壁紙だけで、下の窓は透けない（背後の窓のぼかしは p057）。大きさの違う PPM の拡縮（最近傍）は試していない。

## 追記（2026-09-26）: 壁紙の差し替え

ユーザー「背景画像はこの添付をベースに差し替え、さらに強くぼかして抽象度を上げましょう。」（同じ白樺と山と湖の、ややぼけた版）。1672x941 の中央を 1506x941 で切り、1280x800 にし、3 段（median 7・9・11、Gaussian 6・10・16 px）を比べて中（median 9、Gaussian 10 px、彩度 0.85、contrast 0.85、明るさ 1.05）を採った（`build/ws035-wallpaper/v2-soft-b.png`、比較は `v2-preview.png`、`wallpaper.ppm` を置き換え。git 外のまま）。コードの変更は無い。QEMU の Venus guest で窓と Wiseview の画面を撮った（`build/ws035-p061b/desktop.png`・`wiseview.png`）。
