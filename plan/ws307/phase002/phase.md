<!-- awesome-plan project=zedbsd record=ws307-p002 -->

# ws307-p002 OS 名「Kei」を削除して zedBSD に統一する

- 親: [WS307](../ws.md)。仕様: [spec.md](../spec.md) §13a（U15・U26〜U29・J3）。Queue: q1665 / q1665-i01（担当 P4、2026-10-29）。
- Status: in-progress（A〜H を commit。q1698-i01 で asset の upload と f24bd97bf の cherry-pick（16e45a703）を済ませた。cleared の判定は Q1、材料は下の「結果 q1698-i01」）
- Queue: q1665-i01（前の P4）、q1665-i02（2026-10-29、P4、worktree p4-ws307f、基点 35d2a373c）、q1671-i01（2026-10-29、P5、worktree p5-ws307p002、基点 86d6441af）、q1698-i01（2026-10-30、P4、worktree p4-ws307o、基点 d3abf7c27）。
- 出典: 2026-10-28 ユーザー「P4に伝えてください。起動ロゴ画像を含めて、KeiというOS名を削除して、zedBSDに統一します。インストーラだけでなくすべてに影響します。そういうphaseをこのWSに追加してください。」、U26・U27・U28・U29・J3（ws.md の原文）。

## 前の段: plan/tools を user → kei の順に（Q1 が q1665 に足した、2026-10-29）

- commit bfc63703d（main に merge 済み 0281b40f5 の前の分）: `plan/tools/aat/scenarios/aatlib.py`（Run の作成の時に target の uid 1000 の名前を読み USER・PASSWORD・HOME を決める、`AAT_USER` で上書き、`--local` では読まない）と helper の `/home/kei`・`chown kei`・`su kei`、`plan/tools/aat/aat`（sudo の password の既定は login の利用者の名前）、`plan/tools/hw5330/5330-{run,bootcycle,survey}.sh`（user → kei の順に sshpass）、`plan/tools/gpu-perf/measure.sh`（uid 1000 の名前で su）、`plan/tools/guest/serial.py` の comment。`plan/tools/aat/tests/run-host.sh` PASS。

## 棚卸し（2026-10-29、基点 61630bb0b）

`git grep -I -i kei`（keiland を除く、plan/ 以外）499 行。分類と扱いは Q1 に送った（2026-10-29 の SendMessage）: A 利用者・hostname、B OS 名の定義、C program の文字、D 診断の識別子、E comment（GPU の protocol など）、F 絵（U29・J3）、G license の記録の文（J3）、H release の nightly、I docs/release（出した版の記録）。

## 結果（2026-10-29、P4）

- A（commit 539a8c25a）: `userland/base/etc/passwd`（`user:x:1000:1000:User:/home/user:/bin/sh`）・`shadow`（`user` の password `user` の SHA-512 crypt、`openssl passwd -6`）・`group`（wheel・network の member と `user:x:1000:`）・`rc.conf`（`hostname: zedbsd`）、getty・sh・monitor の既定の host `zedbsd`、`userland/desktop/sessiond/autologin`（`user`）と comment、`config/current-uat.mk`・`config/release/config-amd64-beta2.mk` の comment、`tests/scenarios` の 58 file（利用者の語 `kei` → `user`、`/home/kei` → `/home/user`。`Kei Lab`・`Kei Example`・`kei@example.net` は C の残り）。
- B（commit a0cc36354）: OS 名の 1 か所は `include/kern/os-name.h`（`ZEDBSD_OS_NAME` "zedBSD"・`ZEDBSD_OS_ID` "zedbsd"）。build は `config/os-name.mk` がその header から同じ 2 つを読む（`Makefile` が include）。`/etc/os-release` は `NAME="$(ZEDBSD_OS_NAME)"`・`ID=$(ZEDBSD_OS_ID)`・`PRETTY_NAME="$(ZEDBSD_OS_NAME) $(ZEDBSD_VERSION_NAME)"`（`Kei/zedBSD` を外した）。生成の確認: `make … version` で `NAME="zedBSD"`・`PRETTY_NAME="zedBSD 1.0.0 Beta 2"`。
  - 技術の判断 T26（spec §13a の「make の断片に置く」からの変更、Q1 の確認を求める）: 値の置き場を make の断片でなく C の header にし、make が header を読む。理由: 全ての architecture の userland の CPPFLAGS（amd64・arm64・riscv64・x68k・sparcv9（ユーザーの物）…）と Linux・FreeBSD の Keiland の build は `-I.` を持つので、`#include "include/kern/os-name.h"` で全てに同じ値が届き、各 platform の mk に `-D` を足さずに済む（sparcv9 の mk に触れない）。定義は 1 か所のまま。
