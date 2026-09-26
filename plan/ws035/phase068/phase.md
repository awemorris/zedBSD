<!-- awesome-plan project=zedbsd record=ws035p068 -->

# ws035-p068: zdesktop-terminal（zterm を Wayland と Vulkan へ）

Phase ID: `ws035-p068`
Parent: [WS035](../ws.md)
Status: cleared（q468-i01、2026-09-26）
Phase disposition: normal
Queue: q468-i01
承認: 2026-09-26 ユーザー「userland/base/ztermに、X11のターミナルの実装があります。これをWayland+Vulkanに移植して、userland/base/zdesktop-terminalとして実装をお願いします。」
（zterm の実際の場所は `userland/X11/zterm`）

## 範囲

1. `userland/X11/zterm`（Unicode VT100、Xlib）の端末の部分（pty・VT100 の解釈・画面の格子）を保ち、表示を Wayland の窓
   （xdg-shell、Wiseman の浮いたタイトルバーの下）と Vulkan の描画（glyph の atlas を texture にして格子を描く）へ移す。
   文字は libtruetype で等幅の TTF（JetBrains Mono、OFL。Inter と同じく git に入れず、image へは試験の追加 file）を glyph の atlas に描く。
   `/dev/graphics` の font（zterm・Xzed が使うもの）はレガシー用なので使わない（2026-09-26 ユーザー「/dev/graphicsのフォントは使わないでください。それはレガシー用です。」
   「libtruetypeを使ってください。」）。
2. 入力は wl_keyboard（keymap は zwl が送るもの）と wl_pointer（範囲外なら最小限）。
3. `userland/base/zdesktop-terminal` として package にし、zdesktop の image に入れる。

## 受け入れ

1. Venus の guest の zdesktop で窓が開き、shell の prompt が出て、打った command（`ls` 等）の出力が見える（VNC の画面）。
2. i915 実機（capture）でも窓が描かれる（可能なら）。
3. build は warning 0、新しい C は coding-style の全文を適用し style-check 0。

## 結果（q468-i01、2026-09-26）

実装（`userland/base/zdesktop-terminal/`、新しい C は coding-style の全文、style-check 0）:

- `screen.c`: zterm の VT100 の解釈を移し、scroll 領域（DECSTBM）、行・文字の挿入と削除、ECH、CHA・VPA・CNL・CPL、
  DECTCEM、256 色と direct color、OSC の読み捨て、G0/G1 の指定の読み捨て、alternate screen（消去だけ）を足した。
- `keys.c`: zwl は evdev の code を keymap 無しで送るので、US 配列の表（Shift、Ctrl、Alt は ESC 前置）と xterm の sequence。
- `font.c`: libtruetype で等幅の TTF（既定 `/usr/share/fonts/zdesktop-mono.ttf`、試験では JetBrains Mono、OFL、git 外）を
  cell の大きさの slot の atlas に初出時に描く（`/dev/graphics` の font は使わない）。
- `render.c`: Vulkan。各 cell を三角形 2 つ（vertex に位置・atlas の位置・前景と背景の色）、atlas は host が書く linear の画像、
  frame ごとに全 cell の vertex を作って 1 回の draw。cursor は色を入れ替えた block。shader は i915 の native compiler が通る形
  （gl_VertexIndex・switch・早い return 無し。host の `i915-vk-eudump` で vertex 41 IR / 1104 byte、fragment 29 IR / 1040 byte）。
- `window.c`: xdg-shell の toplevel（題名 Terminal）、wl_keyboard、key repeat（zwl は repeat しないので client で、400 ms・40 ms）。
- `main.c`: forkpty で `/bin/sh`（`--command=` で `sh -c`、`TERM=xterm`）、Wayland と pty を 1 つの poll で待つ、
  configure の大きさで swapchain と grid を作り直し TIOCSWINSZ、shell の終了か窓の close で終わる（`ZTERM DONE reason=`）。
- package: `userland/base/zdesktop-terminal/Makefile`、`platform/amd64/vmunix.mk` の link、zdesktop の 2 つの config に追加。
- 試験の道具: `plan/ws035/tests/build-zdesktop-image.sh`（Venus の image に git 外の font と壁紙を入れる。以前の build はこれらを
  入れ忘れていた）、`qmp-keys.py`（QMP で文字を打つ）、`zdesktop-p068.sh`、実機の scenario の `ZDESKTOP_APP=terminal`
  （`plan/ws031/tests/zdesktop/run-terminal.sh`、log を 2 秒ごとに sync）。

確認:

1. Venus（QEMU）: `zdesktop-p068.sh` PASS（`build/ws035-p068/run4/`、`venus-p068/`）: prompt、`echo ZTERM-OK; ls /` の出力、
   ドッキングで全画面の grid（`ls -l /bin | head -40` が画面を埋めて scroll）、`exit` で `reason=shell-exited`。
2. i915 実機（5330、VFIO、capture）: `hw2` で 6 検査 PASS（`build/ws035-p068/hw2/`: wl_shm の窓 2 つの上に terminal、
   `uname -a` と `ls /` の出力と prompt、ドッキングで 190x46、× で `ZTERM DONE reason=closed`）。1 回目の `hw1` は画面が最初から
   変わらず FAIL、log が失われて原因不明（[BUG-056](../../bugs/BUG-056.md)、tracking）。
3. build は warning 0、style-check 0。Venus の回帰 p052・p059・p062 PASS。boot test PASS（`build/ws035-p068-boot/login.png`）。
4. 実機の LCD・ベアメタル・実機の keyboard での入力: 未実施（入力は QEMU の QMP の key だけ）。

制限: JIS 配列は無い（US だけ）。幅広文字は glyph を 1 cell 幅で描く。scrollback・選択・copy と paste・mouse は無い。
bold は色を変えない。
