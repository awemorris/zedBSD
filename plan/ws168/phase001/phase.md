<!-- awesome-plan project=zedbsd record=ws168-p001 -->

# ws168-p001: 隔離されたプレビューの command の要件と設計

Phase ID: `ws168-p001`
Parent: [WS168](../ws.md)
Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: H1〜H7 決定済み、p003・p004 が cleared）（旧: planning（2026-10-05 P1 generation17。設計の第 2 版（ユーザーの review を反映）。code はユーザーの review の後。§8 の判断が要る。2026-10-05 夜: H1〜H7 決定））
Phase disposition: normal
Queue: Q1 の 2026-10-05 の指示（第 1 版の設計、続けてユーザーの review に従った第 2 版）

## 第 1 版からの違い（2026-10-05、ユーザーの review）

ユーザー（原文）:「設計まで進めて、レビューさせてください。sandboxed vforkっぽいsystem callがいいです。サンドボクシングの権限設定は拡張可能なようにAPI設計したいです。シンプルにしたいです。」

| 第 1 版 | 第 2 版 |
| --- | --- |
| program が自分で `sandbox_enter(root_fd, flags)` を呼んで sandbox に入る | 親が **`sandbox_spawn`** を呼び、子を **最初の命令から sandbox の中で** 始める（vfork のように、親は子が始まるまで待つ） |
| 権限は「1 つの固定の表」と flag 1 つ | 権限は **版つきの構造体**（構造体の大きさで版を表す）と **許す操作の種類の bit の集合**。知らない bit・知らない領域が 0 でないなら断る（古い kernel は新しい要求を断る = 安全側） |
| root を `/var/empty` に替える | 子は **name space を持たない**（root も cwd も無い）。path を取る call は表で断られ、仮に通っても解決する場所が無い。chroot より強く、`/var/empty` も要らない |
| program は動的 link、sandbox に入る前に library と font を読む | zedBSD の子は **静的 link の image** に限る（子は最初から file を開けないので、dynamic linker が library を開けない）。PDF の代わりの font は image に埋め込む |
| uid・rlimit・fd の準備は program の中 | 親が要求の構造体で渡す: fd の対応（親の fd → 子の fd 番号）、上限（memory・CPU の時間・書く大きさ）。子は uid も信号の扱いも何も持ち込まない |

## 範囲

- ユーザーの要件（ws.md の 1〜5）を満たす、縮小表示を作る専用の command（仮の名前 `keiland-preview`、`KEILAND_LIBEXECDIR` に置く）の設計。
- kernel に足す、**子を sandbox の中に起こす system call**（`sandbox_spawn`）の UAPI と実装の方針。HAL は変えない。
- 呼び出し側（Files の縮小表示・Quick Look・Today の hero、Settings の背景の tile）の変え方、時間・memory の上限、失敗の扱い。
- Linux・FreeBSD の版で同じ形をどう作るか。
- 試験の方針。
- 由来: ユーザーの 2026-10-05 の追加（ws.md）。関係する記録: [Future Work](../../future-work.md) の F-035（縮小表示、「decoder の package の監査が要る」）。
- 範囲外: 実装（p002 以降）。compositor の背景の復号（§8 の H4）。Image Viewer・PDF Viewer・browser（WS074 の D1 で process の分離は後回し）など、
  利用者が開いて見る app の隔離（別の WS の題。ただし同じ `sandbox_spawn` を使えるように API を作る）。

## 1. 要件の読み替え

| ユーザーの要件 | この設計での形 |
| --- | --- |
| 専用の command | `keiland-preview`。入力を 1 つ読み、縮小表示を 1 つ書いて終わる。常駐しない |
| 入力は fd 0、出力は fd 1 で開いた状態で起動、出力は規定の場所に保存 | 親が入力の file（読み取り）と cache の記録の一時 file（書き込み）を開き、`sandbox_spawn` の fd の対応で子の fd 0・1 にする。子の fd の表には **この 2 つしか無い**。書き終わったら親が一時 file を規定の名前に rename する |
| 最低でも chroot で隔離 | 子は name space を持たない（root も cwd も無い）。chroot の「狭い場所」でなく「場所が無い」 |
| そのほか可能な限りの jail | 許す操作の集合（§3.3）、上限（memory・CPU・書く大きさ）、親の時間の上限、静的 link の image、新しい実行可能な page を作らせない |
| 他の file を open できない、network を使えない、fork できない | 許す操作の集合に open・socket・fork・vfork・exec・`sandbox_spawn` が無い。fd を増やす call（dup・pipe・fcntl・recvmsg の fd の受け渡し）も無い |
| 純粋に fd 0・1 の入出力と計算のみ | 基本の集合（§3.3 の `BASE`）: 持っている fd の read・write・pread・lseek・fstat・close、匿名の memory、時刻、exit、自分の thread の信号 |
| RCE されても乗っ取られる権限を最小に | 子が最初の命令から sandbox の中にいるので、sandbox の外で信頼できない byte を扱う瞬間が無い。乗っ取った code にできるのは、fd 0 を読む・fd 1 に書く・上限の中で CPU と memory を使う・自分を終わらせる、だけ |

