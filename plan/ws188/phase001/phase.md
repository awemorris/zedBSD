<!-- awesome-plan project=zedbsd record=ws188-p001 -->
# ws188-p001: 設計: Settings の OS の読みを libkeiland → compositor → backend へ

Status: 設計の完了（Q1 の判定待ち。q891-i01、P2、2026-10-08。第 3 版: 第 1 版の M1〜M8・m1〜m15、第 2 版の MJ1・MJ2・m1〜m12 を反映。Q1 の判断 3 点を受けた）
Disposition: normal
Parent: [WS188](../ws.md)

## 目的

Settings（`userland/desktop/settings/`）が OS に直に触っている所（WS188 の監査の表）を、
libkeiland の `kl_system_*` → compositor の拡張 `kl_system_manager_v1` → libkeiland-backend の OS ごとの実装、の経路に移す設計を決める。
Guardrail の「app と設定」「Bluetooth と Display も compositor 経由（2026-10-08 ユーザー）」「配置」に従う。

## 移す物と移さない物（決定）

| 所 | 今 | 決定 |
| --- | --- | --- |
| `settings/about.c` の os-release の PRETTY_NAME、`uname`、`sysconf(_SC_NPROCESSORS_ONLN)`、`gethostname`、x86 の `cpuid` の brand string | Settings の起動時に直に読む | **移す**: 新しい `kl_system_machine_v1` の `about`。backend の共有の `kl_backend_machine_read` |
| `settings/look.c` の `statvfs`（`/`・`/home`・`/usr`・`/var`・`/tmp`・`/boot`） | Home・Storage の頁の**描画のたび**に直に読む | **移す**: `kl_system_machine_v1` の `filesystem`。backend の共有の `kl_backend_filesystems_read` |
| `settings/page-users.c` の `getpwuid`・`setpwent`・`getpwent`・`endpwent`・`getgrnam`（wheel・network、uid≥1000・nologin・false の規則） | Users・Languages の頁が直に読む | **移す**: `kl_system_machine_v1` の `user`。backend の `kl_backend_users_read`（OS ごとの管理者の group） |
| `settings/page-sharing.c`・`settings/welcome.c` の `getpwuid(getuid())` | ssh の行の利用者名、Welcome の挨拶の名前 | **移す**: 同じ `user` の自分の行（`SELF`） |
| `settings/page-languages.c` の `KEILAND_SYSCONFDIR/keiland/language` の読み | login の画面の言語 | **移す**: `kl_system_machine_v1` の `login_language`。compositor の `language.c` の読みを共有する |
| `settings/main.c` の `main_open_files`（`posix_spawn(KEILAND_BINDIR "/files")`、Welcome の終わりに Files） | app が app を起動 | **app に残す**（D5）。p003 の許可の表に `settings/main.c` の `main_open_files` を名指しで載せる |
| `settings/storage-scan.c`・`storage-trash.c` の `open(O_DIRECTORY)`・`readdir` | 利用者が選んだ folder の大きさの分析、利用者の Trash | **app に残す**: app 自身の file の機能（Files と同じ）。device・daemon・system の file ではない |
| `settings/look.c` の壁紙の folder の `opendir`（Keiland の data の directory） | Keiland 自身の data | **app に残す** |
| `preview/*/spawn.c` | 縮小画像の sandbox | **app に残す**（2026-10-08 ユーザーの決定）。p003 の許可の表 |
| Settings の外（p003 の検査が見る物、M6） | `libkeiland/settings.c` の `getpwuid`（$HOME が無い時の app の設定の file の場所）、Files の `getpwuid`・`getgrgid`（`files/ui.c`・`dir.c`・`ui-info.c`、file の持ち主の名前）、Files の `statvfs`（`files/ui-home.c`、Home の空き容量） | **判断を Q1 に送る**（案: libkeiland の settings.c は Guardrail の例外「app 自身の設定の file」として許可、Files の持ち主の名前は app の file の機能として許可、Files の statvfs は FILESYSTEMS で移す小さい Phase を WS188 に足す（p002a）か許可の表に載せる）。決まるまで p003 は許可の表の候補として扱う |

## D1: 新しい interface `kl_system_machine_v1`

About・Storage・Users・Languages が読む「この computer の事実」を 1 つの問い合わせの object にまとめる。
既存の object に足す案（monitor の info に os-release、devices に filesystem、account に users）と比べて:

