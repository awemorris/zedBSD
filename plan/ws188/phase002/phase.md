<!-- awesome-plan project=zedbsd record=ws188-p002 -->
# ws188-p002: 実装: Settings の About・Storage・Users・Sharing・Welcome・Languages を libkeiland 経由に

Status: cleared（2026-10-08 Q1、T1-427 PASS）
Disposition: normal
Parent: [WS188](../ws.md)
設計: [p001](../phase001/phase.md)（第 3 版）

## 実装

| 層 | file | 内容 |
| --- | --- | --- |
| backend（共有） | `libkeiland-backend/machine/machine.c`・`filesystems.c`・`users.c`、`keiland-backend.h`・`backend-private.h` | `kl_backend_machine_read`（os-release・uname・sysconf・gethostname・x86 の cpuid、Settings の about.c から移した）、`kl_backend_filesystems_read`（statvfs、`st_dev` で重複を除く）、`kl_backend_users_posix`（getgrnam_r・getpwent・getpwuid_r、自分の行は必ず入る、名前は切らずに外す）、UTF-8 の境で切る `kl_backend_machine_copy` |
| backend（OS） | `libkeiland-backend-zedbsd/account-zedbsd.c`、`libkeiland-backend-linux/users-linux.c`、`libkeiland-backend-freebsd/users-freebsd.c`、3 つの build の list | `kl_backend_users_read`: 管理者の group は zedBSD・FreeBSD が wheel、Linux が sudo と wheel |
| protocol | `libkeiland/system/kl-system-protocol.h`・`system-protocol.c`・`.h` | manager の version 21、request 14 `get_machine`、`kl_system_machine_v1`（query、parts・about・filesystem・user・login_language・result）、能力の bit 0x8000 |
| libkeiland | `system.c`・`system-view.c`・`system-private.h`、`include/keiland/keiland.h`（KL_VERSION 69）、`exports.map`（exports.py で作り直し） | `kl_system_machine_query`・`_known`・`_serial`・`_about`・`_filesystems`・`_users`・`_login_language`。受けは parts から result まで貯め、同じ request の OK で部分ごとに写して serial を進める |
| compositor | `wayland/machine-shell.c`（新）・`machine-wait.c/.h`（新、純粋）・`language-file.c`（新、純粋）、`system.c`・`protocol.c`・`objects.c`・`kwl.h`・`language.c`、3 つの Makefile | 読みは heap の job と detach の thread、同時に 1 つ、10 秒で見捨てて FAILED、見捨てた thread が残る間は即 FAILED、close は待たない。待ちは全体 16・client 4、object の消滅で外す。答えは丸ごと入る時だけ（UNAVAILABLE）。login の言語は file の 1 行目を language-file.c で読み、en・ja・"" に写す |
| Settings | `machine.c`（新、about.c の置き換え）、`settings.h`・`system.c`・`main.c`・`look.c`・`page-look.c`・`page-users.c`・`page-users-admin.c`・`page-languages.c`・`page-sharing.c`・`welcome.c`、3 つの Makefile | 部分ごとの request・時刻・serial、失敗の後 2 秒・15 秒の上限・file system は表示中 2 秒ごと、main の poll の timeout。管理の選択は名前で持ち、読み直しの間は行の選択と Apply を止める。Languages の結果の log は読み直しの答えで出す（`… errno=0 system=K reread=E`）。statvfs・getpw*・getgr*・os-release の読みは Settings から無くなった |
| AAT | `tests/scenarios/apps/settings/{about,users-page,manage-users,pages}.md`、`plan/tools/aat/scenarios/helpers_apps.py` | 答えの log の行を待ってから撮る・確かめる（users-page は起動からの wait、manage-users は追加の後の `USERS list count`） |

## 他の WS の試験の追従（Q1 の確認が要る、別の commit）

- `plan/ws131/tests/host-system.sh`・`host-system.c`: compositor の system.c が machine-shell を呼ぶので link に machine の file を足し、能力の期待に `KL_SYSTEM_HAS_MACHINE`、端から端の machine の段（4 部分の答え、file system だけの 2 回目で serial、不正な部分の拒否）を足した。
- `plan/ws089/tests/host-kl-system.c`（machine の stand-in、提供しない）・`host-render.c`（`se_about_read` の代わりに固定の名前）。

## 確認（2026-10-08、P2）

| 確認 | 結果 |
| --- | --- |
| zedBSD の build: `make -j16 BUILD=build/ws188-p002 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk build/ws188-p002/dynamic/libkeiland.so build/ws188-p002/bin/wayland build/ws188-p002/bin/settings` | rc 0、warning 0（-Werror） |
| Linux の build: `make -j16 -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws188-p002-linux all` | rc 0、warning 0（-Werror） |
| `sh plan/ws188/tests/host-machine.sh`（ASan・UBSan） | 81 checks passed（os-release の 13 case、UTF-8 の切り、host の名前・file system・account、待ちの表、view の受け） |
| `sh plan/ws131/tests/host-system.sh build/ws188-p002-host/host-system`（ASan・UBSan、libkeiland ↔ compositor ↔ host の backend） | PASS（machine の答え users=1 filesystems=3） |
| WS089 の host の Settings（host-build.sh の about.c を除いた写し）、settings-render の about・users・storage・languages・sharing・home | build warning 0、6 頁 rc 0、About の memory の行は今まで通り |
| `plan/tools/keiland-os-boundary/check.sh`（`BUILD=build/ws188-p002 KEILAND_LINUX_BUILD=build/ws188-p002-linux`） | C1・L1・M1・B2 ほか PASS。FAIL は既存の B3（sessiond・printd）と B5（ws177 の試験の `<browser.h>`、P1 の merge 由来）だけ |
| FreeBSD の build | 未実施（guest が要る。T1 の依頼に含める） |
| QEMU・実機 | 未実施（T1 の依頼） |

## 残り

- 削除の依頼（Q1）: `userland/desktop/settings/about.c`（machine.c と backend へ移した）、`plan/ws089/tests/host-about.sh`・`host-about.c`（case は plan/ws188/tests/host-machine.c へ移した）、`plan/ws089/tests/host-build-ws188tmp.sh`（P2 が確認のために誤って tree に作った写し）。
- about.c が消えるまで `plan/ws089/tests/host-build.sh` は about.c の compile で止まる（消えれば通る）。
- T1: AAT の apps.settings.about・users-page・login-language・manage-users・pages、FreeBSD の keiland-freebsd.mk の build。


## Q1 の判定（2026-10-08）

T1-427 PASS（標準 AAT image、tree dba0f7130）: about・login-language・single-instance・files.open-from-home が pass、users-page・manage-users・pages は needs-person（PNG を Q1 が目視、Storage の頁は disk の card が出て「Reading the disks...」でない）。`KWL SYSTEM machine answer ... result=0`。cleared。