## 2. 今の kernel（2026-10-05 の main を読んだ）

| 項目 | 今 | 場所 |
| --- | --- | --- |
| system call の入口 | HAL が登録された dispatcher（`kernel_syscall_handler`）を呼び、`syscall_dispatch_body` の大きな switch に入る。**全部の call・全部の CPU の架構が 1 か所を通る**。sigreturn の後と停止の後の再 dispatch もこの switch を通る | `src/kern/syscall.c` 9962（`kernel_syscall_handler`）、9377（`syscall_dispatch_body`） |
| call の番号 | `enum syscall_number`（1〜169、17・18 は欠番）。次の番号は 170 | `include/uapi/syscall.h` |
| libc の stub | `call(KERN_SYS_…, …)`（例 `chroot`） | `userland/base/libc/posix.c` 4952 |
| process の生成 | fork・vfork（libc の `posix_spawn` は vfork の上）・execve・fexecve・thread_create。kernel の中だけの spawn（`process_spawn_from`、init の起動）がある。user に見える spawn の call は無い（17・18 は昔の spawn の欠番） | `syscall.c` 8409・8427・8896・8935・7455、`src/kern/exec.c` 546 |
| process | `struct process` の `flags`（fork で写らない）・`limits`・`cred`・`fd`・`cwdi`・`set_id`（fork で写り exec で決め直す） | `include/kern/process.h` 68〜184、`src/kern/process.c` 1108・1358・1415 |
| name space | `cwdi`（root と cwd）。path を取る call の多くは `cwdi == NULL` を EINVAL で断る | `include/kern/namei.h` 44、`syscall.c` 2723・3397 ほか |
| chroot | euid 0 だけ | `src/kern/namei.c` 562・590 |
| rlimit | `NOFILE`・`STACK`・`AS`・`CORE`・`CPU`（soft で SIGXCPU、hard で SIGKILL）・`DATA`・`FSIZE`（write の系統で実施） | `include/uapi/resource.h`、`src/kern/resource.c` 175・315 |
| 新しい fd を得る道 | open・openat・socket・socketpair・accept・pipe・pipe2・dup・dup2・dup3・fcntl（F_DUPFD）・recvmsg（SCM_RIGHTS）・GPU の ioctl の handle の export | `syscall.c` 1389・1478・1728・2393・2447・2744・8139〜8215・8388、`src/drivers/gpu/gpu.c` 3980 |
| mmap | 匿名の private・shared、開いた fd の map（`MAP_SHARED` の書き込みを含む）、device の map。W^X は `vm_prot` | `syscall.c` 3553・3569〜3753 |
| set-ID の exec | image の set-user-ID の bit を取る（traced の process は `MOUNT_NOSUID` の扱い） | `src/kern/exec.c` 299〜329・1355 |
| 静的 link の program | 前例あり: `crt0.o` と `libc.o` を `-static -T platform/amd64/user.ld`（`posix-phase5-helper`・`phase85-curses-test`）。compat の library・libpdf は `.so` だけ | `platform/amd64/vmunix.mk` 616〜720・2176・2392 |

capability mode に当たる物（Capsicum・seccomp・pledge）は無い。

## 3. kernel: `sandbox_spawn`（UAPI の案、HAL は不変）

### 3.1 形