- C の base の分（commit a0cc36354）: base の 31 file の版の表示 `(Kei)`、`ar` の `Kei ar 1`、`at` の job の header、`fetch` の User-Agent、service console の題を `ZEDBSD_OS_NAME` に。`ls` の comment。
- E・G（commit 08b64e95f）: GPU の command protocol の名前（J3）を「zedBSD GPU command protocol」に（`include/uapi/gpu-op.h`、5 つの driver の header、2 つの `vk-stream.c`、libvulkan の Makefile と 2 つの noct の道具、`include/libc/vulkan/API-PROVENANCE.md`。識別子・番号・形は変えていない）。license の記録の文（J3）: `ime/dict/Makefile`・`ime/skk-dict/Makefile`・`SKK-JISYO.ja` の header・それを作る `merge.sh` の雛形の「for Kei」「Kei's input method」を zedBSD に（日付と原文の引用は保つ、「formerly SKK-JISYO.kei」は旧い file 名の注記として残す）。

- H の段 1（mergeable、asset の名前・URL・SHA-256 は不変）: git mv（Q1 の OK、2026-10-29）`tools/release/kei-nightly.mk`→`zedbsd-nightly.mk`、`kei-nightly/`→`zedbsd-nightly/`、`make-kei-nightly-{base,zip}.py`→`make-zedbsd-nightly-{base,zip}.py`。target `zedbsd-nightly-base`・`zedbsd-nightly-zip`、変数 `ZEDBSD_NIGHTLY_*`、出力 `$(BUILD)/zedbsd-nightly.zip`、CI の `ci.yml`・`release.yml`（release の title「zedBSD <名前>」）、`plan/tools/hw5330/README.md` の path。`make -n … zedbsd-nightly-zip` で取得の URL が今の asset のままであることを確かめた。`plan/bugs/BUG-191.md:42` の旧い path は ticket なので Q1 に。

## 確かめ（2026-10-29、P4）

- build（`ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/ws307-main`）: getty・sh・monitor・sessiond、変えた base の 29 の program と service → rc 0、`error:`・`warning:` 0。os-release の生成（上）。
- 未実施: image の起動（boot-test.sh）、AAT の login（user/user）、`plan/tools` の道具の回帰を古い（kei）・新しい（user）の両方の image で（spec §13a の受け入れ）。

## ユーザーの決定（q1665-i02 の間、Q1 の中継）

- 2026-10-29 形式の識別子（原文、クリック）:「全部置き換え」。Q1 の中継: `/KeiNotesImage`・`kei-notes.bin`・`application/x-kei-notes`・libpdf の `Kei` の接頭辞・`~/.config/kei/ime` を zedBSD の名に置き換え、古い名は読まない（移行の code は書かない）。
- 2026-10-29 Q1 の判断: 試験の data の SSID `Kei Lab` と mail の試験の人名 `Kei`・`kei@example.net` は OS 名でないので残す（下の「残す物の一覧」）。完了 WS の試験 3 本の git rm は OK。`plan/tools/pdf/run-pdf-tools.sh` の golden の壊れは q1665 の範囲で直す。

## 結果 q1665-i02（2026-10-29、P4）

