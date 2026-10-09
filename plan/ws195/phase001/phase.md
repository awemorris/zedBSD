<!-- awesome-plan project=zedbsd record=ws195-p001 -->
# ws195-p001: 今の配置と参照の調べ、/opt/keiland の配置と移行の設計、account-admin の移動

Status: planning（2026-10-09 夜 P1 が調べと設計の案を書いた。下の D1〜D7 のユーザーの判断と design-reviewer の review の後に cleared の候補。code は変えていない）
Disposition: normal
Parent: [WS195](../ws.md)
Queue: Q1 の投入（2026-10-09 夜「main の release の image に影響しない設計の仕事」、実装はベータ3、10/17 の後）

## 範囲

調べ（今の配置、参照の数と種類）と、`/opt/keiland` の配置・起動の経路・試験の追従・移行の互換の設計。code・Makefile・試験は変えない。

## 1. 事実（2026-10-09、tree fee50b81e と T1 の image `t1/build/t1-495img/rootfs`（release の config ＋ python3）を読んだ）

### 1.1 今の配置（zedBSD の image）

| 種類 | 今の場所 | 物 | 決めている所 |
| --- | --- | --- | --- |
| program | `/bin` | browser・calendar・files・imageview・mailer・monitor・music・notes・pdfviewer・phone・photos・settings・terminal・textedit・videoplayer・wayland・xserver（17） | 各 `userland/desktop/<pkg>/Makefile` の `ZEDBSD_USERLAND_PACKAGE` の 15 番目（install dir、既定 `bin`）と `Makefile` の `zedbsd_userland_destination` |
| program（system） | `/sbin/sessiond` | greeter と session の起動の daemon | `sessiond/Makefile`（install dir `sbin`）、`/etc/service.d` の `greeter.service` の `command=/sbin/sessiond` |
| helper | `/usr/libexec` | keiland-ime・keiland-preview・keiland-printd・keiland-x11（shell script）。base の account-admin（4555）も同じ所 | 各 Makefile の `usr/libexec`、keiland-x11 は `wayland/Makefile` の `ZEDBSD_PACKAGE_FILES` |
| shared library | `/lib` | libEGL・libGL・libGLESv2・libGLX・libbrowser・libkeiland・libmedia・libtruetype・libvulkan・libwayland-client・libwayland-egl（11） | 各 library の Makefile（install dir `lib`） |
| data | `/usr/share` | `keiland/`（wallpaper・hand・ime・locale ほか）・`fonts/`（keiland*.ttf 6 個と、font の package の物）・`browser/`・`mview/` | `paths.h` の `KEILAND_DATADIR` と各 Makefile の `--file /usr/share/…` |
| 設定 | `/etc/keiland` | apps.conf・autologin・session（image が置く）、language・desktop・open-with（後で作られる） | `paths.h` の `KEILAND_SYSCONFDIR`、sessiond・account-admin の literal |
| license | `/usr/share/licenses/<package>` | 各 package の本文 | 各 Makefile、`tools/release/license-inventory.py` は make の値を読む |

