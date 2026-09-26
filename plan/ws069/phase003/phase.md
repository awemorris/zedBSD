<!-- awesome-plan project=zedbsd record=ws069p003 -->

# ws069-p003: Xzed の rootless（X の top-level ごとの Wayland の窓）

Phase ID: `ws069-p003`
Parent: [WS069](../ws.md)
Status: in-progress（q474-i01）
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