- C（commit f19ffa414）: Settings は Linux・FreeBSD でも build されるので、system の名前は os-release でなく build の定数 `ZEDBSD_OS_NAME`（`include/kern/os-name.h`）。About の「Operating system」の行だけ今どおり os-release の `PRETTY_NAME`（無ければ定数）。翻訳の key は名前を `{1}` にした（`Change how {1} looks and works.`・`Welcome to {1}, {2}`・`Start using {1}` など 8 つ、`settings.keys` と `ja/settings.tr`）。`se_named`（pages.c）が `kl_tr` の後に `{1}` を埋める。表の頁でない見出し（検索の結果）は埋めない（利用者の入れた `{` を守る）。About の hero は mark と名前、「powered by zedBSD」は OS 名が kernel 名と違う時だけ出す（技術の判断）。翻訳されない文（「later version of …」・About の menu・IPv6 の行）は文字列の連結で定数を使う。
- C の残り・D・形式（commit e0ce79063・224177be1）: Files の hero と help、imageview・terminal・textedit の About、libGL・libGLESv2・libEGL・GLX・X server の vendor と version、libbrowser の `navigator.platform` を定数に。browser の `start.html` は名前を言わない文に（data なので）。mailer の入力の例を `Alex Example`・`alex@example.net` に。IME の利用者の辞書の header、gpudemo（os-release の `NAME` を読む）、kuidemo・ipv6-probe の data、comment の「Kei の look・blue・mark」を desktop の物に。
  - D: `KEI_GLES_COMPUTE_TRACE` → `ZEDBSD_GLES_COMPUTE_TRACE`（libegl・libglesv2・gpudemo）、IME の log の印 `KEI-IME` → `ZEDBSD-IME`。`plan/tools/gpu-compute` の 2 本は古い image のために両方の変数を渡す、`plan/tools.md` の記述も。`plan/tools/x11/x11-p004.sh`・`x11-p005.sh` の期待は `(zedBSD|Kei)`。
  - 形式（上のユーザーの決定）: `/ZedBSDNotesImage`、libpdf の更新の名前の接頭辞 `ZedBSD`（`ZedBSD1_`〜`ZedBSD9_`）、Notes の添付 `zedbsd-notes.bin`・`application/x-zedbsd-notes`、IME の利用者の辞書 `~/.config/zedbsd/ime`。技術の判断: 形式の識別子は OS 名の macro でなく固定の文字（distribution の名前が変わっても形式は変わらない）。PDF の `/Producer`・`/Desc` の文は OS 名の定数。`plan/tools/aat/make-edit-samples.py` の `/KeiIm0` → `/ZedBSDIm0`（前の版の名前と重なる場面の試料）。
- 試験の道具の直し: `plan/ws089/tests/host-kl-system.c` に `kl_system_phone_*`・`kl_system_displays_rotation*` の stub 7 つ（commit b7578341a、host-build.sh の link の既存の失敗）。`plan/tools/pdf/run-pdf-tools.sh` の golden（commit da90c27b7）: 基点の f9077504e は object としてあるが、その時 libpdf は `userland/base/libpdf/` にあったので path を直し、`-I.` を足した（`userland/desktop/paths.h` と os-name.h）。完了 WS の試験 3 本（`plan/ws095/tests/ime-p004.sh`・`plan/ws127/tests/bug177-guest.sh`・`plan/ws128/tests/terminal-p011-ime.sh`）を git rm、`userland/tests/ime-probe` の comment の参照を外した（commit 1b7b7b7fe）。
- E（commit edf0dbe17）: loader・kernel の splash・HAL の bsp の実装（`cons.c`）の comment の「Kei splash」を「boot splash」に（comment だけ）。`platform/amd64/vmunix.mk`・`tools/build/make-boot-splash.py` の file の名前は F の後。
- README.md の用語の「Kei」の行を「Disk images」に（commit 081df0d8e）。`tests/scenarios` の利用者の名前（passkey の行 `user:1000`、`auth\nuser`）、About の `PRETTY_NAME` の例、文（commit d1b3911c7）。
- F（U29・J3、ユーザーの回答の後）: git mv `userland/desktop/artwork/kei-boot-splash.png` → `zedbsd-boot-splash.png`、中身を案 b（mark は今のまま、「zedBSD」Lato Light、tagline「operating system」）に。`platform/amd64/vmunix.mk`・`tools/build/make-boot-splash.py`・`artwork/README.md`（出典と Lato の OFL、mark の由来）・`mark.c`・`mark.h`・`plan/tools/imageview/make-images.py` の名前と comment。`make … build/ws307-main/boot-logo.ppm` → 1920x1080、spinner は消えた（目で見た）。
- F（案、前の段）: 今の splash から「Kei」と「powered by zedBSD」を消して背景で埋め、Lato Light（OFL）で「zedBSD」を mark の幅に置いた案 1（mark は今のまま）と、mark を「Z」（上下の淡い青の glass の棒と、左下の濃い青から右上へ渡る葉）に描き直した案 2。PNG は `build/ws307-test/logo/`（P4 の worktree、git に入れない）。Q1 経由でユーザーの選択待ち。