- 読みは全て「頼まれた時に 1 回読んで答える」形で、monitor の連続の標本化や account の変更の job と性質が違う。
- 全ての app が `kl_system_open` で account・devices の object を作るので、そこに users・filesystem を「作った時に送る」形にすると全ての app に getpwent・statvfs が走る。頼んだ app だけに答える問い合わせの方が安い。
- statvfs（Linux の NFS の /home）と getpwent（Linux の NSS）は止まりうるので compositor の event loop で読めない。1 つの thread の job と待ち行列で 4 つ全部を扱える。

**番号（M1）**: 下の番号は仮。manager の version（仮 21）、manager の request（仮 14 get_machine）、能力の bit（仮 0x8000）、libkeiland の `KL_SYSTEM_HAS_MACHINE`（仮 0x10000）、`KL_SYSTEM_CHANGED_MACHINE`（仮 0x8000）、`KL_VERSION`（仮 68）は、
**merge の時に Q1 が次の空きを割り当てる**。競合の相手: ws177-p019（P1、`KL_VERSION 68` を取っている、未 merge）、WS143 p006（Bluetooth の manager の version と request）。実装は番号を 1 か所の define だけで使い、ずらしが 1 行で済むようにする（`keiland.h` の版の一覧の comment は merge の時に Q1 が並べ直す）。

wire（`libkeiland/system/kl-system-protocol.h` に足す）:

```
kl_system_manager_v1
  request 14 get_machine(new_id kl_system_machine_v1)   since version 21 (ws188-p002)

kl_system_machine_v1 (ws188: what Settings reads of the computer)
  request 0 destroy
  request 1 query(uint request, uint what)      what: KL_SYSTEM_MACHINE_ABOUT 0x1, _FILESYSTEMS 0x2,
                                                _USERS 0x4, _LOGIN_LANGUAGE 0x8 (0 or another bit: invalid)
  event   0 parts(uint request, uint what)      the answer of request starts: the parts it holds
  event   1 about(string system, string kernel, string architecture, string processor, string host, uint cpus)
  event   2 filesystem(string path, uint total_high, uint total_low, uint available_high, uint available_low,
                       uint used_high, uint used_low)
  event   3 user(string name, string full_name, string home, uint flags)    KL_SYSTEM_MACHINE_USER_*
  event   4 login_language(string code)           "en", "ja", or "" (no file, or another word)
  event   5 result(uint request, uint applied, uint saved)
```

- 1 つの query の答えは、`parts(request, what)` → 頼まれた部分の event（about は 1 つ、filesystem と user は 0 個以上、login_language は 1 つ）→ `result(request, OK, 0)` の連続。同じ object の別の答えは挟まらない（同じ client の event は compositor が順に書くので、別の object の event も挟まらない）。
  `parts` の後の filesystem・user は、その部分の**全体**（前の答えを置き換える。0 個は「無い」）。
- 失敗の答えは `parts` 無しの `result` だけ: `BUSY`（待ちが一杯）、`UNAVAILABLE`（client の出力の余裕 `KWL_OUTPUT_MAX` に答えが丸ごと入らない）、`INVALID`（what が 0 か未知の bit）、`FAILED`（読みの thread を作れない）。backend がその部分を持たない時は空の部分（about は空の文字列と cpus 0）で OK。
- compositor は読みを thread で行い、同時に 1 つ。読みの最中に来た query は次の読みを待つ。待ちの表は全体で 16、1 つの client で 4（m3）、超えると `BUSY`。
  待ちが複数あれば、次の読みは頼まれた部分の和を読み、各 query にはそれぞれが頼んだ部分だけを答える。object が消えたら待ちから外す。
- user の flags: `KL_SYSTEM_MACHINE_USER_PERSON 0x1`（人の account: uid≥1000 で nobody でなく、shell が nologin・false でない）、`_SELF 0x2`（compositor と同じ uid、すなわち client 自身。compositor は自分の利用者の client にだけ拡張を見せる、WS135）、`_ADMIN 0x4`、`_NETWORK 0x8`。
  自分が人でもあれば 1 行に `PERSON | SELF`（m7）。人の行の後に、人でない自分の行（root など）を 1 行。
