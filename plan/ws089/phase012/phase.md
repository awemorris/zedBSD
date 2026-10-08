<!-- awesome-plan project=zedbsd record=ws089-p012 -->

# ws089-p012: Settings の中だけで済む操作性（検索の key・touch の scroll）

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-238 PASS）（旧: test-wait（T1 依頼中。2026-10-06 q805 P2: 残りの「titlebar の欄から Down で結果へ」を compositor で直した（末尾）。以前: uncleared（q619-i01、P1 generation4、2026-10-03。実装と試験は PASS。範囲 1 の欄の Down だけが compositor の変更待ち））
Disposition: normal
Parent: [WS089](../ws.md)
Queue: q619 / q619-i01（Q1 の dispatch、2026-10-03。承認: user「Settingsも重点的にしましょう」と自走の指示。時限 4 時間）
依存: なし（p010 と並列可。p010 は source を変えない）
目安: 2h（1 Queue）。実行者の目安: phase-runner-mid
所有 path: `userland/desktop/settings/search.c`・`ui.c`・`pages.c`・`page-*.c` の入力の所、`network.c`（C1）、`plan/ws089/tests/`

## 範囲

1. 検索の結果の頁: 上下の key で選び、Enter で開く。titlebar の欄から Down で結果へ。選んだ行の見た目（Files の選択と同じ青）。
2. 頁の pane の touch: 1 本指の drag で scroll（慣性は libkeiland の `keiland_scroller` か Settings の今の scroll の model に合わせる。libkeiui への移行はしない）。
3. 左の項目の pane の Up・Down・Home・End の key の移動を見直し、頁の切り替えと履歴（戻る・進む）が崩れないこと。
4. **C1**（q619 で追加、[beta1-candidates.md](../beta1-candidates.md) の D1）: Wi-Fi の操作の待ちの slot。scan などの答えを待つ間に押した on/off・Disconnect・network の行・鍵の Join を一つ預かり、答えの後に送る（system bar の ws005-p019 と同じ形、scan は預からない）。`settings/network.c`。
5. **C4**（q619 で追加、D4・D5）: 出力の無い Sound の slider を無効の見た目に、Home の Sound の tile と頁の語を揃える、Network Activity の期間の説明。
範囲の外: libkeiui への移行（WS090 p007）、新しい頁、compositor、candidates の C2・C3・C6・C8・C9（ユーザーの採否待ち）。

## 受け入れ

- guest の新しい手順（`settings-p012.sh`）: Ctrl+F → `wall` → Down → Enter で Wallpaper の頁が開く（log `SEARCH open page=wallpaper`）、touch の drag で頁の pane が scroll（log の scroll の位置と画面）。
- `settings-regress.sh`（8 本）・host の試験 PASS、style-check の違反 0、build の warning 0、boot test PASS。

## 検証の方法と範囲

QEMU の Venus（touch は pen の image の注入）。console・serial の log では判定しない。 やっていない確認は「未実施」と書く。

## 受け入れ（C1・C4、q619 で追加）

- C1: 待ちの間に押した操作が答えの後に送られる（host の試験で request の順、guest の p003 の回帰）。C4: 画面（host の描画と guest）。

## 未決の判断

なし（p010 の候補で別の優先が選ばれたら Q1 が順を変える）。

## Event

2026-10-02 / ws089-beta1-plan: fg019 の計画で新設。

2026-10-03 / q619: Q1 の dispatch で C1・C4（p010 の候補、計画エージェントの推奨）をこの Phase の Queue に含めた（承認: user「Settingsも重点的にしましょう」と自走の指示。C2・C3・C6・C8・C9 はユーザーの採否待ちで含めない）。

## q619-i01 の結果（P1 generation4、2026-10-03 04:08〜05:40、base main `39dbf8c15`）

### 実装したこと（`userland/desktop/settings/`）

