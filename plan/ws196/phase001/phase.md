<!-- awesome-plan project=zedbsd record=ws196-p001 -->
# ws196-p001: useradd・usermod・userdel の範囲と、account-admin と共有する code の置き場

Status: planning（2026-10-09 夜 P1 が調べと設計の案を書いた。E1〜E6 のユーザーの判断と design-reviewer の review の後に cleared の候補。code は変えていない）
Disposition: normal
Parent: [WS196](../ws.md)
Queue: Q1 の投入（2026-10-09 夜、設計だけ。実装はベータ3 以降）
依存: [WS195 p001](../../ws195/phase001/phase.md) の D7（account-admin の source の置き場）

## 1. 事実（2026-10-09 の tree）

- base に在る利用者の道具: `passwd`（`passwd [-s] [user]`、root は他の人の password を変えられる）、`id`、`newgrp`、`su`、`sudo`。useradd・usermod・userdel・groupadd などは無い。POSIX（XCU）にも利用者の管理の utility は無い（passwd も無い）。
- account-admin（`userland/base/account-admin/`、ws089-p026、`docs/architecture/security.md`「Account administration」）が Settings の利用者の追加・削除・password の reset・wheel の出し入れ・system の言語をする。層は 3 つ:
  1. **純粋な編集**（`edit.c` 1025 行・`edit.h`、file に触らない）: request の解析、名前・表示名・言語の規則、passwd・group・shadow の文の field の読み、空いている ID（`ADMIN_ID_FIRST` 1000〜`ADMIN_ID_LAST` 59999）、行の削除と追加、group の member の出し入れ、wheel の判定、管理者の数。host 試験 `plan/ws089/tests/run-host-account-admin.sh`（`host-account-admin.c`）・`plan/ws158/tests/run-host-admin-language.sh`。
  2. **file の取引と付随の作業**（`main.c` の static 関数）: `account_files_lock`（`common/account.c`）の下で 3 つの file を読み、検べ、group → passwd → shadow（追加）・逆順（削除）で 1 つずつ atomic に置き換え（`account_file_write`）、止める signal をその間抑える。home の作成と `/etc/skel` の複写（`admin_make_home`・`admin_copy_skeleton`）、home の tree の削除（`admin_remove_tree`）、`/etc/passkey` の行の削除（`admin_passkey_forget`）、走っている process の有無（`admin_busy`）、syslog（auth）への記録。
  3. **方針**（main.c）: 呼び手（setuid の実の uid、root でない）、wheel の人だけ、呼び手の password、root と system の account（uid < 1000）は対象外、自分の削除と自分の wheel の外しは不可、最後の管理者は消せない、process の残る人は消せない。
- 追加の既定: home `/home/NAME`、shell `/bin/sh`、private group（名前と同じ、gid = uid）、wheel は管理者の時だけ、shadow の行 `NAME:HASH:DAY:0:99999:7:::`、gecos は表示名。
- `common/account.c`（699 行）は passwd・su・sudo・account-admin が共有する: password の規則と hash、shadow の行の置き換え、wheel の判定、file の lock・読み・atomic な書き。
- release の image では root の password は lock（U10）。管理は wheel の人の sudo。最後の wheel の人を消すと管理できる人が居なくなる。

## 2. 範囲の案（Linux の shadow-utils の option の慣習に合わせ、使う物だけ）

| 命令 | option（推しの範囲） | 入れない物（理由） |
| --- | --- | --- |
| `useradd [-c COMMENT] [-d HOME] [-g GROUP] [-G GROUP,…] [-m \| -M] [-s SHELL] [-u UID] NAME` | 既定は account-admin と同じ（home `/home/NAME`、`-m` が既定で `/etc/skel` を複写、shell `/bin/sh`、private group、uid は 1000〜59999 の空き）。password は lock（shadow の `!`）で作り、`passwd NAME` で付ける | `-p`（hash を command line に出す）、`-D`（既定の file）、`-e`・`-f`（期限）、`-o`（重複の uid）、`-k`・`-K`、`-r`（system の account、E6） |
| `usermod [-c COMMENT] [-d HOME [-m]] [-g GROUP] [-G GROUP,… [-a]] [-s SHELL] [-L \| -U] NAME` | `-a -G wheel` で管理者に。`-L`・`-U` は shadow の hash の前の `!` の付け外し | `-l`（名前の変更: home・passkey・group の全部に及ぶ、後で）、`-u`（file の持ち主の付け替えが要る）、`-e`・`-f`・`-p` |
| `userdel [-r] [-f] NAME` | `-r` で home を消す。`-f` は process が残っていても消す（E1 の保護は外さない） | `-Z` など |