- 文字列の長さ（NUL を含む）（M2）: system 128、kernel 96、architecture 32、processor 64、host 64、path 64、name 64、full_name 128、home 256、code 8。
  **name は切らない**: 入らない名前の account は list から外す（compositor の log に数を出す）。full_name・system・kernel・processor・home は UTF-8 の文字の境で切る。full_name は gecos の最初の `,` までで、切るのは backend（今の Settings の `strcspn(..., ",")` と同じ）。
  **home は自分の行だけ**（m8）。他人の行は空。
- 1 つの答えの上限: about 1 + filesystem 8 + user 64（人 63 + 自分）。user の event は最大約 470 byte、答えは最大約 31 KB。人が 63 を超えたら超えた分を外し、compositor の log に出す。

## D2: libkeiland の口（KL_VERSION は仮 68）

`include/keiland/keiland.h`:

```c
#define KL_SYSTEM_HAS_MACHINE		0x10000U	/* kl_system_machine_* (ws188-p002) */
#define KL_SYSTEM_CHANGED_MACHINE	0x8000U		/* an answer of kl_system_machine_query came */

/* The parts of the computer kl_system_machine_query reads. */
#define KL_MACHINE_ABOUT		0x1U
#define KL_MACHINE_FILESYSTEMS		0x2U
#define KL_MACHINE_USERS		0x4U
#define KL_MACHINE_LOGIN_LANGUAGE	0x8U

/* A user's flags. */
#define KL_MACHINE_USER_PERSON		0x1U
#define KL_MACHINE_USER_SELF		0x2U
#define KL_MACHINE_USER_ADMIN		0x4U
#define KL_MACHINE_USER_NETWORK		0x8U

struct kl_machine_about { char system[128]; char kernel[96]; char architecture[32]; char processor[64]; char host[64]; unsigned cpus; };
struct kl_machine_filesystem { char path[64]; uint64_t total; uint64_t available; uint64_t used; };
struct kl_machine_user { char name[64]; char full_name[128]; char home[256]; unsigned flags; };

int kl_system_machine_query(struct kl_system *system, unsigned what, uint32_t *request);
unsigned kl_system_machine_known(const struct kl_system *system);
uint32_t kl_system_machine_serial(const struct kl_system *system, unsigned part);
int kl_system_machine_about(const struct kl_system *system, struct kl_machine_about *about);
size_t kl_system_machine_filesystems(const struct kl_system *system, struct kl_machine_filesystem *list, size_t capacity);
size_t kl_system_machine_users(const struct kl_system *system, struct kl_machine_user *list, size_t capacity);
int kl_system_machine_login_language(const struct kl_system *system, char *code, size_t size);
```

- `kl_system_machine_query`: 0（頼んだ）、ENOTSUP（`KL_SYSTEM_HAS_MACHINE` が無い）、EINVAL。答えの終わりは `kl_system_take_result` の result（OK、BUSY は EBUSY、UNAVAILABLE は ENODEV（既存の写し、system-view.c）、FAILED は EIO、INVALID は EINVAL）と、OK の答えの写しの時の `KL_SYSTEM_CHANGED_MACHINE`。
- 受け: `parts` で受けの作業の list（その部分の分）を空にし、その request と部分を覚える。event は作業の list に貯める。同じ request の `result(OK)` で、`parts` の部分だけを表示の view に写し、部分ごとの serial を 1 進め、known に bit を足す。`parts` の無い result（失敗）・別の request の result・`parts` の後に来た別の `parts`（途中で切れた答え）では写さない（作業の list は捨てる）。
- `kl_system_machine_serial(system, part)`（M8）: 部分ごとの写しの回数（0 は未だ）。app は serial の変化で、変わった部分だけを写し直す。
- `kl_system_machine_known` は 1 回でも OK の答えが来た部分の bit。`_about`・`_login_language` は known でなければ ENOENT。list の 2 つは写した数（known でなければ 0）。
- `kl_system_open` は manager が仮 21 以上の時に machine の object を作る（作っただけでは何も読まれず、最初の状態は無いので open の roundtrip は増えない）。
- exports.map は `python3 userland/desktop/libkeiland/exports.py` で作り直し、`--check` で確かめる（m12）。新しい struct は新しい関数だけが使うので既存の ABI は変えない。

## D3: compositor

