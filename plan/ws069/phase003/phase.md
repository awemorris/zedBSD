<!-- awesome-plan project=zedbsd record=ws069p003 -->

# ws069-p003: Xzed の rootless（X の top-level ごとの Wayland の窓）

Phase ID: `ws069-p003`
Parent: [WS069](../ws.md)
Status: cleared（q474-i01、2026-09-26）
Phase disposition: normal
Queue: q474-i01
承認: 2026-09-26 ユーザーの自律実行の指示
設計: [design.md](../design.md) §3

## 範囲

1. backend を接続と窓に分ける（`xzed_wayland_window_*`）。rootful は root の窓 1 つ、rootless は top-level ごとに 1 つ。
2. `--rootless`: root の子が map されると窓（題名は WM_NAME）、unmap・destroy で閉じる。top-level ごとに合成して present。
3. 入力: pointer の enter でその X の窓を上げ（hit の対象）、keyboard の enter で focus。座標は窓の X の screen 上の原点から。
4. zwl の configure の大きさ → X の窓の大きさの変更（ConfigureNotify・Expose）、close → その client を切る。

## 受け入れ

1. Venus の zdesktop で `Xzed --wayland --rootless` の X の app（zterm）の窓が Wiseman の窓（浮いたタイトルバーの題名が X の窓の名前）になり、
   打った command の出力が見える。
2. ドッキングで窓の大きさが変わり、zterm が新しい大きさで描く。× で zterm が終わる。
3. rootful（p002 の試験）が変わらず動く。build は warning 0、新しい C の style-check 0。

## 結果（q474-i01、2026-09-26）

実装:

- `xzed/wayland.c`・`wayland.h`（style-check 0）: 接続（`xzed_wayland_*`）と窓（`xzed_wayland_window_open/present/resize/move/title/close`）に分けた。
  callback: key・pointer（X の screen の座標 = 窓の原点 + 局所）・enter（pointer か keyboard が入った X の窓）・configure（zwl が与えた大きさ）・close。
- `main.c`: `--rootless`（`--wayland` を含む）。present で root の子（top-level）ごとに: map で窓を開き（題名は WM_NAME、無ければ X11）、
  unmap で閉じ、大きさ・題名・原点を合わせ、変わった矩形の部分を top-level とその子だけで合成（`rootless_compose`、他の top-level の重なりを
  含めない）して present。enter で X の窓を上げ（hit の対象）、keyboard の enter で focus。configure で X の窓の大きさを変え ConfigureNotify と
  Expose。close は event の後にその client を切る（`rootless_closing`）。destroy で窓を閉じる。起動した command を waitpid で回収（zombie を残さない）。
  rootful は root の窓 1 つ（p002 と同じ）。style-check の指摘は HEAD より 1 つ少ない。
- `libX11/xlib.c`: `XPending` で server が閉じた接続を見たら Xlib の既定の I/O error の扱いと同じく「X connection broken」で exit(1)（以前は
  EOF を無視して client が残った）。

確認（Venus、QEMU）: `plan/ws069/tests/x11-p003.sh` PASS（`build/ws069-p003/run4/`）: `Xzed --rootless --size 1000x620 -- /bin/zterm` の zterm が
Wiseman の窓（題名 zterm）、打った `echo ROOTLESS-OK; uname -a` の出力、ドッキングで X の窓が 1280x762 になり zterm が新しい大きさで描く
（`ls /`）、バーの × で zterm が終わり Xzed は残る。rootful（x11-p002）も動く。回帰 p069 PASS。build は warning 0。

未実施・残り: override-redirect の窓（menu・tooltip）を popup にしない（top-level として普通の窓になる）、X の窓の位置は Wiseman が決める
（X の client の move は画面に出ない）、WM_DELETE_WINDOW を送らず client を切る、cursor の形、scroll、i915 実機。
