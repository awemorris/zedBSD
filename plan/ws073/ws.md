<!-- awesome-plan project=zedbsd record=ws073 -->

# WS073: Bug Board の掃討（bug sweep）

<!-- awesome-plan-current:start -->
Status: completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.3）で閉じた。残りの uncleared の p025・p033・p040・p045 と p032 の実機の確認は Bug Board の各 ticket に任せる。旧: incomplete）
Primary Milestone: MG002
Related Milestones: MG004, MG006
Objectives: O1
Parent: [Master](../master.md)
Executor: WS073 のサブエージェント（2026-09-29 から worktree `.claude/worktrees/ws073-bugs`、branch `wt/ws073`。以前は `worktree-agent-a4f5b29b09938aa63`、p001・p002 は `worktree-agent-aefedcaf4a52a0507`）。main が merge する
Resume point: 2026-09-30（p044、P1）: BUG-123 を ws073-p044 で修正（libwayland-client の dispatch を標準と同じ 1 回の読みに）。以下は前の記録: 2026-09-30（p042）: ws073-p042（BUG-116）で原因を特定して修正（command の poll の間 IE を落とさない）。少数回の再現の試験と boot test は phase.md、受け入れの試験（TCG・KVM 各 2×20、80 回で cancel・attach-failed・error・列挙の再試行 0、boot test PASS、serial の login 可）で cleared、BUG-116 を resolved。BUG-030 の p041 の未達（KVM の 1 回）もこの機構で、p042 の後の 80 回で usb-storage の error 0（BUG-030 の扱いは main の判断）。その前: ws073-p041（BUG-030）で原因を特定して修正（usb-storage の BOT の段の timeout を 30 秒・flush 60 秒に）。受け入れの試験（2026-09-30）で flush の待ちは 0 だが、KVM の 1 回で usb-storage が BUG-116 の機構で時間切れになり p041 は uncleared（扱いは main の判断）。その前、2026-09-29 夜: ws073-p040（BUG-030）は wrap up で uncleared（再現の道具 `tests/usb-stress.sh`）。2026-09-29: BUG-102（p029）・BUG-104（p031）・BUG-051（p030）・BUG-107（p034）・BUG-108（p035）を解決、BUG-031 を resolved・BUG-039 は確認のみ（p036）。次は main の指示を待つ（残り: BUG-036・041 の再現、BUG-039 は host 試験の土台の WS、BUG-093 は toolchain、BUG-027・033 は低い優先度）
2026-10-03: 新 [p045](phase045/phase.md)（BUG-135）。
2026-10-04: 新 [p051](phase051/phase.md)（BUG-135 の残り、P9、q653。journal の commit を閉じる段と書いて flush する段に分け、flush を `ms->lock` の外へ。T1・T2 への試験の依頼はしない（user））。
2026-10-06: [p055](phase055/phase.md) / BUG-202は通常boot_workerへのdevice初期化移動とamd64 buildまでcleared（source e093bebe、user指定）。同日user「マージしてください。」でmainへ統合、[q779履歴](../history/queue-q779.md)に結果。実機UAT待ち、Bug tracking。
<!-- awesome-plan-current:end -->

## 完了（2026-10-08、Q1 の判定）

- この WS は Bug Board の掃討の作業の入れ物で、各 bug の状態の正本は [Bug Board](../known-bugs.md) と ticket。閉じた時点で uncleared の Phase: p025（BUG-087、patch は `plan/bugs/BUG-087-wip.patch`）、p033・p040（再現・原因が未確定）、p045（BUG-135、続きは p051）。in-progress の p032 は素の 5330 での確認待ち。これらは ticket の tracking のまま。
- Phase の directory と `tests/` は Bug の ticket（約 50 件）と `plan/tools/guest/build-full-image.sh`（`tests/kernel-image.sh`）・`include/kern/buf-unjournaled.h`（`tests/host`）から参照されている。完了の規則の削除は、参照の付け替え（再現の script は `plan/tools/` へ）を Q1 が決めてから（下の表と Phase の記録はそれまで残す）。


## 目標

2026-09-27 ユーザー「バグリストに載っているものを解決するサブエージェントを1つ追加しましょう。」
[Bug Board](../known-bugs.md) の open な bug（他の agent の担当を除く）を 1 件ずつ、再現 → 原因 → 修正 → 絞った試験 → ticket の更新で片付ける。
再現できない bug は調べた範囲と証拠を記録して tracking のまま残す。検証の無い修正を主張しない。