| file | 変更 |
| --- | --- |
| `wayland/machine-shell.c`（新） | `kwl_machine_create`（get_machine）、`kwl_machine_request`（query・destroy）、`kwl_machine_tick`（読みの終わりを見て答え、次の読みを始める）、`kwl_machine_gone`（object の消滅で待ちから外す）、`kwl_machine_close`（終わり: 読みが done なら join、まだなら detach して進む。NFS で止まった thread で logout を止めない、M5） |
| `wayland/machine-wait.c`・`.h`（新、Wayland と thread を持たない純粋な部分、m9） | 待ちの表: 足す（全体 16・client 4 の上限、BUSY）、object の消滅で外す、次の読みの部分の和、読みの終わりに答える待ちの取り出し。host 試験で直に確かめる |
| `wayland/kwl.h` | `KWL_SYSTEM_MACHINE` の kind と宣言 |
| `wayland/system.c` | manager の request の get_machine の分岐、`kwl_system_bind` の能力の bit、`kwl_system_tick` から `kwl_machine_tick`、`kwl_system_close` から `kwl_machine_close` |
| `wayland/protocol.c` の kind の振り分け、`wayland/objects.c` の型の gone | `KWL_SYSTEM_MACHINE` の request と gone（m1） |
| `wayland/language.c`・`.h` | `kwl_language_system_word(char *word, size_t size)`: file の 1 行目の改行を外した語をそのまま返す（今の `kwl_language_system` の読みと同じ。greeter の言語の決め方を変えない、m5）。`kwl_language_system` はこれを使う。machine の答えを作る側だけが `en`・`ja`・`""` に写す（Settings の今の規則: 空白までの語が en か ja） |
| `wayland/Makefile`・`Makefile.linux`・`Makefile.freebsd` | 3 つ全部に `machine-shell.c`・`machine-wait.c`（M1 の検査） |

- 読みの job: 1 つの thread。入力は読む部分の bit、出力は `struct kl_backend_machine`・filesystem の配列（8）・user の配列（64）・login の言語の語。lock と done の形は `system.c` の `system_job` と同じ（thread が done を最後に立て、event loop は done を見て join してから出力を読む）。
- 待ちの処理: query が来た時、読みが無ければ待ちに足してすぐ読みを始める。読みの最中なら待ちに足す（次の読み）。読みの終わりで、読みを始めた時に待ちにあった query に答え、残りがあれば次の読みを始める。
- log: `KWL SYSTEM machine query client=N request=R what=W`、`KWL SYSTEM machine answer client=N request=R what=W result=K users=U filesystems=F skipped=S`（中身の名前は出さない）。
- **管理の後の読み直しは頼みで**: administer の成功の後に自動で送らない。Settings が結果の後に query する。

## D4: libkeiland-backend（3 OS）

`keiland-backend.h` に:

```c
struct kl_backend_machine { char system[128]; char kernel[96]; char architecture[32]; char processor[64]; char host[64]; unsigned cpus; };
int kl_backend_machine_read(struct kl_backend_machine *machine);
struct kl_backend_filesystem { char path[64]; uint64_t total; uint64_t available; uint64_t used; };
size_t kl_backend_filesystems_read(struct kl_backend_filesystem *list, size_t capacity);
#define KL_BACKEND_USER_PERSON 0x1U  /* _SELF 0x2U, _ADMIN 0x4U, _NETWORK 0x8U */
struct kl_backend_user { char name[64]; char full_name[128]; char home[256]; unsigned flags; };
size_t kl_backend_users_read(struct kl_backend_user *list, size_t capacity, unsigned *skipped);
```

| 関数 | 置き場所 | 中身 |
| --- | --- | --- |
| `kl_backend_machine_read` | 共有 `libkeiland-backend/machine/machine.c`（3 OS とも同じ POSIX） | `/etc/os-release`、無ければ `/usr/lib/os-release` の PRETTY_NAME（今の `se_about_pretty_name` の parser を移す）、`uname` の sysname・release・machine、`sysconf(_SC_NPROCESSORS_ONLN)`、`gethostname`、x86（`__x86_64__`・`__i386__`）の cpuid の brand string（他の arch は空）。どれかが読めなくても他は返す。About の Processors・Computer name の正はこの答え（monitor の info の host・CPU の数は System Monitor の物、m14） |
| `kl_backend_filesystems_read` | 共有 `libkeiland-backend/machine/filesystems.c` | 今の 6 つの場所を `statvfs`、`f_blocks` が 0 の物は飛ばす。同じ file system の重複は `stat` の `st_dev` で除く（zedBSD の `f_fsid` は disk の無い mount で番号が重なりうる、Linux の一部で 0）。大きさの計算は今と同じ |
| `kl_backend_users_read` | OS ごと: zedBSD は `account-zedbsd.c`、Linux は新しい `libkeiland-backend-linux/users-linux.c`、FreeBSD は新しい `libkeiland-backend-freebsd/users-freebsd.c`。どれも共有の `libkeiland-backend/machine/users.c` の `kl_backend_users_posix(list, capacity, skipped, admin_groups)`（`backend-private.h` に宣言）を呼ぶ | 共有の部分: group を `getgrnam_r` で先に写し（m4）、`setpwent`/`getpwent`/`endpwent` で人の account を、`getpwuid_r(getuid())` で自分を。人の規則は今の `users_person`、group の規則は今の `users_member`（主 group か member）。network の group は 3 OS とも `network`（今と同じ。Arch にもある、m6）。管理者の group: zedBSD・FreeBSD は `wheel`、Linux は `sudo` と `wheel` |