```c
/* include/uapi/syscall.h */
	KERN_SYS_sandbox_spawn = 170,

/* include/uapi/sandbox.h（新） */

/* 基本の集合に足す操作の種類（allow の bit）。版 1 は 1 つだけ。 */
#define SANDBOX_ALLOW_THREADS		(UINT64_C(1) << 0)	/* 子の中で thread を作る・待つ */
#define SANDBOX_ALLOW_KNOWN		SANDBOX_ALLOW_THREADS

/* 要求の flag。 */
#define SANDBOX_SPAWN_DENY_ERRNO	(UINT32_C(1) << 0)	/* 断る call で EPERM を返す（無ければ子を SIGKILL で終わらせる）。試験用 */
#define SANDBOX_SPAWN_KNOWN		SANDBOX_SPAWN_DENY_ERRNO

#define SANDBOX_FD_MAX			16U	/* 子に渡せる fd の数、子の fd 番号は 0〜15 */

/* 親の fd を、子の fd 番号に。 */
struct sandbox_fd {
	int32_t from;			/* 親の fd */
	int32_t to;			/* 子の fd 番号 */
};

/* 要求。size が版を表す（構造体は末尾にだけ伸びる）。 */
struct sandbox_spawn {
	uint32_t size;			/* 呼び出し側が知っている sizeof(struct sandbox_spawn) */
	uint32_t flags;			/* SANDBOX_SPAWN_* */
	uint64_t allow;			/* SANDBOX_ALLOW_*（基本の集合に足す物） */
	int32_t image;			/* 子の image（静的 link の ELF）の fd */
	uint32_t fd_count;		/* fds の数（SANDBOX_FD_MAX まで） */
	uint64_t fds;			/* struct sandbox_fd の配列の user の address */
	uint64_t argv;			/* NULL で終わる char * の配列の user の address */
	uint64_t memory_max;		/* address space の上限（byte、RLIMIT_AS）。0 は親と同じ */
	uint64_t cpu_seconds;		/* CPU の時間の上限（秒、RLIMIT_CPU）。0 は親と同じ */
	uint64_t write_max;		/* 書く file の大きさの上限（byte、RLIMIT_FSIZE）。0 は親と同じ */
};

/* libc（include/libc/sandbox.h、userland/base/libc/posix.c） */
pid_t sandbox_spawn(const struct sandbox_spawn *request);
```

- 戻り値: 子の pid、または -1 と errno。子は普通の子 process（`waitpid`・`wait4`・SIGCHLD・`kill` は今のまま）。
- **vfork に似た所**: 呼んだ thread は、kernel が子の process を作り、image を読み込み、子が走れる状態になるまで戻らない。読み込みの失敗（image が
  無い・静的でない・実行の権限が無い）は子が走る前にこの call の errno で返る（`posix_spawn` の「exec の失敗を pipe で親に知らせる」仕組みが要らない）。
- **vfork と違う所: 子は親の memory を共有しない**。子は新しい空の address space に image を読み込んで始まり、親の code を 1 命令も走らせない。
  理由: (1) memory を共有すると、乗っ取られた子が待っている親の memory を書き換え、親が戻った時に親の権限で走る code を仕込める。(2) 子が親の code
  （動的 link の library・親の状態）の上で sandbox に入ると、sandbox の中で信頼できない byte を扱うより前に、親の側の大きな code が子の中にある。
  (3) fork のような写しも採らない: 親の memory（file の名前・利用者の文書・認証の途中の data）が乗っ取られた子に見え、大きな GUI の process の写しは重い。
  **image を exec する形**にすると、子の中にあるのは小さな command の code と、渡された fd だけになる。
- 子の状態（全部 kernel が決め、呼び出し側から持ち込まない）:

  | 項目 | 子 |
  | --- | --- |
  | address space | 新しく空。image（静的な ELF）と stack（argv、環境は空、auxv）だけ |
  | fd の表 | `fds` の対応で渡した物だけ（親の file object を共有、子の番号は `to`）。CLOEXEC は付けない |
  | name space | **無し**（`cwdi` が NULL: root も cwd も無い） |
  | credential | 呼び出し側と同じ（§8 の H2）。image の set-user-ID の bit は効かせない。`set_id` は 0 |
  | 信号 | 全部の動作が既定、mask は空、保留は無し |
  | process の集まり | 呼び出し側と同じ group・session。制御の端末の fd は渡さない限り無い |
  | 上限 | 親の rlimit と要求の小さい方（memory・CPU・書く大きさ）。`RLIMIT_NOFILE` は `SANDBOX_FD_MAX`、`RLIMIT_CORE` は 0 |
  | 許す操作 | §3.3 の基本の集合 + `allow` |

### 3.2 要求の確かめ（版と拡張）

1. `size` を先に読む。`size` が版 1 の大きさより小さければ EINVAL。kernel が知っている大きさより大きければ、知らない部分の byte が全部 0 の時だけ受ける
   （0 でなければ E2BIG）。kernel が知らない部分は「使わない」の意味になる（Linux の `openat2`・`clone3` と同じ規則）。
2. `flags` と `allow` に kernel の知らない bit があれば EINVAL（古い kernel は新しい権限の要求を断る。**黙って弱い sandbox にしない**）。
3. `fd_count` が `SANDBOX_FD_MAX` を超える、`to` が範囲の外、`to` が重なる、`from` が開いていなければ EINVAL・EBADF。
4. `image` は通常の file で、呼び出し側に実行の権限があり、ELF で **`PT_INTERP` が無い**（静的 link）こと。違えば EACCES・ENOEXEC。
5. 呼び出し側が sandbox の中なら EPERM（`sandbox_spawn` は基本の集合に無いので、実際には §3.3 で先に断られる）。

