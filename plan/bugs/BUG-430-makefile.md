<!-- awesome-plan project=zedbsd record=BUG-430-makefile -->

# BUG-430 の Makefile・mk/ の差分の案

[BUG-430](BUG-430.md) の観測 1〜3・5〜7 を直す差分の案。Makefile・`mk/` は Claude が変えない規則なので、
当てていない。差分は [BUG-430-makefile.diff](BUG-430-makefile.diff)（repository の root で
`git apply plan/bugs/BUG-430-makefile.diff`、2026-11-14 の main 4e8a9321bf の上で `git apply --check` が通る）。
観測 4 は試験の道具なので P4 が直した（下の「試験の道具」）。

## 項目ごとの差分

### 1. sysroot を BUILD の下に置く（観測 1）

- 原因: `mk/platform-x86_64/sysroot.mk:13` が `SYSROOT := $(REPO_ROOT)/build/x86_64/sysroot` に固定。
  package は `userland/base/package.mk:126` の `STANDALONE_SYSROOT`（`$(abspath $(BUILD))/sysroot`）、
  `mk/llvm/llvm.mk:32` の `--sysroot` も `$(BUILD)/sysroot` を見るので、`BUILD=build/<phase>/` だと header が
  見つからない。kernel も `mk/platform-x86_64/vmunix.mk:11` が `-Ibuild/x86_64/sysroot/usr/include` に固定。
  同じ固定が i386・rpi4・rpi5 の `sysroot.mk:13` にもある（vf2・pc9821 は既に `$(BUILD)/sysroot`）。
- 差分: 4 機種の `SYSROOT := $(abspath $(BUILD))/sysroot`、x86_64 の kernel の `-I$(BUILD)/sysroot/usr/include`。
- 影響: 既定の BUILD（`build/<機種>`）では path は変わらない。

### 2. package は sysroot ができてから（観測 2）

- 確かめ: `make -j4 vmunix userland`（既定の BUILD）では sysroot の smoke link の後に package が始まり、
  再現しなかった（top の `userland: sysroot .WAIT programs` が効く）。P1 の観測は観測 1（sysroot が別の場所に
  できて、BUILD の sysroot が無い）の現れだった見込みが高い。
- 抜けていた依存: `programs`（`mk/userland.mk:78` は `libc` と stamp だけ）・package の stamp・package の
  target（`make userland/base/ls` など）は sysroot に依らない。新しい BUILD で `make -j4 programs` を流すと、
  sysroot を作らずに package が走り `dirent.h` などが無いと失敗した（当てる前）。
- 差分: `mk/tracking.mk` の末尾に、全 package の stamp と package の target が `$(SYSROOT_COMPLETE_STAMP)` を
  order-only で待つ規則（選ばれていない package、例えば sh が要る libedit の stamp も含める。最初の案では
  選ばれた stamp だけにして、libedit が sysroot の header の写しの途中に走った）。

### 3. 入れ子の build の並列を make の -j に従わせる（観測 3）

- 原因: `mk/build-jobs.mk:21` の `BUILD_JOBS` の既定が「16 か CPU の数の小さい方」で、make の `-j` を見ない。
  `mk/noct/noct.mk:72`（NoctLang の `cmake --build --parallel`）、`userland/packages/external.mk:255`
  （`$(MAKE) -j $(BUILD_JOBS)`）、各 cmake の package がこれを使うので、`make -j4` でも 16 で走る
  （64 thread の host で `--parallel 16`、cmake の作る make は `-j16`）。
- 差分: `MAKEFLAGS` の `-jN` があれば `BUILD_JOBS ?= N`。数の無い `-j` と `-j` 無しは今のまま（16 か CPU の数）。
  `BUILD_JOBS=` を明示すればそれが勝つ。

### 5. 一覧の file が build の写しより後（観測 5）

