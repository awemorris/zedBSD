<!-- awesome-plan project=zedbsd record=ws307-p004 -->

# ws307-p004 package の header・.pc と pkgconf、導入先で GTK4 の app を build する

- 親: [WS307](../ws.md)。仕様: [spec.md](../spec.md) §6.2（U13、T18）。Queue: q1698 / q1698-i01（担当 P4、2026-10-30、worktree `/home/awe/zedBSD-worktrees/p4-ws307o`、branch `agent/p4-ws307o`、基点 main d3abf7c27）。
- Status: in-progress（実装と試験の script を commit（2aad28963）。build の確かめと QEMU の試験は未実施。2026-10-30 09:13 に Q1 のラップアップの指示で止めた）
- ユーザーの回答（2026-10-30、原文、ws.md）:「clangとlibcxxのビルドは許可しますが、導入先というのはどういう意味なのかわかりません。zedBSDホストでのセルフビルドのことですか？」→ Q1 の解釈: 導入先 = installer で入れた zedBSD（その上の self build）。ユーザーに確認中で、違えば Q1 が知らせる。clang・libcxx の package の build は許可（共有の toolchain の tree は lock のまま、自分の worktree の `build/` で）。

## 実装（commit 2aad28963）

- `userland/packages/devel/pkgconf/Makefile`（新規）: pkgconf 3.0.10（`https://distfiles.ariadne.space/pkgconf/pkgconf-3.0.10.tar.xz`、429268 byte、SHA-256 9371b2f6…e2de。SHA-512 は Alpine の APKBUILD と一致。license は ISC 相当の COPYING）。meson、検索の path `/usr/lib/pkgconfig:/usr/share/pkgconfig`、system の dir `/usr/lib`・`/usr/include`。入れる物: `/usr/lib/libpkgconf.so.8`・`/usr/bin/pkgconf`・link `/usr/bin/pkg-config`→pkgconf・license。menu `packages/devel`、既定 n。
- `userland/packages/external.mk`: 共通の macro `ZEDBSD_EXTERNAL_DEVELOPMENT`（program・stage・usr/ の下の file・usr/ の下の directory（`--subtree`）・SONAME の列 → `/usr/lib/<SONAME の版を除いた名前>` の link）。
- 各 package に開発の file（header・.pc・.so の link）: gtk4（gtk4・gtk4-wayland・gtk4-unix-print の .pc、include/gtk-4.0）、cairo（10 の .pc）、pango（5）、harfbuzz（3）、glib（8 の .pc、include/glib-2.0・gio-unix-2.0・lib/glib-2.0/include）、graphene（lib/graphene-1.0/include も）、fontconfig・freetype・fribidi・gdk-pixbuf・libepoxy・libjpeg-turbo・libtiff・libxkbcommon・pixman、libavcodec（5 の include と .pc）、libcxx（include/c++、unwind の header、libc++・libc++abi・libunwind の link）、openssl（include/openssl と 3 の .pc、SONAME は版無しなので link 無し）、libpng（libpng.pc）。「development packages later」などの古い comment を直した。
- desktop の libwayland・libwayland-egl の image の data に `wayland-client.pc`・`wayland-egl.pc`（`/usr/lib/pkgconfig`）。
- `platform/amd64/installer-items`: Clang の項目の unit に `devel-pkgconf`（`lang-clang devel-pkgconf`、spec §6.2 の「Clang の項目の依存」。clang の Makefile は toolchain なので触らない形）。
- 試験（未実行）: `plan/ws307/tests/config-amd64-p004.mk`（config-amd64 ＋ live だけの openssh・openssl、emacs を外す）、`p004-gtk.sh`（1 本の script: installer の image で NVMe 512 の 24 GiB に base・desktop・firmware・openssh・openssl・lang-clang・devel-pkgconf・desktop-gtk4 を install-admin で導入 → 導入した root で j3 の sync（p008 の残りを兼ねる）→ host で loader の reader が読む → 導入先だけで起動し SSH の tester で `p004-guest-run.sh`: `pkg-config --cflags --libs gtk4`、`cc -Wall -Wextra -Werror` で `p004-hello.c` を build、session で走らせ「P004 MAPPED」、QMP の PNG、C++ の小さな program を clang++ で build・実行）、`p004-guest-install.sh`、`p004-guest-run.sh`、`p004-hello.c`。

## 確かめた事

- pkgconf の package: `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/p004 pkgconf` rc 0、warning 0、check-dynamic-elf PASS、binary の既定の path を strings で確かめた。
- clang・libcxx の package: 同じ BUILD で build し stage 済み（`build/packages/clang/stage/.zedbsd-staged`、`libcxx/stage/.zedbsd-staged`）。共有の toolchain の tree は `toolchain-lock.sh status` で writable 0 のまま。
- make の parse: `make -n ZEDBSD_CONFIG=plan/ws307/tests/config-amd64-p004.mk ZEDBSD_IMAGE_PROFILE=installer BUILD=build/p004img pkgconf` rc 0。
- 未実施: gtk4 と依存の package の build（同時に走らせた 2 本の make が pixman の meson の build dir で衝突し 1 本目が止まった。2 本目はラップアップで止めた。`build/packages/` の cairo・pango・gtk4 などは途中）、`installer-image` の build（新しい .pc・header・link が root の tree と manifest に入るか、重なりの検査）、host の pkgconf での .pc の閉包の確かめ、QEMU の p004-gtk.sh。

## 次の 1 手