**拡張の仕方**: 新しい権限は `allow` の新しい bit で、新しい種類の資源（例: 読み取りだけの directory を name space として渡す、渡した socket の送受信）は
構造体の末尾の新しい領域で足す。どちらも設計の改訂として扱い、bit と領域を足した kernel だけが受ける。

### 3.3 許す操作の集合

`syscall_dispatch_body` の先頭（switch の前）で、sandbox の process なら番号と引数を確かめる。sigreturn の後や停止の後に再 dispatch される call も同じ
switch を通るので、全部がこの確かめを通る。集合は spawn の時に `allow` から作る process ごとの bitmap（call の番号 1 つに 1 bit）に変えて持つ。

| 集合 | 許す call | 引数の制限 |
| --- | --- | --- |
| 基本（いつも） | `exit`・`thread_exit` | — |
| 基本 | 持っている fd の `read`・`write`・`pread`・`pwrite`・`readv`・`writev`・`lseek`・`fstat`・`close` | 無し（fd の表に有る物だけが対象で、新しい fd は作れない。fd 0 を読み取りで開けば書けない、などは開いた時の mode のとおり） |
| 基本 | `mmap`・`munmap`・`mprotect`・`brk` | `mmap` は `MAP_ANONYMOUS \| MAP_PRIVATE` で fd が -1 の物だけ（file・device の map、匿名の `MAP_SHARED` は断る）。`mmap`・`mprotect` は `PROT_EXEC` を断る（image の code の外に実行できる page を作らせない） |
| 基本 | `clock_gettime`・`clock_getres`・`nanosleep`・`sched_yield` | — |
| 基本 | `getpid`・`thread_self`・`getrlimit`・`sigprocmask`・`sigaction`・`sigreturn` | — |
| 基本 | `thread_kill` | 自分の process の thread だけ（今の実装が既に他の process を断る）。libc の `raise`・`abort` が使う |
| 基本 | `usync`・`atomic`・`getentropy` | — （共有の map が無いので `usync` は他の process に届かない） |
| `SANDBOX_ALLOW_THREADS` | `thread_create`・`thread_join`・`thread_detach`・`thread_cancel` | 自分の process の中だけ |

それ以外は全部断る: path を取る全部の call、`socket`・`socketpair`・`accept`・`connect`・`sendmsg`・`recvmsg`（fd の受け渡し）、`fork`・`vfork`・`execve`・
`fexecve`・`sandbox_spawn`、`dup` の系統・`fcntl`・`pipe` の系統、`ioctl`・`sysctl`、`kill`・`sigqueue`・`ptrace`、`set*id`・`setrlimit`・`setpgid`・`setsid`、
`ftruncate`・`fsync`・`fchmod`・`fchown`・`futimens`・`flock`、timer の系統。集合は「許す物を書く」形なので、後で足した call は自動的に断られる。

例外（2026-10-06 ユーザーの決定、案 (a)）: `ioctl(fd, TCGETS)`（端末かを問う。静的 link の libc の起動が fd 1 に問う）だけは、集合の外でも sandbox の確かめが `ENOTTY`（端末でない）と答え、file・driver に届かず klog も書かない。他の ioctl は断る。

断った時: 既定は子を SIGKILL で終わらせる（信号の処理を経ない、親の `waitpid` には SIGKILL で終わったと見える）。`SANDBOX_SPAWN_DENY_ERRNO` の時は
-EPERM を返す（試験用）。どちらも klog に 1 行（`SANDBOX deny pid=… call=…`、1 process に 8 行まで）。

### 3.4 kernel の中の作り

- `src/kern/sandbox.c`（新）: 要求の確かめ（§3.2）、集合の bitmap の作成、`sandbox_check(process, number, args)`（§3.3）。
- `sys_sandbox_spawn_call`（`syscall.c`）: 子の process を `process_create` の系統で作り、kernel の中の spawn（`process_spawn_from`、`exec.c` 546。init の起動に
  使う、新しい address space に image を読む道）で image を読み込む。fd の表を `fds` から作り、`cwdi` を作らず、rlimit を決め、`process->sandbox`（集合と flag、
  参照数つきの小さな構造体）を付けてから走らせる。
- `struct process` に `sandbox`（NULL なら普通の process）。fork・vfork・exec は断るので引き継ぐ場面は無いが、備えとして `fork_process` の `set_id` を写す所で
  写し、exec は `MOUNT_NOSUID` の扱いにする。
- `cwdi` が NULL の process: path を取る call は今も多くが EINVAL で断る。p002 で path を取る全部の call と `getcwd`・`fchdir`・`*at` の系統を調べ、
  NULL を必ず断ることを確かめる（集合で断られるので二重の守り）。