## 確かめ q1665-i02（2026-10-29、P4）

- build（`ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/ws307-main`）: settings・files・imageview・notes・terminal・textedit・mailer・browser・keiland-ime・pdfviewer・xserver・kuidemo と `dynamic/libGL.so`・`libGLESv2.so`・`libEGL.so`・`libGLX.so` → rc 0、`warning:`・`error:` 0。ipv6-probe（config で n）は build していない（data の 8 byte の文字だけの変更）。
- host: `plan/tools/keiland/run-host-welcome.sh` → `run-host-welcome: PASS`（PNG で「Welcome to zedBSD, …」「Start using zedBSD」、About の hero「zedBSD」と「This computer and the version of zedBSD.」を目で見た）。`tools/i18n/tr.py check --strict` の settings の catalog は変更の前と同じ 5 件の未訳だけ（既存）。`plan/tools/pdf/host-pdf-find-l.sh` PASS、`plan/tools/pdf/run-pdf-tools.sh` → `run-pdf-tools: ok`（`golden: 600 strokes, 0 differ`）。
- 未実施: image の起動と AAT、`plan/tools` の道具の回帰を古い（kei）・新しい（user）の image で、Linux・FreeBSD の Keiland の build。

## 結果 q1671-i01（2026-10-29、P5）

- compositor（commit bee991836）: `greeter.c`・`glass.c`・`glass.h`・`shell.c`・`notify-popup.c` の comment の「the Kei mark」「the Kei look」を「the mark」「the desktop's look」に。greeter の左下の文字は `"Kei"` から `ZEDBSD_OS_NAME`（`include/kern/os-name.h`）に（login と lock の画面）。書き換えた行の開発の管理情報（ws035-p108）を外した。
- H の段 2（mergeable、commit fa94a73c7）: `make-zedbsd-nightly-base.py` は元の package（`/home/awe/Kei-nightly.zip`、SHA-256 2a66519e… は pin と一致）の top の `Kei-nightly/` を `zedbsd-nightly/` に改名し、`boot.ps1` の `nvme,serial=kei-boot,` を `zedbsd-boot` に（1 回だけあることを確かめて置換。技術の判断: guest の NVMe の serial に見える OS 名。tree の source に `kei-boot` を読む物は無い）。`zedbsd-nightly/` の README（題「zedBSD」、directory の名前、Linux の例の serial）・THIRD-PARTY（題、「The disk image … is zedBSD」、「changes for zedBSD」）・inputs・licenses の文。棚卸し（spec §13a の手順 1）: archive の `boot.bat`・`boot.ps1` は自分の場所（`%~dp0`・`$root`）だけを使い、directory の名前に頼らない。
- H の段 2（**upload の前に main に merge しない**）: commit f24bd97bf を branch `agent/p5-nightly-upload` に置いた（Q1 の指示）。`zedbsd-nightly.mk` の asset を `zedbsd-nightly-base-winq-a10-1.zip`・SHA-256 `962f79263b46581bffb2aae61b0109096e8a21e2476c3008518397fb962cb149`、`make-zedbsd-nightly-zip.py` の root を `zedbsd-nightly/` に。作業の branch は revert（9715c5167）して旧い asset のまま。upload する file は P5 の worktree の `build/tmp/nightly-new/zedbsd-nightly-base-winq-a10-1.zip`（56344120 byte、draft: fork の commit は未確定のまま、今の asset と同じ）。
- docs（commit 08a13cbeb）: `docs/release/README.md` の一般の文の「Kei/zedBSD」を zedBSD に。beta2 の見出しは出した版の名前として残し、その注記を足した。
- 試験（commit 1d28d38ee・9fb989048・c1aa9b86e）: `plan/ws307/tests/config-amd64-p002.mk`（AAT の image から clang・libcxx・emacs を外す。toolchain の package を試験のために作らない、Q1 の OK）、`plan/ws307/tests/p002-names.sh`（下）。

## 確かめ q1671-i01（2026-10-29、P5）

