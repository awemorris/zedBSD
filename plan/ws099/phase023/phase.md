<!-- awesome-plan project=zedbsd record=ws099-p023 -->
# ws099-p023: BUG-136（Gears のタイトルバー）と BUG-137（Terminal のタイトルバーの遅れ）

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: q609-i01 の P2 の提案のとおり（BUG-136/137））（旧: in-progress（q609-i01 は 2026-10-03 03:35 に終了。P2 は cleared を提案、判定は Q1。下の「判定の提案」））
Disposition: normal
Parent: [WS099](../ws.md)
Bugs: [BUG-136](../../bugs/BUG-136.md)、[BUG-137](../../bugs/BUG-137.md)

## 範囲（2026-10-02 ユーザーの実機の指摘）

1. BUG-136: X11 の Gears（`keiland-x11 zgears`、rootless）の窓に Keiland のタイトルバーが出ない原因を調べて直す。他の X11 の窓（zterm）も確かめる。
2. BUG-137: Terminal の初回の起動で、窓の本体の数秒後にタイトルバーが出る。初回だけの読み込み（font・icon・cache、BUG-135 の stat）と、titlebar の surface・menu の初期化の順を、時刻の計測で特定して直す。
3. QEMU の Venus で再現と修正の確認（起動直後の PNG を時刻つきで連写、Terminal の初回・2 回目・3 回目）。build warning 0、C 全文規約、変えた領域の試験、最後に boot-test。実機の確認はユーザー（未実施と書く）。

## 所有 path

`userland/desktop/wayland/` の titlebar・x11 の関係の file、`userland/desktop/terminal/`、`userland/desktop/x11server/`（要る範囲）、`plan/ws099/`。


## 2026-10-02 Q1: 範囲の追加（ws099-p020 / q591 の発見）

直す前（base 901037f9f）から落ちている C 基準の試験も、この Phase で原因を調べて直す: Notes にタイトルバーが出ない（p138・c3、BUG-136 と同じ根の可能性）、p137 の probe-a の focus と key、p134 の probe の窓の角が丸くない、C9 の p072。直した後に C9 ×5 と C3・C4・C8 を流し、p020（BUG-125）の clearance の残り（C9 ×5 FAIL 0）もここで確かめる。SSH の retry を他の C9 の script にも入れる。

## 結果（q609-i01、P2、2026-10-02〜03）

worktree `/home/awe/zedBSD-worktrees/p2`（branch `agent/p2`、main `982f958ae` に合わせてから開始）。QEMU 10.0.11 / zedBSD Venus。判定は zdesktop・app の log（SSH）、QMP/VNC の PNG、試験の exit。console・serial の log は読んでいない。実機は未実施（ユーザーの確認待ち）。

### 共通の原因

ws114-p007（ユーザーの G05「SSD が要求されないか CSD が要求された場合は Keiland が装飾しない」）の後、zdesktop は SSD を明示的に要求した窓（keiland_titlebar か xdg-decoration の server_side）にだけタイトルバーを付ける。B2 では Terminal・Files・Textedit だけを確かめていた。次の client はタイトルバーを要求していなかったか、要求の順が遅かった。

| 症状 | client | 原因 | 直し |
| --- | --- | --- | --- |
| BUG-136 Gears（X11）にタイトルバーが無い | X server（`xserver/wayland.c`） | X の窓を xdg_toplevel にするだけで SSD を要求していない（[直す前の log](evidence/base-gears-zdesktop.log): `DECORATION applied mode=1`、`TITLEBAR create` 無し） | X の top-level ごとに、最初の commit の前に keiland_titlebar を作る。link に libkeiland を追加 |
| BUG-137 Terminal のタイトルバーが遅れる | Terminal（`terminal/window.c`・`main.c`） | titlebar を窓・renderer・menu の後に作るため、SSD の configure の ack より前に最初の画像を commit する。その commit は CSD で map され（[直す前の log](evidence/base-terminal-zdesktop.log): `TITLEBAR create` → `CONFIGURE serial=2` → `DECORATION applied mode=1` → `MAP`）、タイトルバーは次の commit まで出ない。QEMU では 5.5 秒たっても出なかった（[PNG](evidence/base-terminal-5s.png)） | `terminal_window_open` の中の registry の roundtrip の前に titlebar を作る。roundtrip の間に ack されるので、最初の画像から SSD になる |
| Notes にタイトルバーが無い（p138・c3） | Notes（`notes/window.c`・`app.h`） | SSD を要求していない | Gears と同じ形で、最初の描画の前に作る |
| p134 の probe の角、p137 の focus と key、C9 の p072 | 試験の client（`userland/tests/popup-probe`・`wlshm`） | SSD を要求していないので、タイトルバー（閉じる・最小化の button、角）が無い | 既定で keiland_titlebar を要求する。`--csd` で外せる（G05 の前に試験を書いた時の状態に戻す） |

link の変更: `platform/amd64/vmunix.mk` の popup-probe・wlshm・xserver に `libkeiland.so` を足した。所有 path の外なので、統合のときに Q1 に確かめてもらう。