担当外: BUG-059・BUG-060（WS072）、BUG-046（WS056、ユーザーの判断待ち）、BUG-050（WS001 の POSIX agent。未修正なら main に確かめてから）、
BUG-027・BUG-033（性能。単独の計測を後で）。

## 規則と道具

- AGENTS.md の規則の全て。commit は `git commit -m WIP -- <paths>`、push しない、集約の `make check` を走らせない。
- 起動の確認は `plan/tools/boot-test.sh` だけ。QEMU の serial・console の log で判定しない。
- 新しいコードは [coding-style.md](../coding-style.md) の全文（`python3 plan/tools/style-check.py`: 新しい file 0、既存の file は悪化させない）。
- HAL の API（`include/hal/hal.h`・`include/hal/arch/*.h`・HAL の責務）の変更は適用せず `plan/ws073/proposed/` に置いて main へ。
- 道具: [tests/kernel-image.sh](tests/kernel-image.sh)（main の測定用 guest image `build/ws053-full-hal-guest` の ESP の vmunix だけを差し替える。clang・sshd がある）、
  [tests/g.sh](tests/g.sh)（`GUEST_RUNTIME=build/ws073-run` で `plan/tools/guest/guest.py` を呼ぶ）、
  [tests/style-diff.py](tests/style-diff.py)（変えた行だけの style-check。既存の file の「悪化させない」の確認）。

## Phase 一覧