- build: `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/ws307-p5 build/ws307-p5/bin/wayland` → rc 0、`warning:`・`error:` 0。Linux の Keiland: `make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/p5-keiland-linux all -j8` → rc 0、`warning:`・`error:` 0（-Werror）。FreeBSD の Keiland（`plan/tools/keiland-freebsd/backend-test.sh`、tree 9fb989048、FreeBSD 15.1-RELEASE-p6 の guest）: build PASS・build-warnings 0・install・audit・sync-rejected・dmabuf-export-rejected PASS。host-seat-freebsd・host-session・host-power は FAIL: `plan/ws131/tests` の link に `kl_backend_notify_poll_done` が無い（試験の script の source の一覧が古い。この Phase の変更と関係しない、Q1 に送った）。
- image（`AAT_CONFIG=plan/ws307/tests/config-amd64-p002.mk plan/tools/aat/build-image.sh build/ws307-p002`）: rc 0、`warning:`・`error:` 0。古い kei の image は p1-ws309 の `build/ws309/img/hdd-image.img`（branch は A の前、uid 1000 = kei、forge の config）の複写。
- `plan/ws307/tests/p002-names.sh build/ws307-p002/hdd-image.img build/tmp/old-kei/hdd-image.img build/ws307-p002-run2`（QEMU、実機ではない）:
  - 新しい image: boot-test.sh の login prompt PASS（`login.png`）、Venus の guest の起動の画面（`early-03.png`）に新しい splash（mark・「zedBSD」・「operating system」）、SSH、uid 1000 = `user`、`hostname=zedbsd`、`PRETTY_NAME="zedBSD 1.0.0 Beta 2"`（os-release に kei 0）。AAT: os.boot.session-up・desktop.lock.lock-unlock（password `user`）・os.accounts.sudo（password `user`）pass。desktop.session.logout-login は fail（Log Out で greeter は出た: `KWL GREETER open users=1 selected=user`。helper の撮影 `keiland-shot` が greeter の時 `no-active-compositor`）。そこで script が VNC で greeter を撮り（`greeter.png`: card の「User」と左下の mark と「zedBSD」）、`user` と Enter で `KWL GREETER auth user=user`、新しい session の `KWL READY … pid=316 role=normal`（前の session は pid=7）。
  - 古い image: boot-test.sh PASS、SSH、uid 1000 = `kei`、splash は旧い「Kei / powered by zedBSD」。AAT: session-up・lock-unlock（helper が target から kei を読んだ）pass、sudo は fail（`rc=127`: その image の config に sudo が無い。helper は kei の password で打った）、logout-login は新しい image と同じ撮影の fail。script の greeter の login: `KWL GREETER auth user=kei`、新しい session（pid=293）。
  - 最後の行は `p002-names: FAIL`（上の 3 つ: 両方の logout-login の撮影と、古い image の sudo の不在）。どれも名前の変更によらない（新旧で同じか、image の中身）。
  - PNG: P5 の worktree の `build/ws307-p002-run2/new/{early-03.png,greeter.png,boot-test/login.png}` と `old/` の同じ名前。
- nightly の取得: 作業の branch で `make … zedbsd-nightly-base` が今の asset（rev-0 の `kei-nightly-base-winq-a10-1.zip`）を取得して SHA-256 一致、`make … zedbsd-nightly-zip` → rc 0。変更前の builder を `--draft` で流すと今の asset と byte で一致（81120981…、再現する）。新しい base は 2 回作って同じ SHA-256 962f7926…、211 member が全部 `zedbsd-nightly/` の下、旧と中身が違うのは README・THIRD-PARTY・boot.ps1 だけ、その文に kei 0。upload の branch の zip の script で新しい base と新しい image から zip（212 member、`zedbsd-nightly/data/hdd-image.img`）→ rc 0。作業の branch の zip の script は新しい base を拒む（merge の順の確かめ）。upload の branch の `make -n … zedbsd-nightly-zip` の URL は `…/rev-0/zedbsd-nightly-base-winq-a10-1.zip`、今は 404（未 upload）、旧い URL は 206。
- `git grep -I -i kei`（`plan/` を除く、keiland を除く）: 63 行・22 file、全部が下の「残す物の一覧」の物（数えた分類: docs/release の beta2 の 3 file 23 行と README の 2 行、beta2 の config の注記 1、偽の一致 10（gl2ext.h 4・builtins.c 1・libffi 1・zwm の ppm 3・`plan/tools/keiui` の参照 1）、試験の data 4（mailer 1・wifi 2・network-probe 1）、旧い名前の注記 3（mark.c・artwork/README・notes/ui.c の `kei-identity-design.md`）、SKK-JISYO.kei 7、nightly の旧い名前 13（base の builder 4、upload の前の mk 3 と zip の script 6））。file の名前: `plan/tools/keiui/` だけ（偽の一致）。upload の branch を merge すると mk 1・zip の script 0 になる。
- 未実施: 実機、upload と新しい URL からの取得（ユーザーの指示の後）、CI の 1 回の実行（push の後）。`plan/tools` の中の kei（371 行）は棚卸しの数に入れていない（下の「残り」）。

