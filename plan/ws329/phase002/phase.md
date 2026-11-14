<!-- awesome-plan project=zedbsd record=ws329-p002 -->

# ws329-p002 共通化の refactor: CAVLC の表・CABAC の context の選び・context の初期化・表 A-1 を decoder と encoder の共通に

- 親: [WS329](../ws.md)、設計 [design.md](../design.md)。
- 状態: 実装済み・試験 PASS（2026-11-14 P5 q2092-i01。cleared は Q1）。2026-10-31 p001 で作成。
- 依存: WS326 completed（p026 の名前の移しを含む。header の凍結 BASE=fd1d6f88c が外れた後）。2026-10-31 に completed、p026 は 92ca2d0e8、WS326 の header の基点は 77ed48ac4。
- 担当の想定: Opus 5.5（phase-runner）。decoder の hot path に触れるので Sonnet に回さない。

## 目的と範囲

design §4・§5.3 の R1〜R5 を行う。decoder の振る舞い（出力の画素・誤りの値・試験の結果）を変えない。

- 置き場（2026-11-14 の今の build に合わせた変更）: 新しい `.c` は libmedia の `sources.mk`（make の file、Claude は変えない）の変更を要するので作らず、下の R1〜R4 の公開の関数と表は既存の module の `.c`・`.h` に置く（R1 は h264sw-cavlc、R2 は h264sw-cabac-se、R3 は h264sw-cabac、R4 は h264sw-ps）。名前と仕様は proposed/shared/ のとおり（cbp の表だけ `h264sw_cavlc_cbp_values`、既存の関数 `h264sw_cavlc_cbp` とぶつかるため）。
- R1: `h264sw-cavlc.c` の表を新しい `h264sw-cavlc-tables.[ch]` へ（[proposed/shared/h264sw-cavlc-tables.h](../proposed/shared/h264sw-cavlc-tables.h)）。値・順は変えず、block の `Values referenced:` を保つ。cavlc.c は新しい名前を使う。
- R2: `h264sw-cabac-se.c` の context の導出を `h264sw-cabac-ctx.[ch]` の公開の関数へ（[proposed/shared/h264sw-cabac-ctx.h](../proposed/shared/h264sw-cabac-ctx.h)）。cabac-se.c はそれを呼ぶ。
- R3: `h264sw_cabac_start` の context の部分を `h264sw_cabac_contexts_init` に分け、表 9-44・9-45 の宣言を header に置く（WS326 p026 が算術の engine を汎用の名前に移していれば、その header に）。表の宣言は p026 が `vcodec-cabac.h` に済ませていた（`vcodec_cabac_range_lps` など）。
- R4: `h264sw-ps.c` の `ps_level_limits` を `h264sw-levels.[ch]`（全列、[proposed/shared/h264sw-levels.h](../proposed/shared/h264sw-levels.h)）へ。表 A-1 の全 20 行を PDF の頁の画で照合する（2 つの独立の読み）。
- R5: 共通に使う header の先頭の文の "decoder" を "codec" に（意味を変えない）。
- proposed の名前は WS326 p026 の結果に合わせて直してよい（直した点を結果に書く）。

## 受け入れ

- 変える前と後で `sh plan/tools/h264sw/run-unit.sh all` の各 test の最後の行（件数を含む）が同じ。WS326 p022 の適合の試験（`in` の全 stream）が前後で同じ結果。
- 新しい単体試験 `plan/ws329/tests/unit/test-cabac_ctx.c`（各関数を手の例で: 隣の有無・skip・intra・I_PCM・direct・cat ごとの offset）と `test-levels.c`（表 A-1 の全行、1b の見つけ方）。この 2 本は p003 の run-unit.sh で流す（p002 では WS326 の run-unit.sh の形を一時の command で）。
- WS326 の `check-skeleton.sh`（BASE 無し）PASS、x86_64・arm64・riscv64 の libmedia の build warning 0。
- WS326 の header の基点が移ることを結果に書く（Q1 が WS326 の記録に反映）。

## 変えてはいけない物

- decoder の各 module の振る舞い。新しい public の関数の他に decoder の公開の関数の形を変えない。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない（消す物は path と理由を Q1 へ）。

## 確かめ

- `sh plan/tools/h264sw/run-unit.sh all` を変更の前後で流し、出力を比べる。
- 適合の試験（WS326 p022 の command）を前後で。
- zedBSD の libmedia（x86_64・arm64・riscv64、自分の worktree の build/ で。初回は `make CONFIGMK=config/config.<機種>.mk llvm-toolchain-cached` と `sysroot`）: `timeout 2400 make CONFIGMK=config/config.<機種>.mk -j4 build/<機種>/packages/libmedia/.stamp`（<機種> は x86_64・rpi5・vf2）→ 各 exit 0、warning 0
- `git diff --check`。commit は `git commit -m WIP -- <path>`、push しない。結果の節（host の `date`、command と結果、未実施）を足し、ws.md の行を更新（cleared は Q1）。