- HAL（`include/hal/hal.h`・`src/hal/`）は変えない。

### 3.5 足さない物（理由）

- fd ごとの細かい権利（Capsicum の `cap_rights`）: 開いた時の mode（読み取り・書き込み）と「新しい fd を作れない」で足りる。必要になれば `sandbox_fd` に
  領域を足す（構造体の末尾の拡張と同じ規則で、配列の要素の大きさを `size` に連動させる）。
- program を載せる filter（seccomp-bpf）: kernel に interpreter が要り、攻撃面が増える。種類の bit の集合で足りる。
- 自分を sandbox に入れる call（第 1 版の `sandbox_enter`）: 子を最初から sandbox の中で始める形にまとめ、入口を 1 つにする。
- 専用の uid（§8 の H2）。

## 4. command（`keiland-preview`）

### 4.1 起動と引数

```
keiland-preview --width=N --height=N [--fit=contain|cover] [--stamp=TEXT]     （fd 0 = 入力、fd 1 = 出力、他の fd は無い）
```

- `--width`・`--height`: 縮小表示の最大の大きさ（Files は 256×256 の contain、Settings は 240×150 の cover、Quick Look は 1600×1600
  （`ui-preview.c` 52 の `LOOK_PICTURE_SIDE`）、hero は 1920×1080）。上限は一辺 4096。
- `--stamp`: 出力の PPM の comment に書く文字列（今の cache の記録の `# keiland-thumbnail mtime=… size=…`）。呼び出し側が作る。
- 終了の status: 0 成功、1 形式が分からない、2 壊れている、3 大きすぎる、4 memory が足りない、5 出力に書けない、64 引数の誤り、70 sandbox に入れない
  （Linux・FreeBSD、§7）。

### 4.2 手順

zedBSD の子は最初の命令から sandbox の中にいるので、program に「入る」手順は無い。

1. 引数を解釈する（argv は親が作った信頼できる文字列）。fd 0・1 を `fstat` で確かめる（通常の file）。
2. 入力を全部読む（上限は今の Files と同じ 64 MiB。超えたら status 3）。
3. 先頭の byte で形式を決め（PNG・JPEG・GIF・PPM/PGM・PDF）、復号して縮小する。
4. PPM（P6、comment に `--stamp`）を fd 1 に `write` で書く（stdio を使わない: libc の `isatty` は ioctl を使い、断られて子が終わるため）。
5. `exit`。

### 4.3 image（静的 link）と library

- zedBSD の子の image は静的 link でなければならない（§3.2 の 4。子は最初から file を開けないので dynamic linker が library を開けない）。build の規則は
  `platform/amd64/vmunix.mk` の前例（`crt0.o`・`libc.o`、`-static -T platform/amd64/user.ld`、`phase85-curses-test` が library の source を一緒に compile する形）に
  倣い、libz・libpng・libjpeg・libgif-compat、libtruetype、libpdf、`picture.c` の source を command 用に compile して link する。vmunix.mk の規則は他の WS の
  link の規則と同じく Q1 が main で掛ける。
- 画像の復号と縮小は、Files の今の code（`thumb.c` の復号、`canvas.c` の縮小）を command に移す。
- PDF: libpdf を link する（dlopen はしない）。libpdf は埋め込まれていない font の代わりを `/usr/share/fonts` から開く（`userland/base/libpdf/font.c` 1202〜）が、
  子は開けないので、**代わりの font を image に埋め込む**（`keiland.ttf` = Inter 0.9 MB、build の時に C の配列にする）。libpdf に「代わりの font を memory で
  渡す」口（userland の library の API の追加）を足す。代わりが無い文字は描かない（今と同じ）。
- 静的 link の libc の起動（`crt0`）が集合の外の call を呼ばないことを p002 で確かめる（`SANDBOX_SPAWN_DENY_ERRNO` と klog の行で調べる）。

### 4.4 sandbox の外で起動された時

zedBSD の command は sandbox の中にいるかを自分では確かめない（守りは kernel の側にある）。利用者が shell から直接起動すれば sandbox の外で走るが、
それは利用者が自分の権限で自分の file を復号するだけで、権限が増える道ではない。呼び出し側（§5）は必ず `sandbox_spawn` で起こす。
終了の status の 70 は Linux・FreeBSD で sandbox に入れなかった時（§7）に使う。

## 5. 呼び出し側

### 5.1 今の形（2026-10-05 の main を読んだ）