## 残す物の一覧（p002 の後に `git grep -I -i kei`（keiland を除く）に残ってよい物）

- Keiland・libkeiland と内部名（U28）、`plan/` の履歴、`docs/release/zedbsd-1.0.0-beta2*.md`（出した版の記録、Q1 の判断）、`config/release/config-amd64-beta2.mk` の「beta2 の release では kei」の注記。
- 旧い名前の互換・注記: `keiland-linux.mk`・`keiland-freebsd.mk` の `libkeiui` を消す一覧、`SKK-JISYO.kei`（旧い file の名前、`ime/dict/Makefile`・`SKK-JISYO.ja`・`merge.sh`）、`plan/ws035/kei-identity-design.md` への参照（plan の file の名前）、`docs/release/README.md` の beta2 の見出しとその注記（出した版の名前）、`make-zedbsd-nightly-base.py` の元の package の名前（`Kei-nightly.zip`・`Kei-nightly/`・`serial=kei-boot`、SHA-256 で固定の入力）と `zedbsd-nightly.mk` の旧い asset の注記。upload の前は作業の branch の mk と zip の script の旧い asset の名前と root も。
- 試験の data（2026-10-29 Q1 の判断）: SSID `Kei Lab`（`userland/tests/network-probe`、aat の helper、scenario、他の WS の試験）、mail の試験の人名 `Kei`・`kei@example.net`（`plan/tools/mail`・aat・scenario）。
- 偽の一致: `include/libc/GLES2/gl2ext.h` の `MakeImageHandle…`、GLSL の `"kEi"`（builtins.c の型の符号）、`plan/tools/keiui`、`userland/x11/zwm/backgrounds/*.ppm`（binary）、`libffi/Makefile` の `makeinfo`。

## 残り（次の attempt への引き継ぎ、q1671-i01 の後）

1. **upload**（ユーザーの指示で Q1）: P5 の worktree の `build/tmp/nightly-new/zedbsd-nightly-base-winq-a10-1.zip`（SHA-256 962f7926…cb149）を rev-0 に。読み戻して SHA-256 を照合し、branch `agent/p5-nightly-upload`（f24bd97bf）を main に merge、`make … zedbsd-nightly-zip` で新しい URL からの取得、push の後の CI の 1 回。旧い asset は rev-0 に残す（消すかはユーザーの判断）。
2. 試験の道具の古さ（Q1 に送った、この Phase の範囲の外）: `keiland-shot` が greeter を撮れない（QEMU、`no-active-compositor`、新旧の image で同じ）ので desktop.session.logout-login が QEMU で fail。`plan/ws131/tests` の host-seat-freebsd・host-session・host-power の link の一覧に `kl_backend_notify_poll_done` が無い。
3. `plan/tools` の中の kei: 利用者の既定（aat・serial・gpu-perf・hw5330）は前の段で user → kei の順。残る 371 行は試験の data（host 試験の偽の passwd の `kei`、FreeBSD の guest の利用者 `kei`）、記録（hw5330/evidence）、`plan/tools/keiui` の名前など。guest で `kei` に login を前提にする試験（`plan/tools/accounts/user-commands-guest.sh` など）は新しい image で直しが要る。直すか消すかは各試験の次の実行の時（AGENTS.md §9 の試験の整理）、Q1 の判断。

## 結果 q1698-i01（2026-10-30、P4）

