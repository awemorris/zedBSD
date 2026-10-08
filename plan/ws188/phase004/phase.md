<!-- awesome-plan project=zedbsd record=ws188-p004 -->
# ws188-p004: Files の mount の一覧を kl_system_machine の要求へ

Status: cleared（2026-10-08 Q1: T1-429 host-model PASS、T1-430 QEMU の USB の mount・Trash・Empty Trash PASS（PNG を Q1 が目視）、T1-431 FreeBSD の build PASS。取り出しの後の Devices の消え方は未実施）
Disposition: normal
Parent: [WS188](../ws.md)
由来: ユーザーの決定（2026-10-08 昼）「Files の mount の表は kl_system に移す」。p003 の走査で見つかった Files の `files/mntent/mounts-mntent.c`（getmntent、zedBSD と Linux）・`files/freebsd/mounts-freebsd.c`（getfsstat）。使う所は Places の mount の volume（places.c の `places_mounts`）と Trash の volume ごとの `.Trash` の発見（trash.c の `fm_trash_list`）。

## 設計

### wire（`kl_system_machine_v1` の 5 つ目の部分）

- 部分 `KL_SYSTEM_MACHINE_MOUNTS 0x10`、event 6 `mount(string path, string type)`（path は 256、type は 32、NUL を含む）。答えの中の mount は全体（前の list を置き換える）。
- manager の version **22**（`KL_SYSTEM_SINCE_MOUNTS 22`、main の次の空き、merge の時に Q1 が確かめる）。machine の object は manager の version で作られ、version 21 の object の query に MOUNTS の bit があれば `INVALID`（今の未知の bit と同じ）。libkeiland は manager が 22 未満なら MOUNTS を頼まない（`kl_system_machine_query` は EINVAL ではなく ENOTSUP を返す）。
- 1 つの答えの mount は最大 64。答えの最大は約 31 KB（users）＋約 20 KB（mounts）で、`KWL_OUTPUT_MAX`（1 MiB）に丸ごと入る判定は今のまま。

### backend

- `struct kl_backend_mount { char path[256]; char type[32]; }`、`size_t kl_backend_mounts_read(struct kl_backend_mount *list, size_t capacity, unsigned *skipped)`。
- **利用者の file を置ける mount だけ**を返す: 仮想の file system の種類（tmpfs・devfs・proc・sysfs・devpts・cgroup ほか、今の Files の `places_hidden_types` の表を backend の共有の表に移す）は外す。Linux は snap の squashfs などで mount が 100 を超えうるので、ここで外して 64 に収める。入らない分と長すぎる path は外し、`skipped` に数える（compositor の log に出す）。
- 置き場所: zedBSD と Linux は共有の `libkeiland-backend/machine/mounts-mntent.c`（`setmntent(MOUNTED)`・`getmntent`、今の Files の物を移す）、FreeBSD は `libkeiland-backend-freebsd/mounts-freebsd.c`（`getfsstat(MNT_NOWAIT)`）。仮想の種類の表と選別は共有の `libkeiland-backend/machine/mounts.c`（`kl_backend_mounts_keep(type)`、`backend-private.h`）。
- compositor の machine の thread が読む（network の mount で止まりうる getfsstat も thread の上）。

### libkeiland（KL_VERSION 71 を仮に。WS189 が 70 を仮に取っている）

- `KL_MACHINE_MOUNTS 0x10`、`KL_MACHINE_MOUNTS_MAX 64`、`struct kl_machine_mount { char path[256]; char type[32]; }`、`size_t kl_system_machine_mounts(const struct kl_system *system, struct kl_machine_mount *list, size_t capacity)`。`kl_system_machine_serial(system, KL_MACHINE_MOUNTS)`。
- view に 5 つ目の slot（pending と effect の 64 個、約 37 KB を足す）。

### Files

- `files/mntent/mounts-mntent.c` と `files/freebsd/mounts-freebsd.c` をやめ（Q1 に削除を依頼）、`files/mounts.c`（新、OS に依らない）が desktop の最後の答えの写しを持つ。`fm_mounts_set(list, count)` が写しを置き換え、`fm_mounts_open`・`_next`・`_close`（`mounts.h` の口は変えない）は open の時の写しの複製を巡る。答えの前は空（今の「table が読めない」と同じ扱い: Places に volume が出ない、Trash は home の物だけ）。
- `files/main.c`: 起動の時と `KL_SYSTEM_CHANGED_DEVICES`（media の mount・eject）の時に `query(MOUNTS)`、`KL_SYSTEM_CHANGED_MACHINE` で MOUNTS の serial が変わったら `fm_mounts_set` → `fm_places_init` → 描き直し。Trash の tab が開いていればその listing も読み直す。
- `places.c` の `places_hidden_types` の選別は backend へ移るので外す（system の folder の接頭辞と Devices の mount の除外は Files の Places の方針として残す）。
- Image Viewer（`imageview`）は `files/trash.c` を link するが `fm_trash_list` を使わない: link の list の `files/mntent/mounts-mntent.c` を `files/mounts.c` に替える（写しは空のまま）。`plan/ws128/tests/host-share.sh` も同じ。
- 許可の表 `app-allow.tsv` の mount の PENDING の 3 行を外す（audiod の行は WS191 まで残す）。

### 試験

