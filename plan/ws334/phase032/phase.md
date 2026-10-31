<!-- awesome-plan project=zedbsd record=ws334-p032 -->

# ws334-p032 試験: Wine＋llvmpipe での確かめの第 1 回

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §7.1・§7.3（最小の合格の組 M1〜M8）、決定 20（host の Wine と lavapipe で開発する。実機の Windows はだいぶ後）。
- 状態: planned（2026-11-01 p003 で作成）。
- 依存: p031。
- 担当の想定: Opus（phase-runner）を 1 回（約 8 LW）。直しは見つけた不具合ごとに小さく、元の Phase の範囲（その file）の中で。範囲の外の直しが要れば Q1 に。

## 中身

- `plan/ws334/tests/wine-run.sh`（p028 から育てた物）を 1 本の受け入れの script にする: zip を展開 → xvfb-run の中で Wine（prefix `build/tmp/ws334-wine-gui`、lavapipe）→ 各項の操作（xdotool）と PNG・log の回収 → 要約（項ごとに PASS・FAIL・未実施、最後の行 `wine-run: PASS` か `FAIL`）。
- 項（design §7.3 の M1〜M8 を Wine でできる形に。できない物は「未実施（実機）」）:
  - M1 `OmniGrammaton.exe` が同梱の `start.html` を Vulkan（lavapipe）で描く PNG。
  - M2 `--no-gpu` と、`WINEDLLOVERRIDES=vulkan-1=d` で CPU の道に落ちて描く PNG。
  - M3 https: host の `openssl s_server` の試験の CA の頁（p030 の方法）。外の site は使わない。Disallowed の拒み。
  - M4 location の欄に英字を打って移る（日本語の IME は未実施（実機））。
  - M5 page の text の欄に英字を打つ（IME の composition は未実施）。
  - M6 再起動の後に期限付きの cookie と localStorage が残る（試験の頁が値を表示し PNG で読む。文字を読むのは Q1 かユーザー。script は頁の title を `--dump` の道か log で確かめる）。
  - M7 snap の配置・暗い mode の題の帯: Wine は snap の配置を出さない（未実施（実機））。暗い mode は Wine の registry の値で PNG。
  - M8 `omnigrammaton-cli.exe --js`・`--dump=layout` が Linux の host と同じ bytes（数本の頁と js）。setjmp と GC（R4）の js。
  - 2 つ目の起動が URL を 1 つ目に渡す。窓の大きさの変更（xdotool）で描き直る。
- PNG は `build/review/ws334-p032/` に写し、一覧を Q1 に（ユーザーに見せるのは Q1）。

## 受け入れ

- `wine-run.sh` の最後の行が `wine-run: PASS`（Wine でできる項が全部 PASS、できない項は「未実施（実機）」として数えない）。Wine の結果は実機の Windows の判定ではない（決定 20）と結果に書く。
- 実機の Windows（またはユーザーの決定 19 の QEMU の Windows の guest）での M1〜M8 の確かめは、ユーザーが拾える試験の一覧に置く（Q1 が）。

## 終わり

- 直した file ごとの commit、SHA・PNG・項ごとの結果を Q1 に、結果の節と ws.md の行。