- `userland/desktop/paths.h`（WS104 p007）が `KEILAND_BINDIR`（既定 `/bin`）・`KEILAND_LIBEXECDIR`（`/usr/libexec`）・`KEILAND_DATADIR`（`/usr/share`）・`KEILAND_SYSCONFDIR`（`/etc`）を持ち、Linux（`keiland-linux.mk`）と FreeBSD（`keiland-freebsd.mk`）は compiler の `-D` で `$(KEILAND_PREFIX)/{bin,libexec,share,etc}`（既定 `/opt/keiland`）に替え、`-Wl,-rpath,$(KEILAND_PREFIX)/lib` で library を探す。Linux の data は `share/keiland/{ime,locale,wallpapers}`・`share/fonts` で、zedBSD の `/usr/share` の下と同じ相対の形。
- `plan/tools/keiland-os-boundary/check.sh` の C4 が、desktop の C の source（sessiond と `keiland/` を除く）の `/usr/share`・`/usr/libexec`・`/etc/keiland`・`/bin/`・`/usr/bin/` の literal を禁じている（`/bin/sh` は可）。だから compositor・app は path を `paths.h` の macro で書いている（`KEILAND_BINDIR` の使用 32 箇所）。
- literal が残る所（移す時に直す物）:
  - sessiond（C4 の外、zedBSD だけ）: `sessiond.h` の `SESSIOND_GREETER "/bin/wayland"`・`SESSIOND_SESSION "/etc/keiland/session"`・`SESSIOND_WALLPAPER`、`main.c` の `/etc/keiland/autologin`、`session.sh` の `exec /bin/wayland …` と wallpaper、`greeter.service` の `command=/sbin/sessiond`、PATH（`greeter.c:487`・`session.c:663` の `PATH=/bin:/sbin:/usr/bin`）。
  - `wayland/apps.conf`（zedBSD 用、`/bin/<app>` の 15 行と `/usr/libexec/keiland-x11`）。Linux・FreeBSD は `apps-linux.conf.in`・`apps-freebsd.conf.in` を `@PREFIX@` で生成している。
  - `wayland/keiland-x11`（shell、`/bin/xserver`、comment の `/usr/libexec/keiland-x11`）。
  - `include/keiland/keiland.h:2528` の `KL_TEXT_EMOJI "/usr/share/fonts/keiland-emoji.ttf"`（libkeiland の `ui/text.c` が使う）。
  - base: `userland/base/common/account.h:43` の `ACCOUNT_ADMIN_PATH "/usr/libexec/account-admin"`（使うのは `libkeiland-backend-zedbsd/account-zedbsd.c` だけ）、`account-admin/main.c` の `/etc/keiland/language`、`login/main.c:136` の PATH、`libpdf/font.c`・`replace.c` の `PDF_FONT_DIRECTORY "/usr/share/fonts"`（macro で替えられる）。
  - package: `userland/packages/libs/fontconfig/files/55-zedbsd-families.conf`、`fonts/hand-hershey`・`noto-color-emoji` の Makefile（`/usr/share/fonts` に入れる）、`libepoxy` の patch（`libEGL`・`libGL` の名前）、`noct/Makefile`（`/lib/libEGL.so`）、`gtk4`（libwayland）。
- 動的 link: `src/rtld/rtld.c` は `LD_LIBRARY_PATH`、DT_RPATH・DT_RUNPATH、既定の `/lib` → `/usr/lib` を探す（2787 行）。
- desktop の library を使う desktop の外の物: libvulkan（`userland/tests` の gpu-forge・mview・wltest・acquire-fence・kuidemo）、libEGL・libGLESv2・libwayland-egl（tests の egltest・gpudemo・glescompute）、libwayland-client（packages の gtk4 と tests の 8 個）、libkeiland（tests の kuidemo）、libEGL・libGL の名前（libepoxy の patch、noct の accel）。libtruetype・libmedia・libbrowser は desktop の中だけ。
- image の symlink: rootfs の staging の tree の symlink は image に入る（例 `/usr/bin/cc -> clang`、`ufs_format.py` の inline symlink）。`--file` は通常の file だけ。
- 利用者の設定の file（`~/.config/keiland/*.conf`、8 種）に program の絶対 path を保存する code は見つからなかった（grep、session の復元は無い）。zedBSD は image で配る（installer は U2 で無い）ので、在る install の更新の経路も無い。
- account-admin: `userland/base/account-admin/`（main.c 1296 行・edit.c 1025 行・edit.h）、`common/account.c`・`login/verify.c`・`passkey/record.c` も compile する。base の package（class basic、既定 y、mode 4555、install dir `usr/libexec`）。`docs/architecture/security.md` の 54〜69・402 行が「base の set-user-ID root、`/usr/libexec/account-admin`」と書く。edit.c は file に触らない純粋な部分（request の解析、名前の規則、passwd・group・shadow の文の編集、空いている ID、管理者の数）、main.c が lock・読み・検べ・書き。
- 境界（Guardrail「配置」と check.sh B1・B3・C3）: OS ごとの backend の tree（`libkeiland-backend-zedbsd/`）は自分の OS の code を使ってよく（`account-zedbsd.c` は既に base の `common/account.h` を include）、compositor と libkeiland は `userland/base/…` を include しない。