| 呼び出し側 | 今 | 場所 |
| --- | --- | --- |
| Files の縮小表示 | `fm_thumb_get` が 1 file を頼み、**main loop の次の回**（`fm_thumb_tick`）で cache の記録を読むか、無ければ `fm_image_thumbnail` で復号して縮小（256 の正方形に contain）し、記録に書く。**復号は Files の main の thread で同期に走る**（その間 window は止まる） | `userland/desktop/files/thumb.c` 191・228、`files.h` 663（`FM_THUMB_SIDE`） |
| Files の復号 | `fm_image_load`: file を全部読み（64 MiB まで）、PPM・PGM・PNG・JPEG（`kl_picture_jpeg`）・GIF の最初の 1 枚（`kl_picture_gif_first`）・PDF の 1 頁目（`fm_thumb_pdf`、libpdf を dlopen）。画素の上限は一辺 8192・16M 画素 | `thumb.c` 84・39〜43、`thumb-cache.c` 151 |
| Files の cache の記録 | `$XDG_CACHE_HOME`（無ければ `~/.cache`）`/keiland/thumbnails/<path の SHA-256>.ppm`、P6 と comment（`# keiland-thumbnail` と元の file の更新時刻・大きさ）。2000 個を超えたら古い物から 1800 個まで消す | `thumb-cache.c` 8〜25・270・357・446・515〜 |
| Files の Quick Look・preview の欄 | `fm_peek_picture` が `fm_image_thumbnail(path, side)`（Quick Look は 1600 の正方形）。cache しない | `peek.c` 98〜115、`ui-preview.c` 52・760 |
| Files の Today の hero | `fm_image_load(app->wallpaper)`（背景の file を全部の大きさで復号） | `ui-home.c` 410 |
| Settings の背景の tile | `look_thumbnail`: loader の thread で file を読み、`kl_wallpaper_decode`（WS138）で全部を復号して 240×150 に cover で縮小 | `userland/desktop/settings/look.c`（`look_thumbnail`） |
| compositor の背景 | prefetch・loader の thread で `kl_wallpaper_decode`（WS138） | `userland/desktop/wayland/glass.c`（§8 の H4） |


### 5.2 変え方

- 呼び出し側に compile する小さな helper（`userland/desktop/preview/client.c` と OS ごとの `zedbsd/spawn.c`・`linux/spawn.c`・`freebsd/spawn.c`）:

  ```c
  struct preview_job {
  	int input;		/* 読み取りで開いた入力（通常の file） */
  	int output;		/* 書き込みで開いた出力（一時 file） */
  	int width, height, cover;
  	const char *stamp;
  };
  int preview_start(const struct preview_job *job, pid_t *child);	/* 子を起こす */
  int preview_poll(pid_t child, int *status);				/* 終わったか（待たない） */
  void preview_kill(pid_t child);						/* 時間切れ: SIGKILL と回収 */
  ```

  zedBSD の `preview_start` は image（`KEILAND_LIBEXECDIR "/keiland-preview"` を `O_RDONLY | O_CLOEXEC` で開いた fd、`paths.h` の名前。文字列の
  `/usr/libexec` は boundary の C4 に反する）、fd の対応 `{input→0, output→1}`、argv、上限（memory 1 GiB、CPU 10 秒、書く大きさ 48 MiB）で
  `sandbox_spawn` を呼ぶだけ。Linux・FreeBSD は §7。
- 手順: cache の記録の一時 file（`<記録>.tmp.<pid>`、0600、`O_EXCL | O_CLOEXEC`）と入力（`O_RDONLY | O_CLOEXEC | O_NONBLOCK`、通常の file でなければ
  断る: FIFO・device を子に渡さない）を開く → `preview_start` → 親は開いた 2 つの fd を閉じる → `preview_poll` で終わりを見る → status 0 なら一時 file を
  記録の名前に rename して読む（今の `fm_thumb_cache_read`）、それ以外は一時 file を消して縮小表示は「無し」（icon）。失敗も cache に「失敗」の印の記録で
  残し、同じ file で繰り返さない。時間の上限（縮小表示 5 秒、PDF 10 秒）を超えたら `preview_kill`。
- **待ち方**: Files は今、main loop の中で同期に復号している。`fm_thumb_tick` は「走っている子が無ければ起こす」「走っている子を `preview_poll` で見る」
  だけを行い、終わった回に記録を読んで slot に入れ、window を描き直す。同時に走らせる子は 2 つまで。これで「大きな画像・PDF の復号の間 window が
  止まる」も無くなる。
- Quick Look（1600 の正方形）・Today の hero（1920×1080）: 同じ helper、出力は cache の folder の一時 file（読んだら消す）。
- Settings の tile: loader の thread で同じ helper（240×150 cover）、記録は同じ cache に大きさと fit を混ぜた名前で置く。
- これで **Files と Settings は復号の library を link しない**（libpng・libjpeg・libgif・libz-compat と `picture.c`、libpdf の dlopen、WS138 で Settings に
  足した `wallpaper.c` と link が消える）。