| 項目 | 内容 | file |
| --- | --- | --- |
| 1 検索の結果の key | 結果の一つが「選ばれた」状態（新しい一覧では先頭、左の一覧の今の頁と同じ `SE_COLOR_SELECTION` の地）。結果を出している間、窓の Up・Down で選び直し（端で止まる）、Enter（窓でも titlebar の欄でも）で選んだ結果を開く。選び直した行は頁の pane の中に scroll して見せる。log `SEARCH chosen index=N page=WORD` | `search.c`（`se_search_open_chosen`・`se_search_step`・`search_reveal`）、`ui.c`、`settings.h`（`se_search` の `chosen`・`reveal`） |
| 2 touch の drag の scroll | 指の押下（`se_event` の新しい `touch`、`window.c` が指の事象に付ける）が頁の pane か左の一覧の上なら保持し、上下に 10 px（`UI_TOUCH_SLOP`）より多く・横より縦に動いたら、その pane の scroll が指に付いて動く（慣性は無し、今の wheel の model と同じ）。scroll になった押下は click しない。slider の上の指は slider を動かす（従来どおり）。log `TOUCH scroll start pane=page|list`・`TOUCH scroll end pane=… scroll=N` | `ui.c`（`ui_touch_press`・`ui_touch_move`・`ui_touch_release`）、`settings.h`（`se_touch_scroll`）、`window.c` |
| 3 左の一覧の key | 見直しの結果、今の動き（Up・Down で前後の頁、端で止まる、各頁は履歴の一歩、Home・End・PageUp・PageDown は頁の scroll、Alt+←→ で履歴）を保った。検索の結果を出している間だけ Up・Down は結果の選択に回る。guest で Down×3 → Up → Alt+← → Alt+→ の履歴を確かめた | `ui.c` |
| 4 C1 Wi-Fi の待ちの slot | libkeiland は一度に一つの request。scan などの答えを待つ間に押した switch・Disconnect・network の行・鍵の Join を一つの slot に預かり、答えの後に送る（後から押した物が前の物に代わる）。scan は預からない。鍵の Join は鍵を直ちに保存し、PROFILES → JOIN の段を slot から始める。以前は「Wait for the network to answer, then try again.」で押した操作を捨てていた（D1） | `network.c`（`network_ask`・`network_send_waiting`・`network_outcome`）、`settings.h`（`pending_*`） |
| 5 C4 文言と無効の見た目 | 出力の無い Sound の slider の値を「—」に、無効の slider の knob を灰に（Appearance・Keyboard・Mouse の保存できない時も同じ）。Home の Sound の tile を頁と同じ語に（`No sound service`・`Running, no sound output`・`Volume N%`・`Muted`）。Network Activity の説明を「The graph shows every interface's traffic since this window opened.」（合計の下の「Since the computer started」と期間が分かれる）（D4・D5） | `widgets.c`・`page-input.c`・`page-home.c`・`page-network.c` |

### 試験（`plan/ws089/tests/`）

- host: `host-render.c` に `touch=X,Y,X2,Y2`（指の押下・4 歩の移動・離し）。新しい `host-slot.sh`（`host-slot.c`: 一度に一つの request の偽の libkeiland で
  `network.c` を動かす 6 case、19 check）。**直す前の `network.c` では 16 check が FAIL、直した後は全て PASS**（D1 の直す前 FAIL・直した後 PASS）。
- guest: 新しい `settings-p012.sh`（touch の image `config-amd64-settings-touch.mk` = Settings の image + `/dev/input-inject` と `touchinject`）。
- `settings-wait.sh` の `find_window` は窓の map を最大 15 秒待つ（llvmpipe で Settings の最初の frame が数秒かかり、p002 が `window at 0,0` で 1 回 FAIL した）。

### 確認（QEMU の Venus と host。実機は未実施）