### 1.2 参照の数（`git grep -lE`、2026-10-09）

正規表現: `/bin/<上の 17>`・`/sbin/sessiond`・`/usr/libexec/{keiland-*,account-admin}`・`/lib/<上の 11>.so`・`/usr/share/{keiland,fonts,browser,mview,kei}`・`/etc/keiland`。**1211 file**（ws.md の「約 520」は数え方が違う）。

| 区分 | file | 移す時の扱い |
| --- | --- | --- |
| `plan/history`（222）・`plan/bugs`（38）・`plan/wsNNN` の `.md`（316）と証拠の `.log`・`.jsonl`（136） | 712 | 変えない（履歴と証拠） |
| `plan/wsNNN` の実行する試験（.sh・.py・.mk・.c・.in・.conf ほか）: 未完の WS（incomplete・planning）の物 | 253 | 下の §2.5 の基準 |
| 同: 完了の WS の物（WS035 の 59、WS005・WS073 ほか） | 63 | 完了の WS の試験は本来消してある物。今も参照される物（例 `plan/ws035/tests/config-amd64-zdesktop.mk` は T1-485 が使う）は `plan/tools` へ移して登録、他は消す（Q1） |
| `plan/tools`（63: aat 14・titlebar 10・gtk4-linux 5 ほか）・`tests/` のシナリオ（15）・`userland/tests`（5） | 83 | 回帰の道具。直す |
| source・build（userland・platform・tools・fonts、66） | 66 | 直す（上の literal の一覧） |
| docs（3）・plan の他（master・queue・future-work・uat 等 10） | 13 | docs は設計を先に直す。plan の物は Q1 |

## 2. 設計の案

### 2.1 配置（推しの案、D1〜D7 の推しを採った時）

| 場所 | 入る物 |
| --- | --- |
| `/opt/keiland/bin` | 上の 17 の program |
| `/opt/keiland/libexec` | sessiond（D6）・keiland-ime・keiland-preview・keiland-printd・keiland-x11・account-admin（4555、D7） |
| `/opt/keiland/lib` | Keiland の library: libkeiland・libtruetype・libmedia・libbrowser（D1） |
| `/opt/keiland/share` | `keiland/`・`browser/`・`mview/`、Keiland の font（D2） |
| `/etc/keiland` | 設定はそのまま（D3） |
| `/lib` | 描画の API の library は残す: libvulkan・libEGL・libGLESv2・libGL・libGLX・libwayland-client・libwayland-egl（D1） |
| `/usr/share/licenses/<package>` | 変えない（system の license の索引、`INDEX`） |

- build: `paths.h` の zedBSD の既定を `/opt/keiland/{bin,libexec,share}` に替える（`KEILAND_SYSCONFDIR` は D3 の推しなら `/etc` のまま）。Linux・FreeBSD の `-D` は変わらない。package の install dir（15 番目）を `opt/keiland/bin` などに。Keiland の program と library の link に `-Wl,-rpath,/opt/keiland/lib`（DT_RUNPATH、Linux と同じ）。
- 起動の経路: `greeter.service` の `command=/opt/keiland/libexec/sessiond`、`sessiond.h` の 3 つを `paths.h` の macro に（sessiond も C4 の対象に入れ、prune を外す）、`session.sh` を `apps.conf` と同じく `@PREFIX@` の template に、`apps.conf` を `apps-zedbsd.conf.in`（Linux・FreeBSD と同じ形）から生成、keiland-x11 の `/bin/xserver` を生成時に置き換え、`KL_TEXT_EMOJI` を `KEILAND_DATADIR` から、`ACCOUNT_ADMIN_PATH` を backend の側の macro（`KEILAND_LIBEXECDIR "/account-admin"`）へ移し base の account.h から消す。
- PATH（D4）: login・greeter・session の 3 箇所の既定 PATH に `/opt/keiland/bin` を足す（`/bin:/sbin:/usr/bin:/opt/keiland/bin`）。
- 確かめの道具: check.sh の C4 に `/opt/keiland` の literal を足し（`paths.h`・生成の template・Makefile の外で禁止）、sessiond を対象に入れる。

