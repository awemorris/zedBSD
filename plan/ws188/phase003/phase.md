<!-- awesome-plan project=zedbsd record=ws188-p003 -->
# ws188-p003: 境界の検査の強化（app と libkeiland の OS の操作）

Status: cleared（2026-10-08 Q1: B3 の直し ebca7b0dd と p004 の後、check.sh が main c671551fe で PASS。残る PENDING は audiod の 3 行で WS191 の範囲）
Disposition: normal
Parent: [WS188](../ws.md)

## 実装

`plan/tools/keiland-os-boundary/check.sh` に A1〜A5 を足し、許可の表 `plan/tools/keiland-os-boundary/app-allow.tsv`（検査・source（file か / で終わる directory）・名前（* は全て）・理由）を新しく置いた。

| 検査 | 見る物（app と libkeiland: `userland/desktop/` の下で、compositor・libkeiland-backend*・sessiond・printd・xserver・graphics と互換の library・include・fonts などを除いた全て） |
| --- | --- |
| A1 | 文字列の literal: `/dev` `/proc` `/sys` `/run` `/var` `/etc` で始まる物、`.sock` で終わる物、daemon の名前（netd・audiod・volumed・bluetoothd・sessiond・printd・wpa_supplicant）。許可の表の行の欠け（理由の無い行）も A1 の FAIL |
| A2 | local socket: `sockaddr_un`・`AF_UNIX`・`AF_LOCAL`・`PF_UNIX`・`PF_LOCAL` |
| A3 | `getpw*`・`getgr*`・`setpwent` など、`statvfs`・`statfs`・`getfsstat`・`getmntinfo`・`getmntent`・`setmntent`、`sysctl*` の呼び出し |
| A4 | `fork`・`vfork`・`exec*`・`posix_spawn*`・`system`・`popen` の呼び出し |
| A5 | `<uapi/…>`・`<linux/…>`・`<dev/…>`・`<sys/sysctl.h>`・`<mntent.h>`・`"userland/base/…"` の include |

comment は除き、literal は引用符の中だけを見る（python3、check.sh の中）。

許可の表（理由つき）: preview の sandbox（ユーザーの決定）、libkeiland/instance.c の AF_UNIX（単一の起動）、libkeiland/settings.c と Files の getpwuid・getgrgid（Q1 2026-10-08）、settings/main.c・pdfviewer/main.c の posix_spawn・files/apps.c・terminal/main.c の起動（app の機能、F-085）、Files の places.c・search.c・trash.c の system の tree の除外の literal と apps.c の "/dev/null"。**判断待ち（PENDING の行）**: Files の mount の表（files/mntent/mounts-mntent.c・files/freebsd/mounts-freebsd.c）、再生の音の audiod への直の接続（videoplayer/audio.c、music と libmedia も使う）。

## 確認（2026-10-08、P2）

| 確認 | 結果 |
| --- | --- |
| `sh plan/tools/keiland-os-boundary/check.sh` | A1〜A5 PASS。FAIL は既存の B3 の 2 行だけ（下の案） |
| 検査が違反を捕まえること: scratchpad に ws188 の前の Settings（a03b9e47c の look.c・page-users.c・about.c・welcome.c・page-languages.c）と合成の file を置き、A の python だけを流した | statvfs・getpwuid・setpwent・getpwent・endpwent・getgrnam・"/etc/os-release"・"/var"、合成の "/run/bluetoothd.sock"・sysctl・system・`<sys/sysctl.h>` を報告、comment の中の fork は報告しない |

## B3 の既存の FAIL の案（Q1 へ）

- `userland/desktop/printd/Makefile:3`: comment の行が `libkeiland-backend` の語に当たっている。案: B3 の Makefile の走査で `#` で始まる行を見ない。
- `userland/desktop/sessiond/Makefile:16`: sessiond（system の session manager）が `userland/base/net/protocol.c` を link する（sleep.c が net の protocol を使う）。sessiond は app でも compositor でもない system の service。案: B3 の `userland/base/net/` の規則から sessiond を外す（B3 の目的は「backend を使うのは compositor だけ」で、sessiond の network の利用は backend の使用ではない）。

## 残り

- 判断待ちの 2 つが決まったら許可の表を直す（移すなら別の Phase・WS）。
- master.md の Tools 節の check.sh の行に A1〜A5 と app-allow.tsv を足す（Q1）。