### 5.3 残る危険

- **出力の記録は信頼できない**: 乗っ取られた子は fd 1 に任意の byte を書ける。呼び出し側に残る解釈の code は「P6 の header と comment と画素の数を
  確かめて写すだけ」の小さな reader（今の `fm_thumb_cache_read`・`cache_header`、`thumb-cache.c` 270・588。一辺の上限を 4096 に）に限る。p004 で見直し、
  壊れた・大きすぎる・短い記録の host 試験を足す。
- **資源**: memory・CPU・書く大きさ（kernel の上限）、時間（親の SIGKILL）、同時の数（親が 2 つまで）で抑える。
- **kernel の bug**: 許す call（read・write・mmap など）の実装の bug は sandbox の外に出る道になりうる。集合を小さく保つ。

## 6. 試験（p002 以降で作る）

- kernel（`userland/tests/sandboxtest`、静的 link の子と、それを起こす親の試験の driver。T1 が QEMU で流す）:
  1. 要求の確かめ: `size` が小さい、知らない部分が 0 でない（E2BIG）、知らない `flags`・`allow` の bit、fd の数・番号の範囲・重なり、動的 link の image
     （ENOEXEC）、実行の権限の無い image（EACCES）。
  2. 子の状態: 渡した fd だけがある（`fstat(2)` 以降が EBADF）、上限が親と要求の小さい方、信号が既定。
  3. `DENY_ERRNO` の子で、集合の外の call が 1 つずつ全部 EPERM: open・openat・stat・getcwd・chdir・fchdir・socket・socketpair・pipe・dup・fcntl・fork・vfork・
     execve・`sandbox_spawn`・ioctl・sysctl・kill（親）・setuid・setrlimit・recvmsg・mount・thread_create（`THREADS` 無し）。
  4. 許す call が動く: read・write・pread・lseek・fstat・close、匿名の mmap・munmap・brk、clock_gettime、getentropy、`THREADS` の時の thread_create。
  5. 引数の制限: file の mmap、匿名の `MAP_SHARED`、`PROT_EXEC` の mmap・mprotect が EPERM。
  6. 既定（KILL）の子: 集合の外の call で SIGKILL で終わり、親の `waitpid` がそれを見る。
  7. `cwdi` が NULL の process の path の call の全数の確かめ（p002 の kernel の host 試験か klog）。
- command: 正常な PNG・JPEG・GIF・PPM・PDF、壊れた file、大きすぎる file、上限（memory・CPU・時間）。
- 「埋め込まれた攻撃」の代わり: 試験用の build で、復号の途中で open・socket・fork を呼ぶ code を通す flag を command に持たせ、子が SIGKILL で終わり、
  出力が無く、何も開かれていないことを確かめる。
- host（Linux）: command の復号と縮小の正しさ、`preview_*` の時間切れと失敗の扱い。

## 7. Linux・FreeBSD

Linux・FreeBSD の kernel に `sandbox_spawn` は無いので、**同じ 2 つの口（親の `preview_start`、子の command）** を OS ごとの source で作る。OS ごとの code は
`zedbsd/`・`linux/`・`freebsd/` の directory に置き、OS ごとの Makefile が選ぶ（boundary の L1 に合う。Files の `freebsd/mounts-freebsd.c` と同じ形。
libkeiland-backend の op にはしない: B3 で backend は compositor だけが使う）。

| | zedBSD | Linux | FreeBSD |
| --- | --- | --- | --- |
| 親（`preview_start`） | `sandbox_spawn` | `fork`（可能なら `clone` に `CLONE_NEWUSER \| CLONE_NEWNS \| CLONE_NEWNET`）→ 子で fd 0・1 を置き `close_range(2, ~0)`、rlimit、`PR_SET_NO_NEW_PRIVS`、名前空間があれば空の directory へ chroot → `execve`（環境は空） | `fork` → 子で fd 0・1 を置き `closefrom(2)`、rlimit、`procctl(PROC_NO_NEW_PRIVS_CTL)`、`security.bsd.unprivileged_chroot` が 1 なら `/var/empty` へ chroot → `execve` |
| 子（command の `main` の最初） | 何もしない（最初から中） | seccomp-bpf を入れる（§3.3 と同じ集合を BPF に: `read`・`write`・`pread64`・`lseek`・`fstat`・`close`・`mmap`（匿名・`PROT_EXEC` 無し）・`munmap`・`mprotect`・`brk`・`exit_group`・`rt_sigreturn`・`clock_gettime`・`getrandom`・`futex`、違反は `SECCOMP_RET_KILL_PROCESS`） | `cap_enter()`（Capsicum: path・socket の作成・fork を断る）、fd 0・1 を `cap_rights_limit` で読み・seek・fstat と書き・fstat に |
| image | 静的 link（kernel が強制） | 動的 link（seccomp は `main` の後なので library は読み込み済み） | 動的 link（同じ） |
| 違い | 子は最初の命令から中 | exec から `main` の最初までは sandbox の外（ld.so と libc の初期化だけで、信頼できない byte はまだ読まない） | 同じ |