| Phase | Bug | 内容 | Status |
| --- | --- | --- | --- |
| [ws073-p001](phase001/phase.md) | BUG-061 | devfs の `/dev/fd/N`・`/dev/stdin` を lstat・readlink・readdir で symbolic link に | cleared |
| [ws073-p002](phase002/phase.md) | BUG-065 | 同じ block device の 2 度目の mount を EBUSY に（優先度 高） | cleared |
| [ws073-p003](phase003/phase.md) | BUG-062 | i386 pcat の vmunix の `sched.c` の -Watomic-alignment（main の ws036-p021 の修正を build で確認） | cleared |
| [ws073-p004](phase004/phase.md) | BUG-063 | `truncate -s N` が無い file を作る、`mount -o rw` を受ける | cleared |
| [ws073-p005](phase005/phase.md) | BUG-028 | 閉じた port への connect を ECONNREFUSED に、自分の interface の address への packet を lo0 で届ける | cleared |
| [ws073-p006](phase006/phase.md) | BUG-067 | devfs の文字 device の node が chmod・chown を受ける（`mesg n`） | cleared |
| [ws073-p007](phase007/phase.md) | BUG-068 | 多 thread の process の execve が、joiner に先に reap された兄弟を待ち続ける | cleared |
| [ws073-p008](phase008/phase.md) | BUG-069 | 端末と pty の読み書きが waitq_sleep の EAGAIN を失敗として返す（console の POSIX-R2 10 回連続 status 0） | cleared |
| [ws073-p009](phase009/phase.md) | — | kernel の boot の FAT を公開する: BOOT を /boot、ESP を /boot/esp（ユーザーの判断 2026-09-27） | cleared |
| [ws073-p010](phase010/phase.md) | BUG-071 | FAT に普通の道具で file を作れる（mount の見せる mode で見せる） | cleared |
| [ws073-p012](phase012/phase.md) | BUG-072 | FAT32 の metadata を仕様どおりに（`..`、FSInfo、日時）、boot の FAT の sync | cleared |
| [ws073-p013](phase013/phase.md) | BUG-029（BUG-052 の node） | 多くの tmpfs の file が system 全体の inode を尽くさない（heap の inode、cache 16384、tmpfs の共有の上限） | cleared |
| [ws073-p011](phase011/phase.md) | BUG-070 | USB HID の keyboard が keypad・Num Lock・Print Screen・日本語の key などを出す | cleared |
| [ws073-p014](phase014/phase.md) | — | amd64 は /boot・/boot/esp を自動で見せず fstab に任せる。fstab の ESP の mount は kernel の hold を adopt する | cleared |
| [ws073-p015](phase015/phase.md) | BUG-073 | 最終の layout: `bootN:` の file を持つ boot の slot を /boot/boot0〜3 に自動で mount（amd64 UEFI も、起動後の `swapon bootN:` も）、ESP は fstab、使用中の file は読めるが書けない。claim のある FAT の line の書き戻し（BUG-073）を修正 | cleared |
| [ws073-p018](phase018/phase.md) | BUG-026 | 共有の file の mapping への store が munmap・exit で dirty にならず書き戻されない（ld.lld の出力が disk で空）を修正 | cleared |
| [ws073-p017](phase017/phase.md) | BUG-036・BUG-031（030・041） | 起動時の USB の root port の列挙を ETIMEDOUT・EIO で 3 回まで再試行、kernel の console への写しを record 単位で排他、間欠の bug の再現の試み | cleared |
| [ws073-p016](phase016/phase.md) | BUG-074・BUG-076 | FAT の readdir の位置を record の番号にし走査中の unlink で entry を飛ばさない（`rm -r`）。FAT の inode の pool が満ちたら cache だけの inode を追い出す | cleared |
| [ws073-p019](phase019/phase.md) | BUG-079 | libc の setenv・putenv が既存の変数の置き換えで後ろの変数を落とす（App Home の app が zdesktop の環境を失う）を修正 | cleared |
| [ws073-p020](phase020/phase.md) | BUG-075・BUG-082 | 読んだ後に消した file の storage を unlink で返す。sync と終了の競合（BUG-082）: wip.patch に加え、sync が読みの fill を i_io_lock を持って待つ待ち合いと、amd64 HAL の `running_task` の per-CPU の 2 回の load（fork が他の thread の frame を写す）を修正。bug082.sh 300/300、bug075.sh 14/14 | cleared（2 回目の試行、2026-09-28） |
| [ws073-p021](phase021/phase.md) | BUG-084 | header の改名の直後の image の build で rootfs が消えた header を複写する（clang の package の header の一覧を sysroot の manifest から） | cleared（2026-09-28） |
| [ws073-p022](phase022/phase.md) | BUG-086 | block・ignore された同期の fault の signal を既定の動作（終了）に強いる（thread 宛て、Linux と同じ） | cleared（2026-09-28） |
| [ws073-p023](phase023/phase.md) | BUG-088 | amd64 の `AMD64_CURRENT_SPACE` の 2 回の load の監査: 使う所は全て割り込み禁止の中で preempt されない、修正不要 | cleared（2026-09-28） |
| [ws073-p024](phase024/phase.md) | BUG-088 | 予防（main の決定）: `current_space` を 1 回の `%gs` 相対の load・store に（HAL の実装、hal.h 不変） | cleared（2026-09-28） |
| [ws073-p025](phase025/phase.md) | BUG-087 | 新しい build の最初の image に clang の resource の header が入らない: 原因を確認、修正は `plan/bugs/BUG-087-wip.patch`（未検証） | uncleared（2026-09-28） |
| [ws073-p026](phase026/phase.md) | BUG-090 | libc の qsort・qsort_r を introsort（O(n log n)、word 単位の交換）、heapsort を本物の heapsort、mergesort を安定な merge sort に（新しい `src/libc/sort.c`）。host 試験 plain・ASan・UBSan PASS、guest で n=20000 の random 1819 ms → 11 ms | cleared（2026-09-28） |
| [ws073-p027](phase027/phase.md) | BUG-089 | 新しい worktree の build が host の LLVM を作り直し共有の build/llvm に書きうる: source・configuration・tblgen・install を identity の内容で受け入れ、tree の外の toolchain の tree への書き込みを拒否（`ZEDBSD_LLVM_ALLOW_FOREIGN=yes`）、package の source の複写を link の中身から（libcxx・clang）。bug089.sh 修正前 8/9 FAIL・修正後 PASS | cleared（2026-09-28） |
| [ws073-p028](phase028/phase.md) | BUG-100 | Remacs 用 host Noct の重複 build と古い `.nb` 出力名による `make` の失敗を修正。canonical host Noct を使い、取得 source の `.nb` の場合だけ patch。toolchain の Noct smoke 依存を外す | cleared（2026-09-29、q498 で通常の `make` を完走） |
| [ws073-p029](phase029/phase.md) | BUG-102 | ping を setuid root（mode 4755）にし、raw socket を開いた直後に `setuid(getuid())` で権限を落とす（ユーザーの決定、BSD と同じ）。QEMU で kei（uid 1000）から 127.0.0.1・10.0.2.2 へ ping が通る、`PING-USER:PASS` | cleared（2026-09-29） |
| [ws073-p030](phase030/phase.md) | BUG-051 | sshd などの SIGSEGV の原因は、signal の frame が中断した %rsp の真下（amd64 の red zone）に積まれ、red zone を使う libcrypto の leaf の関数の変数を壊すこと。`src/kern/signal.c` で amd64 は 128 byte 空ける。gdbstub で捕獲、red zone の probe 修正前 FAIL・後 PASS、SSH の負荷 1092 session で落ち 0 | cleared（2026-09-29） |
| [ws073-p034](phase034/phase.md) | BUG-107 | system 全体の socket の上限 32 を 1024 に。raw ICMP・packet・route は family ごとに 32（作成で ENFILE）、TCP の timer の snapshot は数に合わせて割り当て。Venus の guest で client 44・socket 100、SSH 10 本並列、socketpair 200 ＋ TCP 60 が PASS | cleared（2026-09-29） |
| [ws073-p035](phase035/phase.md) | BUG-108 | unix の blocking の connect が backlog の空きを待つ（nonblocking は EAGAIN、close で ECONNREFUSED、signal で EINTR）、backlog の上限 16 → 128。wltest の「EMFILE」は zedBSD の errno 24 = EAGAIN の読み違え。一斉の 44 client が 3 回とも全て描画 | cleared（2026-09-29） |
| [ws073-p036](phase036/phase.md) | BUG-039・BUG-031 | BUG-039: 状態は 09-27 と同じ（VFS PASS、overlay の link 125 未定義、UFS の断片が作れない）、host 試験の土台の WS へ。BUG-031: 起動の途中の画面を撮る試験（`console-midboot.py`）が修正の無い kernel で 10/10 検出、今の kernel で 21 起動 0 → resolved | cleared（2026-09-29） |
| [ws073-p039](phase039/phase.md) | BUG-106 | `ssh -tt 'cd /bin && ls'` を修正ありの kernel で 800 回（負荷なし 300・負荷あり 500）欠け 0、red zone の修正を外した kernel では 17 回欠け（全て rc 255 ＋ SIGSEGV）→ BUG-051 の duplicate | cleared（2026-09-29） |
| [ws073-p040](phase040/phase.md) | BUG-030 | 起動時の USB mass storage の読み取りの ETIMEDOUT の再現と原因（TCG と KVM、disk の丸読み） | uncleared（2026-09-29、wrap up。再現した（TCG 2 回に 1 回、並列の KVM 3 回に 1 回、起動の途中の CSW の時間切れだけ）、原因は未特定） |
| [ws073-p041](phase041/phase.md) | BUG-030 | 原因の特定と修正: guest の event の取りこぼしではなく、起動時の SYNCHRONIZE CACHE に QEMU が host の image の fdatasync を待って CSW を返さず、usb-storage の CSW の 5 秒の時間切れに掛かっていた（xHCI の診断と QEMU の trace で 6 回中 6 回同じ）。BOT の段の timeout を SCSI disk の慣例（30 秒、flush 60 秒）にした | cleared（2026-09-30 main の判断: 受け入れの条件は BUG-030 の形の時間切れ 0。KVM の 1 回は BUG-116 の形で移した）（2026-09-30、試験の担当の 2 回目の枠。受け入れ条件（main の判断で「usb-storage の e…））（2026-10-08 q910 P2 の照合で phase.md に合わせた。旧: uncleared（2026-09-30。受け入れ: TCG 75 回・KVM 40 回で flush の待ちの時間切れは 0（修正前 TCG 40 回中 7 …） |
| [ws073-p042](phase042/phase.md) | BUG-116 | 原因の特定と修正: driver が command の poll の間 IMAN.IE を 0 にして戻していたが、QEMU の xHCI は IE=0 で MSI-X の vector を unuse し、IE=1 の復帰で「IP の再送 → use」の順なので再送が捨てられ、ring に残った event と EHB=1 のまま止まっていた（EP0 の `iman=3` と bulk の `iman=2` の両方を説明）。command の間も IE を落とさない（Linux と同じ） | cleared（2026-09-30。受け入れ: TCG 2×20（起動のみ）と KVM 2×20（丸読み 1 回）の 80 回で `xhci: cancel` 0・attach-failed 0・`error=` 0・列挙の再試行 0、usb-hid 毎回、SSH 80/80、`dd` 40/40 が 2216689664 bytes。boot test（uefi-usb）PASS、serial の login 可。BUG-116 を resolved） |
| [ws073-p043](phase043/phase.md) | BUG-119 | Shut Down で電源が切れない: kernel に ACPI の S5 の電源断が無かった（init は HALT を送っていた）。UAPI に `KERN_SYSTEM_POWEROFF`、`/dev/system`・`kern_platform_poweroff()`・ACPI の `drv_acpi_poweroff()`（起動時に `\_S5`、電源断で `\_PTS(5)` と PM1a/b_CNT の SLP_TYP\|SLP_EN）、init は POWEROFF を送り `EOPNOTSUPP` なら HALT | cleared（2026-09-30。QEMU: `/sbin/poweroff` の 6 秒後に QEMU が `guest-shutdown` で終了、reboot は従来どおり、boot test PASS。5330 の実機は未実施） |
| [ws073-p044](phase044/phase.md) | BUG-123 | desktop-probe が `--timeout-s=3` の後に終わらない: kernel の poll は正しく（guest で 1000 回単位の時間切れ・close の試験）、libwayland-client の `wl_display_dispatch_queue` が queue の event が来るまで時間切れ無しで待ち続けていた。標準と同じ 1 回の待ちと読みに | cleared（2026-09-30、P1。Venus の guest の restart の手順 修正前 7 回中 2 回 FAIL → 修正後 18 回 PASS、host の回帰試験。5330 の実機は未実施） |
| [ws073-p050](phase050/phase.md) | BUG-129 | package の menu の Fonts: 直しは q643（P1）で main に入済み。host の fixture に Packages > Fonts > noto-color-emoji の選択の walk（curses なし）を追加 | cleared（q652、fixture PASS） |
| [ws073-p045](phase045/phase.md) | BUG-135 | stat が数秒止まる: gdbstub で `ufs_lookup` が volume 全体の `namespace_lock` を待ち、持ち主の journal の commit か名前の変更が device の flush を lock を持ったまま待っていた。検索と commit が namespace を共有、fsync の最後の flush を lock の外へ。QEMU で停止 10 → 2 回（4 回ずつ） | uncleared（2026-10-03、P1。Q1 の割り込みで中断、残る停止は名前の変更 → `ms->lock` → commit の flush の鎖） |
| [ws073-p051](phase051/phase.md) | BUG-135 | 残る停止の直し: journal（j3）の commit を閉じる段（lock の中、memory の複写）と書いて flush する段（lock の外）に分け、pin を transaction の sequence の tag に（`buf_write_pinned`・`buf_unpin`）、freed set を running と閉じた側の 2 つに。T1・T2 への試験の依頼はしない（user） | cleared（2026-10-04、fsprobe の stat 最長 4 ms） |
| [ws073-p031](phase031/phase.md) | BUG-104 | less で Ctrl-F・f（1 画面進む）と Ctrl-B（1 画面戻る）。more は不変。host の pty 試験と guest で `PAGER-KEYS:PASS` | cleared（2026-09-29） |
| [ws073-p032](phase032/phase.md) | BUG-105 | Logi Bolt の受信機（046d:c548）の HID の descriptor を parse する: 同じ key の複数の field（0x31・0x32 → KEY_BACKSLASH）を許し、keyboard の usage を持たない array を読み飛ばし、AC Pan を REL_HWHEEL に。host 試験 PASS、HID の host 試験 3 本の回帰なし、vmunix warning 0、boot test PASS | in-progress（2026-09-29、実機の確認はユーザー待ち） |
| [ws073-p033](phase033/phase.md) | BUG-106 | `ssh -tt` の最後の命令の出力の欠け: kernel の pty と OpenSSH の終わりの順を読み（出力を捨てる経路なし）、pen の image で約 500 回（欠け 0。0 byte の 1 回は sshd の listener の SIGSEGV と同時で BUG-051 の種類）。原因未確定、修正なし | uncleared（2026-09-29、WS081 のサブエージェントが main の依頼で実施、60 分で区切った） |
| [ws073-p037](phase037/phase.md) | BUG-109 | libm の `sqrt`・`sqrtf` を amd64 の `sqrtsd`・`sqrtss`、arm64 の `fsqrt` に（kernel・i386 は整数の平方根のまま、`FE_INEXACT` は根の 2 乗の検査）。host の libm の試験の出力が変更前と同じ、bit・errno・例外の比較 PASS、guest の libm の試験 PASS、`sqrtf` 347.5 → 220.5 ns、boot test PASS、arm64 は build だけ（WS035 のエージェントが main の依頼で実施） | cleared（2026-09-29） |
| [ws073-p038](phase038/phase.md) | BUG-110 | TLS の thread pointer を processor から読む: rtld の `__tls_get_addr`・TLSDESC・`__rtld_pthread_private` は amd64 `%fs:0`・arm64 `TPIDR_EL0`、static の libc も main thread の attach の後は同じ（i386 等は syscall のまま）。errno 1 回 204 → 7.5 ns、複数 thread と signal の TLS の試験（dynamic・static）・dyntest・libm・desktop・boot test PASS、arm64 は build だけ（WS035 のエージェントが main の依頼で実施） | cleared（2026-09-29） |
| [ws073-p046](phase046/phase.md) | BUG-053 | RAM を超える anonymous memory の残り（1300 MiB）: 測定の道具 swaphog を作り直し、swap の slot の探索を next-fit に | cleared（T2-018: 1300 MiB を 53 s） |
| [ws073-p047](phase047/phase.md) | BUG-052 | tmpfs の容量を物理 memory の半分に: file の data を物理 page と radix tree の index で持つ（heap と 2 乗の解放をやめる）、frame は private の page を reclaim して取る | cleared（q651、T1-048・T1-052 PASS） |
| [ws073-p048](phase048/phase.md) | BUG-103 | /bin/sh の上下の矢印の履歴を PS/2 の keyboard だけの QEMU で確かめて閉じる（修正は WS087 で済み） | cleared（q651、T1-045 PASS） |
| [ws073-p049](phase049/phase.md) | BUG-033 | guest の clang の速さの今の測定（expat の 3 file の compile、512 MiB・2 GiB）、残る原因があれば直す | cleared（q651、T1-050 の測定） |
| [ws073-p052](phase052/phase.md) | — | i386 pcat の vmunix の build（ACPI の無い構成の `drv_acpi_poweroff`、`splash.o` の欠け） | cleared（q651、pcat・pc98・amd64 の build warning 0） |
| [ws073-p053](phase053/phase.md) | BUG-163 | fsync の後、journal の pin が同じ line の普通の content を止める（4 KiB に揃わない volume で block の境の line）: buffer cache が line の block ごとに journal の外の書き込みを記録し、`ufs_sync` が pin された line のその sector だけを cache の下から書く。T には依頼しない（user） | cleared（q664、2026-10-04） |
| [ws073-p054](phase054/phase.md) | BUG-164 | crash の後の名前の無い inode: mount の回収（orphan の scan）が tail journal の volume でしか走らなかった。v3・journal 無しの volume でも前の session が clean に終わらなかったとき scan する。T には依頼しない（user） | cleared（q665、2026-10-04） |
| [ws073-p055](phase055/phase.md) | BUG-202 | LPSS/ACPIの待機をidleで行わないようdevice初期化をboot_workerへ移す | cleared（2026-10-06、user指定の機能修正・buildまで。main統合済み、実機UATはuser） |