1. `make -j32 ZEDBSD_CONFIG=plan/ws307/tests/config-amd64-p004.mk BUILD=build/ws307-p004 ZEDBSD_INSTALLER_TEST_FILES="<python3 plan/tools/guest/guest.py extra-files の値>" installer-image`（1 本だけ、並行の make を走らせない。gtk4 などは BUILD に依らず `build/packages/` で再開する）。warning 0（zedBSD の code）。
2. host: `PKG_CONFIG_LIBDIR=build/ws307-p004/installer/full/usr/lib/pkgconfig:…/usr/share/pkgconfig PKG_CONFIG_SYSROOT_DIR=build/ws307-p004/installer/full pkgconf --cflags --libs gtk4` で .pc の閉包が足りるか。
3. `sh plan/ws307/tests/p004-gtk.sh build/ws307-p004/installer-image.img`（host の load が高いと guest の起動が遅れる。他の build と重ねない）。

## q1711-i01（2026-10-30、P6、worktree p6-ws307、branch agent/p6-ws307、基点 main 5aca73738）

- build（Q1 の許可、2026-10-30「p004 の build を許可します」: 出力は自分の worktree の build/packages と BUILD=build/ws307-p004、共有の build/llvm* は読むだけ）: `flock /tmp/zedbsd-image-build.lock make -j32 ZEDBSD_CONFIG=plan/ws307/tests/config-amd64-p004.mk BUILD=build/ws307-p004 ZEDBSD_INSTALLER_TEST_FILES="<guest.py extra-files>" installer-image` rc 0、`check-amd64-native-image: OK`。clang・libcxx・pkgconf・gtk4 と依存は自分の `build/packages/` で stage（clang は target の LLVM を `build/packages/clang/build` で build、共有の `build/llvm-build` は変化無し）。warning は外部の source（fribidi・pixman など）だけ、zedBSD の code は 0。
- host の .pc の閉包: `PKG_CONFIG_LIBDIR=build/ws307-p004/installer/full/usr/lib/pkgconfig:…/usr/share/pkgconfig PKG_CONFIG_SYSROOT_DIR=build/ws307-p004/installer/full pkg-config --cflags --libs gtk4` rc 0（gtk-4.0・pango・glib・harfbuzz・cairo・graphene などの include と -lgtk-4 … -lglib-2.0）。
- QEMU run1（`sh plan/ws307/tests/p004-gtk.sh build/ws307-p004/installer-image.img`）: **FAIL**（`build/ws307-test/p004.9Et1i3`）。live の system が 900 秒 SSH に応えず。同じ構成で起こし直して gdbstub で見た: 全 CPU が sched_idle、`klog_buffer` で stick の flush の xHCI の時間切れ → root が read-only → `/sbin/init` が EIO。[BUG-359](../../bugs/BUG-359.md) に当たった（Q1 の判断、ticket に観測を追記）。試験の stick を `-set drive.boot.cache=unsafe` に（installer_walk.py と同じ回避）。
- QEMU run2（stick を cache=unsafe に）: **FAIL**（`build/ws307-test/p004.HjyntV`）。導入は全 STEP ok（units に lang-clang・devel-pkgconf・devel-libcxx・desktop-gtk4 と依存の libs-*。/usr/bin/clang・pkgconf・pkg-config の link・gtk4.pc・gtk/gtk.h・glibconfig.h・libgtk-4.so・c++/v1/vector）、**HOST j3-sync ok**（512 の NVMe に導入した root で 2,867,200 byte を書き rename・sync、mount のまま止めて host の ufs-cat が同じ cksum。p008 の残りの確かめ直し）。導入先の起動で session の socket が無い: QMP の screendump に「sessiond: there is no greeter or no display; the console login takes over」。試験の誤り（導入先を GPU 無しの std VGA で起動していた）。導入先を `zdesktop-guest.sh`（Venus）で起動し、窓は `zdesktop-shot.py`（VNC）で撮るよう直した。
- QEMU run3（`VENUS_RENDERER=/home/awe/zedBSD-claude1/build/ws035-sq-venus/install sh plan/ws307/tests/p004-gtk.sh …`）: **FAIL**（`build/ws307-test/p004.mI0a8r`）。導入・j3-sync ok、導入先で SSH ok、`PKGCONF 3.0.10`、`STEP pkg-config-gtk4 ok`、`STEP build ok`（`cc -Wall -Wextra -Werror -o p004-hello p004-hello.c $(pkg-config --cflags --libs gtk4)`）、`P004 MAPPED gtk=4.18.6`・`P004 QUIT status=0`・`STEP hello-mapped ok`。FAIL の 2 つ:
  - C++: `clang++ -std=c++17 p004-cxx.cc` が `'iostream' file not found`。libc++ の header は `/usr/include/c++/v1` にあるが、toolchain の ZedBSD の driver（`toolchain/llvm/patches/0001-add-zedbsd-x86-target.patch` の `ToolChains/ZedBSD.cpp`）に C++ の標準の include の道（AddClangCXXStdlibIncludeArgs）が無い。直すのは toolchain の変更（Q1 の許可が要る）。Q1 に案 A（`lang/clang` の package に `/usr/bin/clang++.cfg`）・案 B（LLVM の patch）を送り判断待ち。
  - 写真: 初回の login の Settings の Welcome が全画面で前にあり、hello.png に GTK の窓が写らない。`p004-guest-run.sh` で hello の前に Settings を閉じるよう直した。
  - 観察（範囲の外、未調査）: GTK が `Unrecognized accessibility backend "\`\xe4..."` を出す（script は `GTK_A11Y=none` を渡す）。環境変数の値が壊れて読まれている疑い。Q1 に報告。