### 2.2 ユーザーに聞く判断（推しの案つき）

| ID | 問い | 案 | 推し（理由） |
| --- | --- | --- | --- |
| D1 | 描画の API の library（libvulkan・libEGL・libGLESv2・libGL・libGLX・libwayland-client・libwayland-egl）も /opt/keiland/lib に移すか | (a) /lib に残し、Keiland の library（libkeiland・libtruetype・libmedia・libbrowser）だけ移す (b) 全部移し、rtld の既定の探す所に /opt/keiland/lib を足す（base の変更） | (a): zedBSD ではこれらが唯一の Vulkan（kernel の i915 の実行器の前段、Guardrail「libvulkan は desktop の境界ではない（driver）」）と唯一の GL・Wayland の client で、desktop の外（gtk4・libepoxy・noct の accel・X の client・tests）が `/lib` の名前で使う。Linux の Keiland も `/opt/keiland/lib` に自前の `libvulkan.so.1`（system の libvulkan の前に WSI を足す front、`libvulkan-compat/Makefile.linux`）と `libwayland-client.so` を置くが、その下の driver は system の物で、zedBSD の libvulkan はその driver の役も持つ。(b) にすると desktop の外の利用者全部に rpath か rtld の変更が要る |
| D2 | font（keiland*.ttf 6 個）の場所 | (a) `/usr/share/fonts` のまま（system の font の場所、base の libpdf・fontconfig・font の package と共有）。paths.h に `KEILAND_FONTDIR`（既定 `KEILAND_DATADIR "/fonts"`、zedBSD は `/usr/share/fonts`）を足す (b) `/opt/keiland/share/fonts` に移し、libpdf の `PDF_FONT_DIRECTORY` と fontconfig の設定もそこへ | (a): libpdf（base）が置き換えの font に同じ物を使い、font の package も `/usr/share/fonts` に入る。移すと base が Keiland の install の場所を知ることになる |
| D3 | 設定 `/etc/keiland` | (a) `/etc/keiland` のまま (b) Linux と同じ `/opt/keiland/etc/keiland` | (a): 管理者が直す system の設定で、sessiond・account-admin（system の言語）も読み書きする。Linux で `/opt/keiland/etc` なのは package が /etc を持たないため |
| D4 | 端末から app を名前で起こせるか（PATH） | (a) 既定の PATH に `/opt/keiland/bin` を足す（3 箇所） (b) 足さない（App Home と絶対 path だけ） | (a): Terminal で `textedit file` などが今どおり使え、試験の script も名前で呼べる |
| D5 | 古い path の互換 | (a) 置かない（試験と道具は一度に直す） (b) ベータ3 の間だけ `/bin/<app>`・`/usr/libexec/…` の symlink を置き、後で消す | (a): image で配り在る install の更新が無く、利用者の設定は path を持たない（§1.1）。symlink は直し忘れを隠し、C4 でも見つからない |
| D6 | sessiond の場所 | (a) `/opt/keiland/libexec/sessiond`（init の service が起こす物で、利用者の命令ではない） (b) `/opt/keiland/sbin/sessiond` | (a): Linux・FreeBSD の配置に sbin が無く、libexec の他の helper と揃う |
| D7 | account-admin の source の置き場 | (a) `userland/desktop/libkeiland-backend-zedbsd/account-admin/`（zedBSD の backend の OS の道具。base の code を使ってよい tree） (b) `userland/desktop/account-admin/`（desktop の直下） | (a): Linux・FreeBSD の backend は OS の道具（useradd など）を使うので account-admin は zedBSD だけの物。境界の規則（B1・B3・C3）の上で base の verify.c・account.c を使い続けられる。install は `/opt/keiland/libexec/account-admin`（4555）、wayland の package の依存にして「Keiland の必須」にする |

### 2.3 account-admin の移動