- ユーザーの許可（2026-10-30、原文、ws.md）:「GitHub releaseのアップロードはご提案の通り許可します。」
- base の作り直し: この worktree（基点 d3abf7c27）で `python3 -I tools/release/make-zedbsd-nightly-base.py --original /home/awe/Kei-nightly.zip --cache build/tmp/nightly/cache --output build/tmp/nightly/zedbsd-nightly-base-winq-a10-1.zip --draft` → 56344120 byte、SHA-256 `962f79263b46581bffb2aae61b0109096e8a21e2476c3008518397fb962cb149`。P5 の `build/tmp/nightly-new/` の物と byte で同じ（再現する）。
- upload（gh、git push とは別）: `gh release upload rev-0 build/tmp/nightly/zedbsd-nightly-base-winq-a10-1.zip --repo awemorris/zedBSD` → rc 0（`--clobber` 無し）。読み戻し: `curl --fail --location https://github.com/awemorris/zedBSD/releases/download/rev-0/zedbsd-nightly-base-winq-a10-1.zip` → 56344120 byte、SHA-256 962f7926…cb149（一致）。旧い asset `kei-nightly-base-winq-a10-1.zip` は rev-0 に残した（HTTP 200、消すかはユーザーの判断）。
- f24bd97bf の取り込み: merge でなく `git cherry-pick --no-commit f24bd97bf` → `git commit -m WIP -- tools/release/make-zedbsd-nightly-zip.py tools/release/zedbsd-nightly.mk`（16e45a703、競合なし）。9715c5167 の取り消しの後の作業の branch に、upload の branch の 2 file の変更（asset の名前・SHA-256・archive の root `zedbsd-nightly/`）だけが入った。
- 新しい名前での取得（make の規則、CI と同じ target）: 空の `build/releases/` で `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/tmp/nightly-mk zedbsd-nightly-base` → rc 0、新しい URL から取得して pin と一致（`build/releases/zedbsd-nightly-base-winq-a10-1.zip`）。`make … ZEDBSD_NIGHTLY_IMAGE=build/ws307-p015s/hdd-image.img zedbsd-nightly-zip` → rc 0、`zedbsd-nightly.zip` 185336300 byte、212 member が全部 `zedbsd-nightly/` の下、`zedbsd-nightly/data/hdd-image.img` あり。
- `git grep -i -e kei-nightly -e Kei-nightly`（plan/ を除く）: `make-zedbsd-nightly-base.py` の元の package の名前（3 行、SHA-256 で固定の入力）と `zedbsd-nightly.mk` の旧い asset の注記（1 行）だけ（「残す物の一覧」の物）。zip の script の旧い名前は 0。
- 未実施: CI の 1 回の実行（push はユーザーの指示の後）、Windows での zip の起動（範囲の外）。
- CI について見つけた事（この Phase の変更によらない、Q1 へ）: `.github/workflows/ci.yml` の「Package zedbsd-qemu.zip」の段は、直前の段が `config.mk` を pc98 の物にした後で `make zedbsd-nightly-zip` を走らせるので、`zedbsd-nightly: … use an amd64 config` で止まる見込み（`continue-on-error: true`）。また amd64 の最後の build は server の config なので、amd64 の config で走らせても image は server の物。`release.yml` は `ZEDBSD_NIGHTLY_ALLOW_DRAFT=` で走らせるので、draft の base（fork の commit 未確定）を拒む見込み（前の asset でも同じ）。どちらも push の後の CI で確かめる事で、直すかは Q1 の判断。

### cleared の判定の材料（Q1 へ）

- 受け入れ（spec §13a の H）: 新しい名前の asset が rev-0 にあり、tree の規則が新しい URL から取って pin と一致し、zip が `zedbsd-nightly/` の root で作れる → 満たした（上の確かめ）。
- A〜G・compositor・docs は q1665-i01・i02・q1671-i01 で commit 済み、`git grep -i kei` の残りは「残す物の一覧」の物だけ（q1671-i01 の数え）。
- 残る未実施: push の後の CI の 1 回、`plan/tools` の中の kei（371 行、各試験の次の実行の時に直すか消す、Q1 の判断）、試験の道具の古さ 2 つ（keiland-shot と greeter、ws131 の link の一覧、Q1 に送った物）。どれも名前の変更の受け入れの外と見る。