- 原因: `mk/rootfs.mk:133-161` で `--file` の一覧（`ROOTFS_FILES`・`PACKAGE_FILES`）を入れた後、
  `mk/rootfs.mk:162-183` で `$(BUILD)/etc` などを上から写す。init の DATA（`userland/base/init/Makefile:8`）が
  `$(BUILD)/etc/net.conf` に base の net.conf を入れるので、一覧で替えた net.conf が上書きされる。
  もう一つ、最後の `find … -type d -exec chmod 0755`（`mk/rootfs.mk:198`）が一覧の directory の `--mode`
  （`/root/.ssh=0700` など）を消す。
- 差分: build の写しを先に、一覧（platform・package・新しい `ROOTFS_EXTRA_FILES`）を後に。directory の 0755 は
  一覧の前に。`ROOTFS_EXTRA_FILES`（`--file DEST=SOURCE`・`--mode DEST=MODE` の対）は機種に依らない試験・config の
  file の入口で、最後に入り、source が変われば root を作り直し（`ROOTFS_EXTRA_INPUTS`）、値は
  `$(BUILD)/.rootfs-config` に入る。`userland/desktop/wallpapers/Makefile` の古い `TEST_EXTRA_FILES`・
  `ZEDBSD_TEST_EXTRA_FILES` の参照もこれに替える。
- 注: `mk/rootfs.mk:136` は `.rootfs-config` の `data`・`files`・`platform_files`・`links` の鍵を読むが、
  `.rootfs-config` はこれらを書かない（作り直しの前の名残。害は無いので差分では触らない）。

### 6. networkd が実行する program を常に入れる（観測 6）

- 原因: networkd は interface の UP・address・DHCP・route を `/sbin/ifconfig`・`/sbin/route`・`/sbin/dhcpc` を
  実行して行う（`userland/base/networkd/main.c:4165`・`4190`・`4202`・`4226` ほか）。`mk/userland.mk:14` は
  networkd と net を必ず入れるが ifconfig・route・dhcpc を入れないので、それらを選ばない最小の image
  （WS353 の config は dhcpc だけ選び ifconfig・route が無い）では exec が ENOENT で、`net startup` の UP が
  「No such file or directory」で失敗する。code で確かめた（guest での再現は未実施）。
- 差分: 必ず入れる一覧に `ifconfig route dhcpc` を足す（`/sbin/wifi` は無線だけなので足さない）。
- 判断が要る点: 「networkd を入れる image は ifconfig・route・dhcpc を必ず持つ」をユーザーに確かめたい。
  別の形は、networkd の package が要る program を宣言して選択に足す仕組みだが、今の package の宣言には無い。

### 7. package の stamp が config を追う（観測 7）

- 原因: `mk/userland.mk:55` の stamp は package の source と `package.mk` だけに依る。config の値
  （`ZEDBSD_TEST_SCREEN_CAPTURE` は `userland/desktop/wayland/Makefile:18` で source を選ぶ）を変えても
  package の make が走らない。当てる前に、command line で package の source の一覧を変えても package の make は
  0 回だった。さらに package の make の中でも、link は object にだけ依る（`userland/base/package.mk:298`）ので、
  source の一覧が古い object どうしで入れ替わると link し直さない。
- 差分:
  - top の `Makefile`: `include $(CONFIGMK)` の前後の `MAKEFILE_LIST` の差で、config と config が include する
    file を `CONFIGMK_FILES` に取る。
  - `mk/userland.mk`: `$(BUILD)/.package-config`（command line の変数 `MAKEOVERRIDES` と config の file の中身。
    毎回書き、変わった時だけ置き換える）を全 package の stamp の依存に足す。config が変わると全 package の
    make が走り、各 make が要る物だけ作り直す。
  - `userland/base/package.mk`: object の一覧の stamp（`.objects`、変わった時だけ置き換える）を program・
    共有 library・静的 library の出力の依存に足す。

## 確かめた事（差分を worktree に当てて流し、元に戻した。QEMU・実機は未実施）