- 場所は `/sbin/useradd` などの base の program（mode 0755）。root でない呼び手は EPERM（setuid にしない）。
- 終わりの状態は shadow-utils に合わせる（0 成功、1 使い方の誤り以外の失敗、2 使い方。細かい番号は p002 の詳細設計で）。全ての変更を syslog の auth に記録（password・hash は書かない）。
- groupadd・groupmod・groupdel は入れない（wheel の出し入れは `usermod -a -G wheel` と `gpasswd` 相当が無くても済む）。要るなら Future Work（E3）。

## 3. 共有の code の置き場の案

| 層 | 今 | 案 |
| --- | --- | --- |
| 1 純粋な編集 | `account-admin/edit.c`・`edit.h`（`admin_*`） | `userland/base/common/account-edit.c`・`.h` へ移す（関数名の prefix は `account_edit_*` に揃えるか今の `admin_*` を保つかは p002 で、host 試験は一緒に移す） |
| 2 取引と付随の作業 | `account-admin/main.c` の static | `userland/base/common/account-change.c`・`.h`（新）: 3 つの file を lock の下で読む・書く取引、home の作成と削除、`/etc/passkey` の掃除、process の有無、syslog |
| 3 方針 | `account-admin/main.c` | 共通の保護（E1）だけ `account-change` に置き、account-admin の呼び手の検べ（wheel・password）は account-admin に残す。useradd などの呼び手の検べは「root であること」 |

- account-admin（WS195 で Keiland の zedBSD の backend の tree へ、D7 (a)）は base の `common/` の source を compile する。境界の規則で OS の backend の tree は自分の OS の code を使ってよいので、今の `common/account.c`・`login/verify.c` と同じ扱いでよい。D7 が (b)（desktop の直下）になったら、境界の checker（C3）で許されるかを WS195 p002 で確かめる。
- 移す順: WS196 の実装（共有の層の切り出し）を WS195 の account-admin の移動より先にすると、移動は Makefile と path だけになる。逆なら移動の後に切り出す。どちらでも account-admin の振る舞いは変えず、`run-host-account-admin.sh` を両方の前後で流す。

## 4. ユーザーに聞く判断（推しの案つき）

| ID | 問い | 案 | 推し（理由） |
| --- | --- | --- | --- |
| E1 | root が端末で使う時も account-admin の保護（root と uid < 1000 の account を消さない・変えない、最後の wheel の人を消さない・wheel から外さない）を当てるか | (a) 当てる（override の option も無し） (b) shadow-utils と同じく root は何でもできる | (a): release では root が lock（U10）で、最後の wheel の人が居なくなると誰も管理できない。system の account（`_bluetooth`・`_greeter` など）は image が作る物で、消すと daemon が起動しない |
| E2 | option の範囲 | (a) §2 の範囲 (b) shadow-utils のほぼ全部 | (a): Settings と同じ操作を端末でできる最小。`-p` は hash を process の一覧に出すので入れない |
| E3 | groupadd・groupmod・groupdel も作るか | (a) 作らない（Future Work） (b) 同じ WS で作る | (a): 今は private group と wheel だけで、group の管理の必要が見えていない |
| E4 | 共有の code の置き場 | (a) §3 の `userland/base/common/account-edit`・`account-change` (b) account-admin の中に置き、useradd などが account-admin の source を compile する | (a): base の道具が Keiland の tree の source に依存しない（WS195 で account-admin が Keiland に移るため） |
| E5 | useradd の後の password | (a) lock で作り `passwd NAME` で付ける（Linux の既定） (b) useradd が対話で聞く | (a): 道具の役割を分け、script からも使える |
| E6 | system の account（`-r`、uid < 1000、home 無し）を作れるようにするか | (a) 入れない（system の account は image の `/etc/passwd` が持つ） (b) 入れる | (a): 今 system の account を足すのは image を作る時だけで、E1 の保護とも合う |

## 5. Phase の案（実装はベータ3 以降、Q1 の割当）

| 案 | 内容 | 確かめ |
| --- | --- | --- |
| p002 | 共有の層の切り出し（E4）。account-admin の振る舞いは変えない | `run-host-account-admin.sh`・`run-host-admin-language.sh` の前後で同じ PASS、build warning 0 |
| p003 | useradd・usermod・userdel（§2）と host 試験（tmp の root に置いた passwd・group・shadow・skel で、追加・変更・削除・E1 の拒否・lock の取り合い） | host 試験、build |
| p004 | T1: guest で root（または sudo）から 3 つの命令、Settings の Users の頁が同じ account を見ること、login | T1 の依頼 |
| p005 | 規約の全文の見直し | — |

## 6. 未確かめ・限界

- `passwd` の root の時の振る舞い（他の人の lock の付け外しの有無）は usage の行だけ読んだ。`usermod -L/-U` と重なるかは p002 で読む。
- design-reviewer の review は未実施（ユーザーの判断の後に通す）。

## ユーザーの決定（2026-10-09）

E1〜E6 はクリック「全部推しどおり」（各 (a)）。10/17 の後に design-reviewer を通して p002 から。
