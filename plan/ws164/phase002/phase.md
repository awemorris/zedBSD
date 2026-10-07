<!-- awesome-plan project=zedbsd record=ws164-p002 -->
# ws164-p002: Welcome の実装（設定の key、Settings の welcome の mode、About の button、compositor の起動）

Parent: [WS164](../ws.md)
Status: cleared（2026-10-06 Q1 判定: T1-269 の QEMU で期待どおり。実機は UAT）
Disposition: normal
Queue: q821（P2 の第 1 段の列、2026-10-06 Q1）
依存: [p001](../phase001/phase.md)（H1〜H4 決定済み）

## 範囲

[p001](../phase001/phase.md) §3 のとおり: 設定の key `welcome.done`、compositor が最初の login で `settings --welcome` を起動、Settings の welcome の mode
（Welcome・Network・Look・Keys・Done の 5 段）、About の「Show Welcome again」、host 試験。第 1 段の規則（2026-10-06 ユーザー）で正常系だけ。

## 実装（2026-10-06 P2）

- **設定の key**（`settings-keys/settings-keys.c`、共有の表。H4 の Q1 の承認の範囲）: `welcome.done`（compositor の key、bool、既定 0、保存する）。
- **Settings の welcome の mode**（新規 `settings/welcome.c`、`settings.h`・`ui.c`・`main.c`・`look.c`）:
  - `settings --welcome`（と、既に動いている Settings への引き渡しの語 `welcome`）で始まる。list の pane を隠し、頁の pane に段を描く。
  - 段 1 Welcome: Kei の mark、「Welcome to Kei, NAME」（passwd の GECOS の名前、無ければ login 名）、一行。段 2 Network: Wi-Fi の radio が有れば
    Settings の Wi-Fi の頁（一覧・接続・鍵）、無ければ Ethernet の頁。段 3 Look: Wallpaper と Appearance の頁。段 4 Keys: Kei の主な key の表。
    段 5 Done: 「You are all set」。段 2・3 は頁の control がそのまま効く（`app->page` を Wi-Fi・Ethernet・Appearance にする）。
  - pane の下の帯: 段の点 5 つ、Back（最初の段を除く）、Next（最後は「Start using Kei」）。右上に Skip。control の番号は頁の物と重ならない 9000〜。
  - 「Start using Kei」: `welcome.done` を 1（`se_look_set`、look.c の書き込みの口を公開）、`files` を起動（Today が開く）、窓を閉じる。Skip:
    `welcome.done` を 1 にして閉じる。窓の × は印を付けない。Welcome の途中で menu などから別の頁へ行くと Welcome を抜ける（印は付けない）。
  - log: `ZSETTINGS WELCOME step=N name=welcome|network|look|keys|done`、`WELCOME done|skip error=`、`WELCOME files error=`、`WELCOME left page=`。
- **About**（`page-about.c`・`pages.c`）: 頁の下に「Show Welcome again」の button（`se_about_press`）。About の検索の語に welcome。
- **build**: Settings の 3 つの Makefile に `welcome.c`。

## 試験と結果（host、2026-10-06）

| コマンド | 結果 |
| --- | --- |
| `sh plan/ws164/tests/run-host-welcome.sh <scratch>`（新規: Settings の host の描画で About の button → 5 段 → Back → Start using Kei、Skip。各段の PNG） | 8/8、PASS。PNG を目視（Welcome・Network（Wi-Fi の一覧）・Look・Keys・Done、帯の点と Back・Next） |
| `sh plan/tools/settings/host-store.sh`・`host-settings.sh`（key の表の追加の回帰） | 43/43・38/38 |
| build: zedBSD の `bin/settings`（-Werror）、keiland-linux の `bin/settings` | warning 0 |
| style-check（welcome.c・page-about.c・main.c・look.c・settings-keys.c、ui.c の変えた所） | 新しい違反 0（ui.c の既存の 5 件は前から） |

## compositor の起動（2026-10-06 P2、WS131 p022 の統合の後、Q1 の許可）

- `wayland/desktop.c` の `desktop_welcome`（`kwl_desktop_tick` が desktop の program の起動の後に呼ぶ）: login の session（`server->session`、greeter でない）で
  1 回だけ、`welcome.done` が 0 なら `KEILAND_BINDIR "/settings --welcome"` を `kwl_spawn` で起動する（落ちても起こし直さない）。`--testing`・login の画面は
  session でないので起動しない。値が読めない（store が無い）時は起動しない。log `KWL WELCOME start pid=`・`KWL WELCOME skip done= error=`・`start-failed`。
- `wayland/settings.c` に `kwl_settings_number`（設定の今の値を数で、`kwl.h`）。
- build: zedBSD の `bin/wayland`・keiland-linux の `bin/wayland` warning 0。style-check の新しい違反 0（settings.c の既存の 1 件は前から）。host の
  試験は無い（compositor の起動の規則は T1 で: 新しい account の最初の login で `KWL WELCOME start`、Done の後の login で `KWL WELCOME skip done=1`）。

## 未実施・残り

- QEMU（T1）: 新しい account の最初の login で出ること（compositor の起動の後）、段の操作、Files の Today、2 回目の login で出ないこと。未実施。
- 準正常系・異常系の未実装は [WS177 の P2 の一覧](../../ws177/backlog-p2.md)。

## q875（P1、2026-10-08）: 言語と入力の段（p001 の H3）

ws177 の backlog-p2 の 29 行「言語と入力の段（WS154 の Languages）」を実装した。WS154 の Languages の頁ができたので、p001 の H3 のとおり Look と Keys の間に入れた。

- `settings/welcome.c`: 段を 6 つに（Welcome・Network・Look・**Languages**・Keys・Done、log の名前は `languages`）。Languages の段は header「Languages and input」の下に `se_languages_draw`（入力の方式と表示の言語、管理者には login の画面の言語）を描き、その段の controls は Languages の頁の物（`welcome_page` が `SE_PAGE_LANGUAGES` を返す）。
- Keys の段の desktop の移動を「Alt+Shift+Left / Right」に（ws181-p010 の key、同じ q875 の 1 番の commit）。
- 翻訳: `locale/settings.keys` に段の header 2 つ、`locale/ja/settings.tr` を `tools/i18n/tr.py update` で更新して日本語を入れた（`tr.py check`: 140 entries、140 translated、0 problems）。
- host の試験 `plan/ws164/tests/run-host-welcome.sh` を 6 段に。Languages の段で control 5（日本語）→ 4（英語）を押し、`LANGUAGES ui language=ja`・`=en` が出ること（その段の switch が Languages の頁の物）を足した。結果: PASS（11 項目 ok、PNG は `build/ws164-welcome.run.v8NJhF/4-languages.png`・`5-keys.png`）。
- 試験の道具の直し（同じ commit）: Settings の host の renderer（`plan/ws089/tests/host-build.sh`）が link できなくなっていた（printers・displays・power の `kl_system_*` と WS168 の `preview_picture` が無い）。`plan/ws089/tests/host-kl-system.c` に compositor が無い時の答えの stand-in を足し、`plan/ws089/tests/host-preview.c`（新規、壁紙の tile を process の中で共有の decoder で読む）を build に入れた。
- build（warning 0）: `make -j16 BUILD=build/ws164-p002 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk build/ws164-p002/bin/settings`（rc 0）。
- QEMU: 未実施（T1 で Welcome を流すなら、T1-269 (2) の手順で Next を 1 回多く押す）。
- 既知: host の renderer は fallback の font が無く「日本語」が □ で出る（host だけ、guest は font がある）。