host は 64 thread。config は x86_64 の最小（sh init cron getty syslogd networkd mount fsck sysctl net login
dhcpc ls cat echo、`CONFIG_ROOTFS_DEVELOPMENT=n`）。

- 1: `make -j4 CONFIGMK=… BUILD=build/p4-x/ disk` が通り `build/p4-x/hdd-image.img` を作った（check PASS）。
  sysroot は `build/p4-x/sysroot`、既定の `build/x86_64/sysroot` は触らなかった。rpi5・rpi4・vf2・i386・x86_64 で
  `make -pn` の `SYSROOT` が `BUILD` の下（parse の誤り無し）。
- 2: 新しい BUILD で `make -j4 programs` が通り、package は sysroot の smoke link の後に始まった（当てる前は失敗）。
- 3: `make -j4` で NoctLang を作り直すと `cmake --build … --parallel 4`（当てる前は 16）。`-j`・`-j` 無しは
  今の既定、`-j4 BUILD_JOBS=3` は 3。
- 5: `ROOTFS_EXTRA_FILES` の net.conf が root に入り（init の net.conf に勝つ）、`/root/.ssh` は 0700、
  `authorized_keys` は 0600。同じ値の 2 回目は作り直し 0、source を touch すると root を作り直す。
  一覧の file が無い時の root は、当てる前と後で全 file の path・mode・size が同じ（72 件）。
- 6: 当てた build の `sbin` に ifconfig・route・dhcpc がある。guest で `net startup` が通る事は未実施。
- 7: command line で echo の source を足すと package の make が走り CC と LD、戻すと LD、古い object だけで
  同じ入れ替えをしても LD（.objects による）、何も変えない 2 回目は make の走りが 0。
- 未実施: makefile-style.md B.8 の「header を 1 つ変えて、それに依る object だけが作り直されるか」は、object の
  規則を変えていないので流していない。wayland の package での `ZEDBSD_TEST_SCREEN_CAPTURE` の切り替えそのもの、
  rpi5・vf2・i386 の build、QEMU の起動。

## 試験の道具（観測 4、P4 が直して commit）

- `plan/tools/guest/test-image.sh`: `CONFIGMK`・`BUILD`・`BUILD_JOBS` で make を呼ぶ。既定の target は `disk`
  （古い `disk-image` は `disk` と読む）。harness と引数の `--file`・`--mode` は、`make rootfs` の後に
  `BUILD/rootfs` へ写し、root の stamp を touch してから target を作る（今の Makefile のままで、package の
  file に勝つ）。`firmware-licenses` は target がある機種だけ。最後に `sync`。
- `plan/tools/guest/guest.py extra-files`: 古い `ZEDBSD_TEST_EXTRA_FILES='…'` でなく、`--file`・`--mode` の
  引数をそのまま 1 行で出す。それを sed で読んでいた `plan/tools/aat/build-image.sh` の 1 行も合わせた。
- `plan/tools/guest/jobs.sh`: 既定を 16 から 4 に（4 thread の規則）。
- 確かめ: 今の main の Makefile のまま `test-image.sh build/p4-min.mk build/x86_64` が通り、root の net.conf が
  harness の物、`/root/.ssh` が 0700、root の UFS に harness の鍵と net.conf が入った。差分を当てた tree でも
  `BUILD=build/p4-x/` で同じ。
- 残り（範囲の外、直していない）: 古い変数・target を使う他の試験の道具と config（`plan/tools/guest/config-amd64-ssh.mk`
  の `ZEDBSD_CONFIG`・`disk-image` の例と `ZEDBSD_USER_PROGRAMS`、`plan/tools/installer/` の
  `ZEDBSD_INSTALLER_TEST_FILES`、`plan/ws035/tests/config-amd64-userland.mk` の `ZEDBSD_*` の書式と、それを
  include する config）、`plan/tools.md` の guest.sh の行の「extra-files の出力を eval して作る」。