- source を D7 の所へ git mv、Makefile の package の登録を desktop の側へ（class・mode 4555・install dir `opt/keiland/libexec`）。`common/account.c`・`login/verify.c`・`passkey/record.c` は base のまま compile する（D7 (a) の tree なら境界の上で可）。edit.c・edit.h の置き場は WS196 p001 で決める（useradd などと共有する時は base の `userland/base/common/` に置き、account-admin が使う形が自然。WS196 の判断）。
- `wayland` の package の依存（10 番目の REQUIRE）に account-admin を足し、Keiland を入れた image には必ず入る。Keiland の無い image（最小の config）には入らない（その時の管理は WS196 の useradd など）。
- `docs/architecture/security.md` の「Account administration」を、場所（`/opt/keiland/libexec/account-admin`）と所属（Keiland の zedBSD の backend の道具）に直す。security の性質（set-user-ID root、PATH に載せない、要求の検べ）は変えない。
- 試験: `plan/ws089` などの account-admin の試験は §2.5 の基準。

### 2.4 移行の手順（Phase の案、ベータ3、各 Phase は着手の前に Q1 の割当）

| 案 | 内容 | 確かめ |
| --- | --- | --- |
| p002 | build の規則と配置: paths.h の既定、package の install dir、rpath、apps.conf・session.sh の template、keiland-x11、KL_TEXT_EMOJI・KEILAND_FONTDIR、PATH の 3 箇所、check.sh の C4 | build（amd64 の image の rootfs、warning 0）、rootfs の file の一覧の前後の比較（移った物と残った物が §2.1 の表どおり）、`readelf -d` の RUNPATH、check.sh |
| p003 | account-admin の移動と security.md | build、account-admin の host 試験、境界の check |
| p004 | 試験と道具の追従（§2.5）、docs | 置き換えの script を一度だけ流し、差分を review。消す物の一覧を Q1 へ |
| p005 | T1: AAT の全体と boot-test（login prompt）、Settings の利用者の追加（account-admin の経路）、Terminal の PATH。Linux・FreeBSD の build が変わらないこと（`-D` の値は同じ） | T1 の依頼 |
| p006 | 規約の全文の見直し（ベータ3 の規約の Phase） | — |

### 2.5 試験の追従の方針（AGENTS.md「試験の整理の基準」2026-10-06 を当てる）

- 履歴・証拠（`plan/history`・`plan/bugs`・`.md`・`.log`・`.jsonl`）は変えない（712 file）。
- 実行する試験（未完の WS の 253 と、完了の WS の 63）: master.md の Tools・試験の一覧、未完の WS の未完了の Phase、`tests/` のシナリオ、T1 の未実行の依頼のどれかから参照されていれば、置き換えの表（§2.1 の旧 → 新）で機械的に直す。どこからも参照されない物は直さずに消す（path の一覧を Q1 へ、文書の参照も外す）。完了の WS に残る物で今も使う物は `plan/tools` へ移して master に登録する。
- 置き換えは名前の一覧を固定した script で一度に行い（`/bin/wayland` → `/opt/keiland/bin/wayland` など、§2.1 の表から作る）、D4 (a) なら script の中の呼び出しは名前（PATH）で書いてよい。`/lib/libvulkan.so` など残る物（D1 (a)）は置き換えない。
- 道具（`plan/tools` 63・`tests/` 15・`userland/tests` 5）は直して残す。

## 3. 未確かめ・限界

- 数は 2026-10-09 の tree の grep で、`/usr/share/kei` 以外の data の小さい path（`share/keiland/hand` など）は `/usr/share/keiland` で数えた。正規表現に入らない書き方（変数で組み立てた path）は数えていない。
- Linux の Keiland の libvulkan・libwayland-client は `/opt/keiland/lib` の自前の物（上の D1）。FreeBSD の側は読んでいない（同じ形の見込み、未確認）。
- design-reviewer の review は未実施（ユーザーの判断の後に通す）。

## ユーザーの決定（2026-10-09）

D1〜D7 はクリック「全部推しどおり」（各 (a)）。10/17 の後に design-reviewer を通して p002 から。