## 残りの bug（2026-09-27 21 時の時点）

| Bug | 状態 | 次の手 |
| --- | --- | --- |
| BUG-031 | resolved（ws073-p036、起動の途中の画面の試験 21 回 0、修正の無い kernel では 10/10 検出） | — |
| BUG-036 | 緩和（列挙の再試行） | 時間切れの原因（xHCI の event か QEMU か）、hub の下の port |
| BUG-030・041・051 | 再現せず | 再現したとき |
| BUG-039 | VFS の fixture だけ修正（2026-09-29 に再確認、変化なし） | host 試験の土台の WS（overlay・UFS の fixture） |
| BUG-024 | ユーザーの判断待ち（A 説明だけ／B PC-98 の PCI・USB の移植） | 判断の後 |
| BUG-023・013・025 | PC-98・実機が要る（amd64 だけの方針で未実施） | 試験が許されたとき |
| BUG-026 の workaround | kernel は修正済み。clang の package の `--mmap-output-file` は main の判断で当面残す | WS032 の受け入れを無しで通したら外す |

## 判断が要る点

- BUG-024（PC-98 の menuconfig の PCI・USB）: A 説明だけ／B 移植の WS（main 経由でユーザーへ、2026-09-27）。既定: どちらもせず tracking。

- （解決 2026-09-27）ws073-p002: kernel が private に持つ boot の FAT（ESP）の公開の mount が EBUSY になった。ESP を running system から触るなら
  kernel の mount を公開する案（[phase](phase002/phase.md)）。ユーザーの判断（原文）: 「ESPは/boot/espにします。/bootはBOOTという名前のFATパーティションですね。
  UEFIのみのイメージでBOOTパーティションがない場合もあります。」→ BOOT の FAT を `/boot`、ESP を `/boot/esp` に公開する。BOOT が無い UEFI だけの image では
  `/boot` はただの directory で ESP は `/boot/esp`。ws073-p009 で行う。