- host: `plan/ws188/tests/host-machine.c` に mount の選別（仮想の種類を外す）と host の `/` の mount、view の 5 つ目の部分、`files/mounts.c` の写しと巡回。`plan/ws131/tests/host-system.c` の machine の段に MOUNTS。
- build: zedBSD の libkeiland.so・wayland・files・imageview・settings、keiland-linux all（warning 0）。
- T1: Files の Places に USB の volume（media）以外の mount の volume が今と同じに出る、Trash の一覧、AAT の Files の places・trash の scenario。

## 実装（2026-10-08、P2）

設計どおり。追加: `files/mounts.h` に `fm_mounts_set`、Files の main は `KL_SYSTEM_CHANGED_DEVICES` の後にも読み直す、Trash の tab が開いていれば読み直す。`plan/tools/files/host-build.sh` は `files/mntent/*.c` を外した（mounts は desktop の答え、host では空）。`plan/ws128/tests/host-share.sh` は `files/mounts.c` を link。

| 確認 | 結果 |
| --- | --- |
| zedBSD: libkeiland.so・wayland・files・imageview・settings（BUILD=build/ws188-p002） | rc 0、warning 0 |
| keiland-linux all | rc 0、warning 0 |
| `sh plan/ws188/tests/host-machine.sh` | 101 checks passed（mount の選別、host の mount の表に `/`、view の 5 つ目、Files の写しと巡回） |
| `sh plan/ws131/tests/host-system.sh` | PASS（`what=31`、host の mount 7） |
| `sh plan/tools/files/host-build.sh`、`sh plan/ws128/tests/host-share.sh` | rc 0、PASS |
| `check.sh` | A3・A5 の FAIL は削除待ちの `files/mntent/mounts-mntent.c`・`files/freebsd/mounts-freebsd.c` だけ（消えれば PASS）。他は PASS |
| FreeBSD の build、QEMU | 未実施（T1） |

削除の依頼（Q1）: `userland/desktop/files/mntent/mounts-mntent.c`（と directory）、`userland/desktop/files/freebsd/mounts-freebsd.c`（と directory）。

## design-reviewer（2026-10-08）と第 2 版

レビュー: blocker 1・major 3・minor 8。反映:
- **B1**: backend が tmpfs・overlay を外すと利用者の tmpfs の Trash（ws127-p003、host-model の 8b）と zedBSD の `/`（overlay）が消える → backend は**中身を持たない疑似の file system だけ**を外す（proc・procfs・sysfs・devfs・devpts・kernfs・fdesc・fdescfs・linprocfs・linsysfs・swap・cgroup・cgroup2・nsfs・mqueue・mqueuefs・tracefs・debugfs・securityfs・pstore・bpf・efivarfs・configfs・fusectl・binfmt_misc・rpc_pipefs・hugetlbfs・autofs）。tmpfs・overlay・squashfs などは Files の Places の表示の方針（`places_hidden_types`、元に戻した）で隠す。上限 64 で落ちる物が利用者の mount にならないよう、system の木（/dev・/proc・/sys・/run・/snap・/var/lib）の下の mount を後に並べる（table を 2 回読む、FreeBSD は snapshot を 2 回巡る）。
- **M1**: `plan/tools/files/host-build.sh` の `$src/mntent/*.c` を外した。`plan/tools/files/host-model.c` の 8b は `fm_mounts_set` で利用者の tmpfs を desktop の答えとして渡す。
- **M2**: 古い一覧: `files/mounts.c` の walk（Places の作り直し・Trash の読み）が 2 秒より古い写しを見たら読み直しを頼む（`fm_mounts_wanted`、main が query）。答えの walk 自身は頼まない（新しいので）。eject の直後は答えが来るまでの短い間だけ古い写し。
- **M3**: MOUNTS は単独の query、同時に 1 つ、待つ間の要求は印にして答えの後に出す。FAILED・BUSY・ENOTSUP は前の写しを残す。Today の `main_home_free_take` は FILESYSTEMS の serial が変わった時だけ。
- m1: `kl_backend_mounts_read` は 0／errno を返し、読めなければ compositor は MOUNTS を頼んだ query に FAILED（Files は前の写しを残す）。m3: 種類の表は上に書いた（mqueuefs を含む）。m5: Keiland でない・古い compositor では Places に volume が出ず Trash は home の物だけ（境界の規則による後退）。m6: exports.map は exports.py で作り直し済み。m7: FILESYSTEMS と同じ読みに相乗りすると statvfs の止まりで MOUNTS も失敗しうる（受け入れ、Files は MOUNTS を単独で頼む）。m8: getmntent は machine の thread だけ（machine/mounts-mntent.c の注記）。m4: T1 の依頼は USB の fat の volume の Trash の流れ（下）。

再確認（第 2 版）: zedBSD build（libkeiland.so・wayland・files・imageview・settings）rc 0 warning 0、keiland-linux all rc 0 warning 0、host-machine 118 PASS、ws131 host-system PASS（mounts=17）、host-share PASS、files の host-build rc 0（host-model.sh は sudo の mount と test の中の削除を含むので P2 は流していない → T1／Q1）。check.sh は削除待ちの 2 file の A3・A5 だけ FAIL。

T1 の依頼の案: (1) `sh plan/tools/files/host-model.sh`（host、8b の volume trash）、(2) QEMU: AAT の apps.files.mount-usb の流れで USB の fat を mount → その上の file を Trash へ → Trash の tab に出る → Empty Trash、Files の log `FILES MOUNTS count=N`、Places に USB は Devices に出て Locations に重ならない、(3) FreeBSD の build。