- thread の前提（m4）: compositor の中で `getpwent` の列挙を使うのは machine の読みの thread だけ（greeter の `greeter.c` の getpwent は login の画面の compositor で、そこでは利用者の system の拡張は見えない、WS135）。他の所の `getpwuid`・`getgrnam`（power・sharing）は列挙の状態に触れない。glibc の static の領域の共有を避けるため、group と自分は `_r` の形で読む。
- build の list: `libkeiland-backend-zedbsd/sources.mk`、`libkeiland-backend-linux/Makefile.linux`、`libkeiland-backend-freebsd/Makefile.freebsd` に共有の 3 file と OS の file を足す。
- 共有の file は OS の macro の block を持たない（L1、共有の `libkeiland-backend/` も見られる）。arch の macro は L1 の対象外。

**意図した振る舞いの変化（m6・m13）**:
- Linux で `sudo` の member が Administrator と出る（今は wheel だけを見るので Standard）。
- 管理者の判定は自分の行の `ADMIN` で行う。uid が 1000 未満の管理者（root で動く Settings など）でも管理の card が出る（今は人の行の中の自分だけを見る）。
- Keiland でない compositor・古い compositor（`KL_SYSTEM_HAS_MACHINE` 無し）では About の値・Users・Storage が空、Languages の login の card が出ない（境界の規則による後退）。

## D5: Files の起動（`main.c` の `main_open_files`）は app に残す

- 他の app も別の desktop の app を起動している（Files の open-with、PDF viewer の Notes）。app が別の app を起動するのは app の機能で、OS の device・daemon・system の file の操作ではない。Settings だけを compositor の起動の要求に変えても境界は揃わない。
- compositor の起動の要求（compositor が activation の token を付けて起動する、今の posix_spawn は token を持たない）は全ての app に効く別の設計で、Future Work の候補として Q1 に送る。
- p003 の検査の許可の表は `settings/main.c` の `main_open_files` を名指しで載せる。

## D6: Settings の側（p002 の実装の形）