seccomp・Capsicum が使えない（kernel が古い、設定で無効）時、command は復号せずに status 70 で終わり、呼び出し側は縮小表示を出さない（icon）。
名前空間・`unprivileged_chroot` が無く chroot ができない時は、seccomp・Capsicum が path を全部断るので要件 4 は満たす（§8 の H5）。

## 8. 人間の判断が要る点

| ID | 問い | 案 | 理由 |
| --- | --- | --- | --- |
| H1 | kernel に `sandbox_spawn`（system call 170、`include/uapi/sandbox.h`、§3）を足してよいか | 足す | 子を最初の命令から sandbox の中で始められる。権限は版つきの構造体と種類の bit で拡張でき、知らない要求は断る。HAL は不変 |
| H2 | 子を呼び出し側の uid のまま走らせるか、専用の uid に落とすか | 呼び出し側の uid のまま | 子は sandbox の中で uid で得られる物が無い（path・信号・ptrace・socket を全部断る）。専用の uid には root の仲介か set-user-ID の helper が要り、それ自身が新しい攻撃面になる |
| H3 | 断った call の扱い | 既定は子を SIGKILL、試験のために EPERM の flag | 乗っ取った code に「何が断られるか」を探らせない |
| H4 | 対象 | v1 は Files と Settings。compositor の背景の復号（WS138）は v2 の候補 | 背景は利用者が自分で選んだ file だけで、受け取った file を開いただけで復号される縮小表示より危険が小さい。出力が画面の大きさになり起動が遅くなる |
| H5 | Linux・FreeBSD で chroot ができない system（名前空間が無い、`unprivileged_chroot` が 0）で、seccomp・Capsicum だけで復号してよいか | よい | path の call は全部断られる。chroot を必須にすると多くの system で縮小表示が出なくなる |
| H6 | zedBSD の子の image を静的 link に限ってよいか（kernel が `PT_INTERP` のある image を断る）。PDF の代わりの font は image に埋め込む | よい | 子は最初から file を開けないので動的 link は動かない。静的 link の前例はある（compat の library・libpdf・libtruetype を command 用に compile する規則を足す）。font の埋め込みで image は約 1 MB 大きくなる |
| H7 | 子に name space を持たせない（chroot の代わり） | 持たせない | root も cwd も無ければ path は解決できず、`/var/empty` のような場所も要らない。「最低でも chroot」より強い |

### 決定（2026-10-05 夜、ユーザー、Q1 経由）

H1〜H7 を案のとおり承認（`sandbox_spawn` の system call と `include/uapi/sandbox.h` の追加を含む）。p002 は WS130 の後に Queue に入る。

## 9. 段（案）

| Phase | 内容 | 依存 |
| --- | --- | --- |
| p002 | kernel の `sandbox_spawn`（§3）と libc の wrapper、`userland/tests/sandboxtest`（§6 の kernel の 1〜7）。T1 で QEMU | H1・H3・H7 |
| p003 | `keiland-preview`（§4）: 静的 link の規則、font の埋め込み、libpdf の font の口、host 試験。Linux・FreeBSD の 2 つの口（§7） | p002、H2・H5・H6 |
| p004 | Files・Settings を helper に切り替え（§5）、復号の library の link を外す、記録の reader の見直し。T1 で QEMU | p003、H4 |
| p005 | 全文規約の見直し（code を作る WS の最後の段） | p004 |

## 結果

- 2026-10-05 第 1 版（自分を sandbox に入れる `sandbox_enter`）。
- 2026-10-05 第 2 版（ユーザーの review を反映: 子を sandbox の中に起こす `sandbox_spawn`、版つきの構造体と種類の bit の集合、name space 無し、静的 link の子）。
  ユーザーの review 待ち。

## ユーザーの review（2026-10-05）

H1〜H6（第 1 版）の案を見たユーザーの回答（原文）:「設計まで進めて、レビューさせてください。sandboxed vforkっぽいsystem callがいいです。サンドボクシングの権限設定は拡張可能なようにAPI設計したいです。シンプルにしたいです。」→ 第 2 版（この文書）。