### 直した後の観測（`build/p2-p023-fix`）

- Terminal を 3 回起動した（[1](evidence/fix-terminal1-zdesktop.log)・[2](evidence/fix-terminal2-zdesktop.log)・[3](evidence/fix-terminal3-zdesktop.log)）: 3 回とも `TITLEBAR create` → `DECORATION applied mode=2` → `MAP` の順で、本体とタイトルバーが同じ画面に出た（[PNG](evidence/fix-terminal-first.png)）。
- Gears: タイトルバー、button、丸い角が出た（[PNG](evidence/fix-gears.png)、[log](evidence/fix-gears-zdesktop.log)）。
- Notes: タイトルバーと menu が出た（[PNG](evidence/fix-notes.png)）。
- build: compiler の warning は 0。log の warning は外部の openssh の deprecated だけで、既存のもの。変えた C の file で `style-check.py` の違反 0、`git diff --check` も PASS。
- criteria: C3（p138・c3-swipe-back）PASS、C4（p137）PASS、C8（p134）PASS（[results](evidence/crit-1-results.txt)）。直す前（p020 の base、`901037f9f`）はこの 4 本が全て FAIL だった。
- zterm（X11、Q1 の追加の指示）: 直した image で `TITLEBAR create` → `DECORATION applied mode=2` → `MAP` の順になり、タイトルバーが出た（[PNG](evidence/fix-zterm.png)、[log](evidence/fix-zterm-zdesktop.log)）。直す前の zterm は流していない。直す前の X11 の窓の証拠は Gears で取った。
- 1 回目の criteria（C3・C4・C8・C9）で流した p072 の画面（最小化・Wiseview・drag・desk2・desk1・戻し）も目で確かめた: 期待どおり。

### SSH の retry（Q1 の指示、p020 の続き）

`plan/ws099/tests/guest-retry.sh` を新しく作った。ssh の status 255 のときだけ、合わせて 3 回まで試し、retry は stderr に出す。C3・C4・C8・C9 の各 script（p052・p053・p072・p076・p126・p128・p134・p137・p138・cursor-owner・c3-swipe-back）の `guest()` から使う。期待の値・判定は変えていない。

### 受け入れ（直した image `build/p2-p023-fix`、`criteria.sh … C3 C4 C8 C9` を 5 回、[summary](evidence/acc/summary.txt)）

| 回 | 結果 |
| --- | --- |
| 1 | 12 PASS / 2 FAIL: C9 の p128（App Home から Files を開いた後の `ZWL GLASS launch` が 20 秒で出ない。[log](evidence/acc/run1-p128.log)）と cursor-owner（試験の途中で guest の session が無くなり、QMP が接続を拒否し `no guest is running` になった。[log](evidence/acc/run1-cursor-owner.log)） |
| 2 | 14/14 PASS |
| 3 | 14/14 PASS |
| 4 | 14/14 PASS。background の task の 2 時間の上限で p052・p053 の後に止まったので、残りの 8 本を別に流した（[acc-4](evidence/acc/run4-results.txt)・[acc-4b](evidence/acc/run4b-results.txt)） |
| 5 | 14/14 PASS |

- p128 の同じ形の FAIL（`HANDOFF go=1` はあるが `GLASS launch` が無い）は q577 の supplement でも 1 回出ている。今回の変更の前からある、時々起きる失敗と見ている。原因は調べていない（Files の起動はこの Phase で変えていない）。
- cursor-owner の回は guest が途中で無くなった。原因は未確定（QEMU が落ちたのか、誰かが止めたのかは分からない）。2〜5 回目では出ていない。
- SSH の retry が起きたのは 5 回の合計で 6 回。どれも 2 回目以内の試行で回復した。
- boot test: PASS（[PNG](evidence/boot-login.png)）。
- 直した後に変えたのは Terminal の comment だけ（`window.c` の BUG-137 の説明を、観測した順に合わせた）。image は comment を直す前の source から作った。

### 判定の提案

- BUG-136・BUG-137、および Q1 が足した Notes・p137・p134・p072 は、原因と直しの証拠がそろった（QEMU）。p023 は cleared を提案する。実機（5330）での Gears・Terminal の目視は未実施で、ユーザーに依頼する。
- p020 の残り（C9 ×5 で FAIL 0）は厳密には満たしていない。5 回のうち 4 回は FAIL 0。1 回目は p128 の時々起きる失敗（前からある）と、guest が途中で無くなったことで落ちた。p020 を clear するかは Q1 が判断する。
- 原因の G05（ws114-p007）について ws114 の記録に残す一文（Q1 が書く）の案:「2026-10-03 / ws099-p023: G05 の後、SSD を要求していなかった X server・Notes・試験の client（popup-probe・wlshm）と、要求の順が遅かった Terminal にタイトルバーが出なかった（BUG-136/137）。client 側で、最初の描画の前に keiland_titlebar を作るように追従した。」