- （解決 2026-09-27）amd64 の /boot・/boot/esp: ユーザーの判断（原文）「amd64 のUEFIおよびハイブリッドのイメージでは、カーネルが特殊な処理で/bootや/boot/espを
  マウントせず、fstabに任せてください。つまり、デフォルトの配布イメージではマウントしなくていいです。インストーラがfstabに書けば済むことです。」→ ws073-p014。
- （解決 2026-09-27、最終の layout、p009・p014 の公開を置き換える）ユーザーの判断（原文）「amd64 UEFIでも、/boot/boot0みたいなマウントは自動でやりましょう。
  ESPはfstabです。スワップだけでもマウントします。rootfs.imgは読み込み専用なので、書き込みできなくても、読み込めていいと思います。」→ ws073-p015（cleared）。
- ws073-p015（既定を選んだ、可逆）: ESP 自身が `bootN:` で参照される slot なら `/boot/bootN` にも出す（fstab の `/boot/esp` は同じ状態の別の view）。
  root に promote された slot は出さない。rpi4 の legacy ARM overlay の boot の FAT は `/boot/boot0` に。起動後の `swapon bootN:PATH` は成功時にその slot を出す
  （[phase](phase015/phase.md)）。
- ws073-p010（既定を選んだ、可逆）: FAT での file の作成は要求の mode を捨て、mount が見せる mode（0755 root:wheel）で見せる（msdosfs と同じ）。
  以前の「一致しなければ EOPNOTSUPP」に戻すなら fat.c の 2 つの関数を戻す（[phase](phase010/phase.md)）。