| 確かめ | 結果 |
| --- | --- |
| host: `host-build.sh`・`host-preferences.sh`・`host-slot.sh`、`settings-render` の search の key・touch（頁と一覧の scroll、tap は click、slider の上の指は slider） | PASS |
| style-check（`userland/desktop/settings/*.c *.h`、`host-slot.c`） | 違反 0（直す前も 0）。`host-render.c` は既存の sscanf の分岐の形に 1 件（call-in-condition）足した |
| build: `build-settings-image.sh build/p1-settings` と touch の image | exit 0、warning 0（`-Wall -Wextra -Werror`） |
| `settings-p012.sh`（touch の image） | **PASS**: Ctrl+F → `wi` → Tab → Down → `SEARCH chosen index=1 page=ethernet` → Enter → `SEARCH open page=ethernet`、Ctrl+F → `wall` → Enter → wallpaper、一覧の key と履歴、指の drag で頁が scroll（`scroll=300`、頁は開かない）、一覧の行の tap で Wi-Fi、一覧の drag（`pane=list scroll=195`）。画面 `build/ws089-p012/p012/search-chosen.png`・`touch-before.png`・`touch-after.png` |
| `settings-regress.sh`（8 本、通常の image、負荷の無い新しい guest） | 1 回目 p002 が `window at 0,0`（map の待ち不足）で FAIL → `find_window` に待ちを足して流し直し **PASS（8 本）** |
| `volume-p005.sh`（volume の image を作り直し） | **PASS** |
| boot test | PASS（`build/ws089-p012/boot-test/login.png`） |

### 未達・止めたこと

- **範囲 1 の「titlebar の欄から Down で結果へ」は未達**: 検索の欄は zdesktop の titlebar の field（`userland/desktop/wayland/titlebar-shell.c` の field の key の処理）で、
  Down は field が食べて何もしない（Settings に届かない）。今は Tab（field を `LEFT` で出る）→ Down・Up → Enter、または欄で Enter（選んだ結果＝先頭を開く）。
  欄からの Down には compositor の変更（Down・Up で field を `LEFT` で終える、または client に渡す）が要る。compositor は P2 が変更中で、この Phase の所有の外。
  再開の条件: Q1 が compositor の Phase で field の Down を扱うか、ユーザーが「Tab → Down」を受け入れとして認める（後者なら cleared にできる）。
- 調べの途中の注意（記録）: 試験の image を他の build の directory の複写から作ると、`.d` の target が元の directory の名前のままで header の依存が効かず、
  `settings.h` を変えても `main.o` などが古い構造体のまま残った（指の事象がずれて見えた）。build の directory の settings・notes の object を消して作り直して解決。
  project の build の不具合ではない（複写の仕方の問題）。
- Linux・FreeBSD の Keiland の build（settings の `Makefile.linux`・`Makefile.freebsd`、新しい file は無い）は未実施。
- 実機（5330 の touch screen・Wi-Fi の実 radio）での確認は未実施（C1 の効果は実 radio の scan で効く。p014）。

## 2026-10-06 q805 P2: 欄の Down（範囲 1 の残り）

- `userland/desktop/wayland/titlebar-shell.c`（`zwl_titlebar_key`）: suggestion が出ていない時の Down は、Tab と同じく field を `ZWL_TEXT_LEFT` で終え、key（press と release）を window に渡す（0 を返す）。Settings は先頭の結果が選ばれた状態で Down を受け、2 番目へ（`SEARCH chosen index=1`）。suggestion が出ている時の Up・Down（候補を光らせる）は今までどおり。同じ関数の既存の style の指摘 4 件（段落の空行）も直した。
- 他の app への影響: titlebar の欄を持つ app（Files の検索、Browser の address など）で、suggestion の無い時の Down は欄を出て window へ行く（今までは何もしなかった）。
- `plan/ws089/tests/settings-p012.sh`: Tab を除き、欄で Down → `SEARCH chosen index=1 page=ethernet`。
- 確認: wayland の build warning 0、keiland-linux の build warning 0、style-check 0（直す前は 4）、`sh -n`。QEMU（`settings-p012.sh`、touch の image）は T1（未実施）。