- **要求と写し**: Settings は自分の request の番号と頼んだ部分を覚え、写しは部分ごとの serial（`kl_system_machine_serial`）が変わった部分だけ行う（M8）。log もその部分の写しの時だけ出す。
- **やり直し**（M7）: 部分ごとに「出している request」と「最後に出した時刻」を持つ。失敗の result（EBUSY・ENODEV・EIO）の後は 2 秒の間を置いて、その部分を要る頁が描かれる時にやり直す。known でない USERS・LOGIN_LANGUAGE・ABOUT は、要る頁を描く時に同じ間隔で頼み直す。
- **起動**: `se_system_open` の後に `query(ABOUT | USERS | LOGIN_LANGUAGE)` を 1 回。
- **About**（m2）: `settings/about.c` を消す（Q1 に削除を依頼、3 つの Makefile から外す）。ABOUT の写しは `se_about` の system・kernel・machine・processor・host・cores の 6 つの欄だけを上書きし、window が入れる graphics・display と monitor の memory には触れない。答えの前は値の行が空（今の「読めない値は空」と同じ見え）。
- **Storage・Home**: `se_look_volumes` は statvfs をやめ、「FILESYSTEMS の最後の写しが 2 秒より古く、出している request が無ければ query」と「最新の写し」だけにする。最初の答えまでは Storage の頁は「Reading the disks…」、Home の Storage の tile は空き容量を出さない。
- **Users**（M4）: `users_read`・`se_users_reload` は `query(USERS | LOGIN_LANGUAGE)` を出すだけ。USERS の写しで `se_users` の行（`PERSON` の行、`SE_USERS_LIST_MAX` を 64 に）と自分の name・full_name・home（`SELF` の行）を作る。管理の選択は**名前**で持ち、写しで名前で選び直す（無くなれば選択を外す）。USERS の request を出している間は、行の選択と管理の操作（Apply）を止める。
- **Languages**（M3）: `languages_read_system` は LOGIN_LANGUAGE の写し。`chosen` は system の値が最初に分かった時と変わった時だけ設定し、利用者が選びかけた値を消さない（M8）。管理の結果が OK なら `query(USERS | LOGIN_LANGUAGE)` を出し、結果の log の行 `LANGUAGES system result request=N errno=0 system=K` はその答えの写しの時に出す（今と同じ文・同じ値。AAT の login-language の正解 `system=1` を保つ）。失敗の結果は今と同じくすぐ出す。
- **Sharing・Welcome**: 利用者名・挨拶の名前は SELF の行（答えの前は今の「you」「there」）。写しの時に描き直す。
- **試験の log の行**（今と同じ文で、その部分の写しの時に 1 回）: `ABOUT system=… kernel=… machine=… cores=… host=…`、`USERS account name=…`、`USERS list count=N`（N は PERSON の行の数、m7）、`LANGUAGES system language=…`、上の `LANGUAGES system result …`。
- **AAT の改訂**（m10）: `tests/scenarios/apps/settings/about.md` の paths（about.c を外し machine の file を足す）、about・users-page・pages（Storage）の撮影の前に log の行（`ABOUT`・`USERS list`）を待つ手順。
- Users の行の数の上限: wire は 64、Settings は 64 を出す（m7）。

## 試験（p002）

- host:
  - 共有の backend の 3 file: os-release の parser（`plan/ws089/tests/host-about.c` の case を `plan/ws188/tests/` に写して新しい関数へ。ws089 の host-about は Q1 に削除の依頼、m11）、filesystems（host の `/` が出る、重複が無い）、users（host の passwd で自分の行が SELF、人の規則、`,` の切り、長い名前を外す）。
  - compositor の `machine-wait.c`: 足す・上限の BUSY・消滅で外す・和・取り出し（m9）。
  - libkeiland の受け: parts・event・result の順、失敗の result で写さない、途中の `parts` で前を捨てる、別の request の result で写さない、置き換え、serial（Wayland 無しの stand-in か受けの関数を直に）。
- build: zedBSD の `$(BUILD)/bin/settings`・`$(BUILD)/bin/wayland`・`$(BUILD)/dynamic/libkeiland.so`（warning 0）、keiland-linux の settings・wayland・libkeiland（warning 0）。FreeBSD は guest が要るので T1 の依頼に含めるか未実施と書く。
- T1: AAT の apps.settings.about・users-page・login-language・manage-users・pages（Storage を含む）。
- p003 の検査が Settings に通る。

## 未決・確認（Q1 へ）

1. 番号の割り当て（D1、merge の時）。
2. Settings の外の getpw*・statvfs の扱い（M6、表の最後の行）。
3. Future Work の候補: compositor の app の起動の要求（D5）。

## design-reviewer

- 2026-10-08 第 1 版のレビュー（design-reviewer、読むだけ）: major 8（M1 KL_VERSION 68 の衝突、M2 名前の欄 32 で切れた名前が別の利用者を指す、M3 login-language の AAT の `system=1`、M4 行の番号の選択、M5 読みの thread の join と NFS、M6 Settings の外の getpw*・statvfs、M7 失敗の後のやり直し、M8 変わった部分が分からない）、minor 15（m1 変更の場所の抜け、m2 About の写しの範囲、m3 待ちの上限の共有、m4 getgrnam の thread、m5 言語の読みの 2 通り、m6 Linux・FreeBSD の表示の変化、m7 行と数の決まり、m8 他人の home、m9 待ちの host 試験、m10 AAT の撮影の時、m11 ws089 の試験の範囲、m12 exports.py は生成、m13 古い compositor の後退、m14 monitor と重なる値、m15 D5 は妥当・Terminal の例は不適）。
- 第 2 版で全てを反映: M1 は番号を仮にして Q1 の割り当てへ、M2 は欄 64・128 と名前を切らずに外す、M3 は結果の log を答えの写しで出す、M4 は名前の選択と読み直しの間の操作の停止、M5 は `kwl_machine_close` の detach、M6 は Q1 の判断へ、M7 は部分ごとの request と時刻のやり直し、M8 は部分ごとの serial。m1〜m15 は各節に。