## 段の計画（2026-09-30 Q1、広く浅くの方針）

bug の WS は ticket ごとに直す形のままにする（数値目標は各 ticket の再現の試験の回数と結果: 例 BUG-030・116 は TCG と KVM の 2×20 で 0）。
段は「デモへの影響」で分ける。
- L1: デモの台本の場面を止める bug が 0（BUG-117・119・116 は済み）。今の対象: BUG-122（WS099）、BUG-121（WS099）。
- L2: デモの操作で稀に出る bug の再現の試験（BUG-036 の 2 台目の usb-storage、BUG-123 の poll の時間切れ）。
- L3: デモの外の bug（BUG-120 の窓 30 個、BUG-058 の間欠）。

### 進め方とハーネス（2026-09-30 Q1 の補足）

- 型は BUG-030・BUG-116 で固まった: **(1) 再現の率を上げる負荷の試験を先に作る**（`plan/ws073/tests/usb-stress.sh` の TCG・KVM の 2 本並列）→
  **(2) 時間切れの時点の状態を 1 行で出す診断を入れる**（xHCI の `pending-events`・`iman`・`completion`）→ **(3) 修正の担当（Fable High）と
  試験の担当（Opus Mid）を分ける** → 受け入れは 2×20 回で 0。
- L2 の BUG-036 は、`usb-stress.sh` に 2 台目の usb-storage を足す引数を加え、同じ 2×20 で usb-net の SSH の成功を数える。
- L2 の BUG-123 は、poll の時間切れが戻らないかを確かめる小さな C の試験（`poll(fd, 1, 3000)` を 1000 回、戻りの時間を記録）を先に作り、
  kernel の不具合か probe の不具合かを切り分ける。