## 第 3 版（2026-10-08、第 2 版のレビューと Q1 の判断を受けて）

Q1 の判断（2026-10-08）:
1. 番号: **KL_VERSION 69**（68 は ws177-p019 が main に入れた）。manager の version 21・request 14・能力の bit 0x8000・`KL_SYSTEM_HAS_MACHINE 0x10000`・`KL_SYSTEM_CHANGED_MACHINE 0x8000` は WS188 に割り当て（WS143 p006 はその次）。merge の前に main を取り込んで衝突を確かめる。
2. p003: `libkeiland/settings.c` の getpwuid と Files の getpwuid・getgrgid は許可の表。**Files の statvfs（Home の空き容量）は machine の FILESYSTEMS で移す小さい Phase ws188-p002a を足す**。
3. Future Work F-085（compositor の app の起動の要求、activation の token 付き）。D5 は了解。

第 2 版のレビューの反映:
- **MJ1（止まった読み）**: 読みの job は読みごとに heap に作り、thread は detach で作る（join しない）。job の lock は static の 1 つの mutex。thread は終わりに lock の下で、見捨てられていれば job を free し見捨ての数を減らし、そうでなければ done を立てる。event loop は 1 pass ごとに done を見る。**10 秒**で終わらない読みは見捨てる: その読みの query に `result(FAILED)` を答え、job に見捨ての印を付けて手放す。見捨てた thread が 1 つ残っている間に来た query は読まずに `FAILED`（thread が増え続けない）。`kwl_machine_close` も同じく見捨てるだけで待たない（static の mutex は壊さない）。Settings の側も request に 15 秒の上限を置き、過ぎたら出していない扱いに戻す（Users の操作の停止が永久に続かない）。
- **MJ2**: UNAVAILABLE は ENODEV。Settings のやり直しは EBUSY・ENODEV・EIO の後。
- m1: Languages の結果の log は、管理の OK の後に**必ず新しい** `query(USERS | LOGIN_LANGUAGE)` を出し、`kl_system_take_result` がその request を errno 0 で返した時の view の値で出す（compositor は読みの最中の query を次の読みに回すので、値は管理の後のもの）。
- m2: Settings の main の poll の timeout に machine のやり直しの時刻を足す（`se_machine_wait`）。
- m3: 自分の行は必ず入る（人の上限は capacity − 1、自分が人の中にいれば SELF を付け、いなければ最後に足す）。自分の名前が 64 byte を超える時は自分の行が無い（Settings は「you」「there」のまま）。
- m4: group と自分の行は 4096 から倍にして上限 64 KiB の buffer で `_r` の形で読む。上限を超えた group は無い物とする。
- m5: 待ちの entry は「読みに入った」印を持ち、読みの最中に足された query は次の読み。上限（全体 16・client 4）は読みの最中の entry も数える。object の消滅は entry を外す（読みの最中でも）。
- m6: libkeiland は parts に無い部分の event・parts の前の event・容量を超えた event を無視する。`kl_system_machine_serial` は 1 つの部分の bit 以外に 0。
- m7: UNAVAILABLE の判定は parts・各 event・result の合計（header を含む）で事前に行う。途中の送りの失敗は `kwl_emit` の既存の扱い（client を fatal）に任せる。
- m8: kernel の欄は 160（utsname の sysname と release の 65+65）。Settings の `se_users.home` を 256 に。
- m9: root・sudo で動く Settings は compositor に system の拡張を見せてもらえない（他の uid）ので About・Users・Storage が空になる（今は自分で読めた）。意図した後退として記す。D4 の「root で動く Settings でも管理の card」は誤りで、管理者の判定は自分の行の `ADMIN`（uid が 1000 未満の管理者の account でも、compositor と同じ uid なら出る）。
- m10: AAT の manage-users の「一覧に出る」も `USERS list count` の行を待つ。
- m11: libkeiland の view が約 58 KB 増える（全ての app）。受け入れる（kl_system は heap、app の大きさに比べて小さい）。
- m12: ws.md の射影は Q1。
