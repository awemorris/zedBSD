<!-- awesome-plan project=zedbsd record=ws145-design -->

# WS145 の設計: 印刷（ws145-p001、第 3.1 版 2026-10-05 P2 g15、q754）

[WS145](ws.md) の単一目標「app が libkeiland に PDF の場所を渡すと network の printer（IPP か LPD）で印刷される。一覧と既定の printer を libkeiland から compositor 経由で取れ、Settings の Printers の頁で IP address・port・protocol を設定できる」の設計。ユーザーの指示（2026-10-04 夜、ws.md に原文）に従う。

第 2 版は初版への敵対的レビュー（[phase001](phase001/phase.md) に要旨）の重大 7・中 11・軽 8 の指摘を、第 3 版は第 2 版への 2 回目のレビューの重大 3・中 12・軽 13 を、第 3.1 版は第 3 版への 3 回目のレビュー（重大なし）の中 9 と主な軽を反映した。番号は拡張の protocol を **version 10**、libkeiland を **KL_VERSION 34** とする（version 9・KL 33 は ws132-p009 の媒体の情報が使った。merge の時に他の WS と重なれば Q1 が詰め直す）。

## 0. 判断の要点

| 番号 | 決めたこと（案） | 理由 | ユーザーの判断 |
| --- | --- | --- | --- |
| D1 | printer の daemon `keiland-printd` は**利用者の権限**で動く。compositor の libkeiland-backend が必要な時に起動し、仕事が無くなれば合意の手順（§5.1）で終わる。root の daemon・setuid・init の service（ws.md の範囲 3 が想定した「WS002 の service の仕組み」）を使わない。 | network の printer に送るだけなら特権が要らない。root の daemon の口を足さない。ユーザーの指示「libkeiland-backend がプリンタデーモンが未起動なら起動」に合う。ws.md の範囲 3 の記述はこの版に合わせて直す（Q1 に報告済み）。 | 要らない（特権の口を足さない） |
| D2 | printer の設定は**利用者ごと**（`~/.config/keiland/printers.conf`、書くのは compositor だけ）。 | 管理者の権限も root の書き込みも要らない。 | **要確認**: 複数の利用者で printer を共有したいか（共有なら system の設定の段を別に設計し、root の口の承認を取る） |
| D3 | 設定の項目は address・port・protocol だけ。IPP の resource の path は `/ipp/print` → `/ipp` → `/` の順に試し、通った path を設定に覚える。LPD の queue の名前は `lp`。名前は足した時に IPP で問い合わせた `printer-info`（無ければ `printer-make-and-model`、どちらも無ければ「address (IPP)」）を設定に覚える。 | 指示どおり 3 項目に保つ。多くの printer で通る既定を選ぶ（`/ipp/printer`・`/ipp/port1` などの機種は通らない）。 | **要確認（小）**: IPP の path と LPD の queue の名前を「詳しい設定」として入力できるようにするか |
| D4 | 文書は PDF だけ（段 1）。printer が PDF を受けなければ job は失敗（format）。 | 指示どおり。 | **要確認**: 受け入れの printer の機種。多くの家庭の printer は PDF を受けず PWG raster・PCL が要る。その機種なら filter の WS の PDF → PWG raster（libpdf の rasterizer を使える）を受け入れの前に入れる |
| D5 | Linux・FreeBSD の Keiland も同じ `keiland-printd`。CUPS は使わない（外部の package に依らない）。 | 3 つの OS で同じ code。 | **要確認（小）**: Linux・FreeBSD で CUPS に既にある printer を一覧に出すか（出すなら CUPS の IPP（localhost:631）を読む段を別に足す。CUPS の library は使わない） |
| D6 | app の Print の menu は範囲の外。WS145 は libkeiland の口と試験の client（`userland/tests/printtest`）まで。 | 単一目標は「libkeiland に渡すと印刷される」。 | **要確認（小）**: PDF Viewer の File > Print を WS145 に足すか（足すなら p007） |
| D7 | printer に利用者の login 名を送る（IPP の `requesting-user-name`、LPD の `P` と `H` の host 名）。 | printer の job の一覧で誰の job か分かる（IPP・LPD の通常）。 | **要確認（小）**: 送らない（固定の "kei"）方がよいか |
| D8 | spool の上限: 1 文書 256 MiB、合計 512 MiB、job 16 個（待ち・送信中の合計）。超えた依頼は busy で断る。 | spool は RAM の tmpfs（`/run/user/UID`）なので、上限なしでは利用者の client が RAM を使い切れる。代わりの案: 複写せず、app の file の fd を printd が持ち続けて直接送る（RAM を使わないが、送る前に app が file を変えると変わった中身が印刷される）。 | **要確認（小）**: 上限の値と、複写か直接送るか |
| D9 | printer と job の一覧は、同じ利用者の全ての app に見え（job の題名を含む）、どの app も job を取り消せる。 | 利用者ごとの設定で、同じ利用者の app は同じ権限（Settings が一覧と取り消しを出す）。 | **要確認（小）**: 他の app の job の題名を見せないか |

判断の期限: D2（共有）は p003 の backend の形を変えるので p003 の前。D4（受け入れの機種）は filter の WS の順を変えるので p006（受け入れ）の前。他は p004 の前。

外部の実装は取り込まない（IPP・LPD の符号化は RFC から自分で書く）。HAL・UAPI・toolchain には触れない。zedBSD の kernel には IPv6 が無い（`include/uapi/socket.h`、EAFNOSUPPORT）ので、address は IPv4 の literal と host 名（DNS で引ける物）に限る。

## 1. 全体の流れ

```
app ──kl_system_printers_print(system, printer, path, title, &request)──▶ libkeiland
       libkeiland: path を O_RDONLY|O_NONBLOCK|O_NOCTTY|O_CLOEXEC で開く → fstat で S_ISREG と大きさ → pread で先頭 1024 byte に "%PDF-"
       → title を検める → fd を送る（libwayland は 'h' を dup するので marshal の後に自分の fd を閉じる）
libkeiland ──kl_system_printers_v1.print(request, printer, title, fd)──▶ compositor（system.c）
       compositor: 検証の前に必ず zwl_take_fd で fd を取る。不正なら fd を閉じて result（INVALID）
       → kl_backend_print_submit（成否に関わらず fd の所有を受け取る）
backend（userland/desktop/libkeiland-backend/print/、OS に依らない）: job の ID を振る → queued(request, job) と job の一覧の変化
       → printd が無ければ起動 → "JOB ..." + fd（SCM_RIGHTS、nonblocking、MSG_NOSIGNAL）
printd: fstat・pread で検めて spool に複写 → "ACCEPTED job" → IPP / LPD で送る → "STATE job ..."
backend ──job の状態──▶ compositor ──job・done──▶ 全ての printers の object
```

- app は OS の口も daemon への接続も持たない（Guardrail「app と設定」）。libkeiland は file を開いて fd を作るだけ。
- path でなく fd を compositor に渡す: app が読める file だけが印刷され、compositor が app の相対 path を解いたり file を読んだりしない。app の API は指示どおり path を受ける。
- **fd の所有**: compositor は print の要求で `zwl_take_fd` を最初に呼ぶ。-1（fd がまだ届いていない）なら何も確保せず副作用も起こさずに EAGAIN を返す（`shm.c` と同じく、要求は fd が届いてからやり直される）。fd を取った後に EPROTO・ENOMEM で返す経路は、その fd を閉じてから返す。検めで断る時（INVALID）も fd を閉じる。`kl_backend_print_submit` を呼んだ時点で fd は成否に関わらず backend の物（compositor は以後触らない）。backend は printd の ACCEPTED・REJECTED か、依頼を諦めるまで fd を持ち、その時に閉じる。backend が同時に持つ fd は 16 まで（超えれば BUSY）。backend が printd に送るのは自分の表の fd だけ。

## 2. libkeiland の口（KL_VERSION 34、`<keiland.h>`）

既存の kl_system_* と同じ形: 状態は get、変化は `kl_system_dispatch` の changed の bit、依頼は request の番号と `kl_system_take_result` の errno。

```c
#define KL_SYSTEM_HAS_PRINTERS      0x200U   /* kl_system_printers_*（KL_VERSION 34） */
#define KL_SYSTEM_CHANGED_PRINTERS  0x100U   /* printer・job の一覧 */
#define KL_PRINTER_IPP              1U
#define KL_PRINTER_LPD              2U
#define KL_PRINTER_DEFAULT          0x1U     /* flags */
#define KL_PRINTERS_MAX             16U
#define KL_PRINT_JOBS_MAX           32U      /* 待ち・送信中 16 と、終わった最後の 16 */
#define KL_PRINTER_HOST_MAX         64U
#define KL_PRINTER_NAME_MAX         128U
#define KL_PRINT_TITLE_MAX          128U
#define KL_PRINT_DETAIL_MAX         32U

struct kl_printer {
	uint32_t id;                        /* backend が振る（1 から、設定の file に残る） */
	unsigned protocol;                  /* KL_PRINTER_IPP / _LPD */
	char host[KL_PRINTER_HOST_MAX];     /* IPv4 の literal か host 名 */
	unsigned port;                      /* 1〜65535（IPP 631、LPD 515 が既定） */
	char name[KL_PRINTER_NAME_MAX];
	unsigned flags;
};

/* job の状態 */
#define KL_PRINT_QUEUED     1U   /* 受け付けた、送る前 */
#define KL_PRINT_SENDING    2U
#define KL_PRINT_WAITING    3U   /* printer が受け取り、処理を待つ・処理中（IPP の pending・processing） */
#define KL_PRINT_DONE       4U
#define KL_PRINT_FAILED     5U
#define KL_PRINT_CANCELLED  6U

struct kl_print_job {
	uint32_t job;                       /* backend が振る（compositor の寿命の間一意） */
	uint32_t printer;
	unsigned state;
	char title[KL_PRINT_TITLE_MAX];
	char detail[KL_PRINT_DETAIL_MAX];   /* §5.6 の語の一つ、または "" */
};

size_t kl_system_printers_get(const struct kl_system *system, struct kl_printer *printers, size_t capacity);
size_t kl_system_print_jobs_get(const struct kl_system *system, struct kl_print_job *jobs, size_t capacity);
int kl_system_printers_add(struct kl_system *system, unsigned protocol, const char *host, unsigned port, uint32_t *request);
int kl_system_printers_remove(struct kl_system *system, uint32_t printer, uint32_t *request);
int kl_system_printers_set_default(struct kl_system *system, uint32_t printer, uint32_t *request);
int kl_system_printers_print(struct kl_system *system, uint32_t printer, const char *path, const char *title, uint32_t *request);
int kl_system_print_job_of(const struct kl_system *system, uint32_t request, uint32_t *job);
int kl_system_print_cancel(struct kl_system *system, uint32_t job, uint32_t *request);
```

- `printer` に 0 を渡すと既定の printer（無ければ result が EINVAL）。
- `print` の libkeiland の中の検め: 開けない（errno のまま）、正規の file でない（EINVAL）、大きさ 0 か 256 MiB 超（EFBIG）、先頭 1024 byte に `%PDF-` が無い（EINVAL）。
- **title の規則**（層ごとに一つ）: libkeiland は UTF-8 として不正なら EINVAL で断り、C0（U+0000〜U+001F）・DEL・C1（U+0080〜U+009F）は空白に置き換え、127 byte を超えれば文字の境界で切ってから送る（app は長い題名を気にせず渡せる）。compositor と printd は置き換えも切り詰めもせず、規則に外れる title（不正な UTF-8、制御文字、127 byte 超）を INVALID・REJECTED で断る（libkeiland を通らない client への備え）。printer の名前（NAMED）は printd が libkeiland と同じ規則で直す。
- `kl_system_print_job_of`: print の result の前に compositor が送る `queued(request, job)` を、results の ring（`SYSTEM_VIEW_RESULTS` 32）と同じ大きさの (request, job) の表に覚え、依頼した app が自分の job の ID を知る。続けて印刷しても取りこぼさない（1 件だけの `busy_program` の形にしない）。1 で job、0 で無し。
- result の errno: 0、EINVAL（INVALID）、EBUSY（BUSY: 上限）、ENOTSUP（UNSUPPORTED）、EIO（FAILED）。

## 3. 拡張の protocol（kl_system_manager_v1 version 10）

```
kl_system_manager_v1
  request 9 get_printers(new_id kl_system_printers_v1)       since version 10 (ws145)
  capabilities の bit KL_SYSTEM_CAPABILITY_PRINTERS 0x200（printd が実行できる時だけ（`access(X_OK)`、`kl_backend_account_can_administer` と同じ形）。login・lock の画面の compositor は出さない）

kl_system_printers_v1
  request 0 destroy
  request 1 add(uint request, uint protocol, string host, uint port)
  request 2 remove(uint request, uint printer)
  request 3 set_default(uint request, uint printer)
  request 4 print(uint request, uint printer, string title, fd document)
  request 5 cancel(uint request, uint job)
  event   0 printer(uint id, uint protocol, string host, uint port, string name, uint flags)
  event   1 job(uint job, uint printer, uint state, string title, string detail)
  event   2 done(uint serial)                 the printers and jobs before it are the whole state
  event   3 queued(uint request, uint job)    before print's result, when the job was taken
  event   4 result(uint request, uint applied, uint saved)    applied is a KL_SYSTEM_RESULT_* number
```

- 作られた時に全ての printer と job と done。変化のたびに全体と done（devices と同じ）。
- add・remove・set_default は backend の writer の thread が flock の下で読み直し、そこで id を振って（`next-id`）書き、**書き終えてから** result（saved を含む）と printer の event を出す（2 つの session が同時に足しても id が重ならない）。16 個を超えれば BUSY、同じ protocol・host・port が既にあれば INVALID。IPP の名前と path は後から printd の NAMED で埋まり、printer の event で変わる。NAMED が取れなければ名前は「address (IPP)」、path は `/ipp/print`（print の時に path を探し、見つけた path を printd が `PATH` で返して設定に覚える）。
- 利用者ごとの設定なので、その compositor の client なら誰でも add・remove・set_default・print・cancel ができる。
- compositor の検め（protocol の入口）: protocol は 1・2、host は `[A-Za-z0-9.-]` で 1〜63 byte（IPv4 の literal を含む）、port は 1〜65535、title は §2 の規則（`SYSTEM_WIRE_TEXT_MAX` を超える string は従来どおり protocol の error）、printer・job は在る物。外れれば fd を閉じて result INVALID（protocol の error にしない）。
- 定数は `kl-system-protocol.h` に `KL_SYSTEM_MANAGER_GET_PRINTERS 9U`（libkeiland の `system_manager_requests[]` は配列の添字が opcode なので 9）、`KL_SYSTEM_PRINTER_IPP`・`_LPD`、`KL_SYSTEM_PRINT_*` の状態、`KL_SYSTEM_SINCE_PRINTERS 10` を置く（libkeiland の `KL_PRINTER_*` と同じ値）。
- 古い組み合わせ: version 9 以前の compositor では libkeiland の capability に PRINTERS が無く、全ての call が ENOTSUP。古い libkeiland（version 9）は get_printers を送らない。
- printers の object は他の object と同じく `kl_system_open` で作られる（全ての app に 1 つずつ。object は状態を送るだけで、printd も問い合わせも起こさない）。

## 4. compositor と libkeiland-backend

- compositor（`wayland/system.c` に printers の object）: 要求を backend に渡し、backend の changed の bit で全ての printers の object に state と done、依頼した object に queued と result。
- **request の番号**: client が振る番号は client ごとに重なるので、backend には渡さない。volumes と同じく **backend が一意の番号を振って返し**、compositor は devices と同じ待ちの表（backend の番号、client の番号、object の ID、client の request の番号。`wayland/system.c` の devices の待ちの表の形、8 個）で突き合わせる。表が満杯なら（print なら fd を閉じてから）BUSY を返す。
- **backend の口**（volumes の型、`keiland-backend.h`）:
  ```c
  /* backend の printer と job（libkeiland の kl_printer・kl_print_job と同じ項目、path は IPP の path か LPD の queue） */
  struct kl_backend_printer {
  	uint32_t id;
  	unsigned protocol;
  	char host[64];
  	unsigned port;
  	char path[64];
  	char name[128];
  	unsigned is_default;
  };
  struct kl_backend_print_job {
  	uint32_t job;
  	uint32_t printer;
  	unsigned state;      /* KL_BACKEND_PRINT_QUEUED ... _CANCELLED、ほかに内部の CANCELLING（外には SENDING などのまま見せる） */
  	char title[128];
  	char detail[32];
  };
  struct kl_backend_print *kl_backend_print_open(const char *config_path, const char *runtime_dir, const char *program);
  void kl_backend_print_close(struct kl_backend_print *print);
  int  kl_backend_print_can(const struct kl_backend_print *print);       /* printd が実行できるか（capability） */
  int  kl_backend_print_update(struct kl_backend_print *print, unsigned *changed);   /* compositor の tick（volumes と同じ）ごと、KL_BACKEND_PRINT_CHANGED_LIST・_RESULT */
  size_t kl_backend_print_printers(const struct kl_backend_print *print, struct kl_backend_printer *list, size_t capacity);
  size_t kl_backend_print_jobs(const struct kl_backend_print *print, struct kl_backend_print_job *list, size_t capacity);
  int  kl_backend_print_add(struct kl_backend_print *print, unsigned protocol, const char *host, unsigned port, uint32_t *request);
  int  kl_backend_print_remove(struct kl_backend_print *print, uint32_t printer, uint32_t *request);
  int  kl_backend_print_set_default(struct kl_backend_print *print, uint32_t printer, uint32_t *request);
  int  kl_backend_print_submit(struct kl_backend_print *print, uint32_t printer, const char *title, int fd, uint32_t *request, uint32_t *job);
  int  kl_backend_print_cancel(struct kl_backend_print *print, uint32_t job, uint32_t *request);
  int  kl_backend_print_take_result(struct kl_backend_print *print, uint32_t *request, int *error, unsigned *saved);
  ```
  返す値は errno（0・EINVAL・EBUSY・ENOTSUP・EIO）で、compositor が `system_result_of` で KL_SYSTEM_RESULT_* に変える。`saved` は add・remove・set_default で設定の file に書けたか（print・cancel は 1）。saved が 0 の時は既存の settings と同じく KL_SYSTEM_RESULT_NOT_SAVED を返し、libkeiland はその errno の写し方に従う。submit は fd の所有を必ず受け取る。
- **設定の file**（backend の共通の code）: `~/.config/keiland/printers.conf`。形:
  ```
  # Keiland printers
  next-id 3
  printer 1 ipp 192.168.1.20 631 /ipp/print Office-Printer
  printer 2 lpd 192.168.1.30 515 lp 192.168.1.30 (LPD)
  default 1
  ```
  （id・protocol・host・port・IPP の path か LPD の queue・名前（行の残り、§2 の規則で直した物））。書きは settings-store と同じく writer の thread で、`flock` を取ってから**読み直し、変えた所（足す・消す・既定・名前と path）だけを当てて**一時 file から rename する（同じ利用者の別の session の変更を消さない）。id は `next-id` から振り、消した id は使い直さない（古い id を持つ app が別の printer に印刷しない）。他の session の変更は file の mtime が変わった時に読み直す（printers を送る前に見る）。壊れた行は飛ばす。最初に足した printer は既定になり、既定を消すと残りの最小の id が既定になる。
- **printd の起動**: `posix_spawn` で `KEILAND_LIBEXECDIR "/keiland-printd"` を起動する（compositor は複数の thread の process なので fork を避ける）。socketpair は `SOCK_STREAM | SOCK_CLOEXEC` で作り、子の端を `posix_spawn_file_actions_adddup2` で fd 3 に置く（子の端が偶然 3 なら先に別の番号に dup する。zedBSD の dup2 は同じ番号では CLOEXEC を外さない）。signal は `POSIX_SPAWN_SETSIGDEF` に compositor が handler を持つ SIGINT・SIGTERM・SIGHUP と SIGPIPE・SIGCHLD だけを入れ（zedBSD の libc は sigdefault の全ての signal に sigaction を呼び、SIGKILL・SIGSTOP で失敗する）、`POSIX_SPAWN_SETSIGMASK` で空の mask にする。他の fd の継承は printd が最初に `closefrom(4)` で断つ。posix_spawn の失敗は QEMU の確認に入れる。
- **printd の終わり**: socket の EOF で知る。`waitpid(pid, WNOHANG)` は ECHILD（compositor の `home.c` が毎 tick `waitpid(-1, WNOHANG)` で子を回収している）も「回収済み」として扱い、起動の失敗の数え方は exit の status でなく EOF の時刻で行う（10 秒に 3 回まで、その後 60 秒は起動せず、その間の依頼は FAILED daemon）。EOF の時、backend がその printd の spool の dir を消す（crash の後に RAM を残さない）。`home.c` の `waitpid(-1)` を pid に限るのは範囲外（Q1 に報告）。
- **backend と printd の socket**: nonblocking。送信は `MSG_DONTWAIT | MSG_NOSIGNAL`。送れなかった分は送信の queue に置き、各項目は「行の bytes、付ける fd、fd を送ったか」を持つ（sendmsg が途中で切れても fd は最初の 1 byte とともに渡っているので、残りの再送には付けない）。受信は `MSG_CMSG_CLOEXEC` で、`MSG_CTRUNC` が立てば socket を切る（printd を起動し直す）。printd から fd は来ない（来れば閉じて切る）。
- **job の状態遷移**（backend が持つ）:

  | 今 | 出来事 | 次 | 備考 |
  | --- | --- | --- | --- |
  | （無し） | print を受けた | QUEUED | job の ID を振り queued と result OK。fd を持つ |
  | QUEUED | printd へ JOB を送った | QUEUED（printd 待ち） | fd は ACCEPTED まで持つ |
  | QUEUED | ACCEPTED | QUEUED（spool） | fd を閉じる |
  | QUEUED | REJECTED detail | FAILED detail | fd を閉じる |
  | QUEUED | STATE sending | SENDING | |
  | SENDING | STATE waiting | WAITING | IPP だけ |
  | SENDING・WAITING | STATE done | DONE | LPD は送り終えて done |
  | 終わっていない | STATE failed detail | FAILED detail | |
  | 終わっていない | STATE cancelled | CANCELLED | |
  | QUEUED | printd の EOF（JOB を送る前か ACCEPTED 前） | QUEUED | 新しい printd に JOB を送り直す（1 job 2 回まで、超えたら FAILED daemon） |
  | QUEUED（spool）・SENDING・WAITING | printd の EOF | FAILED daemon | spool の dir は backend が消す |
  | QUEUED（JOB の行の 1 byte も socket に書いていない） | cancel | CANCELLED | 送信の queue から項目を取り除いてから fd を閉じる。printd は知らない |
  | JOB を送った後の終わっていない状態 | cancel | CANCELLING（外には今の状態のまま） | 常に CANCEL を送る（ACCEPTED の前でも）。CANCELLED は printd の STATE で確定 |
  | CANCELLING | ACCEPTED | CANCELLING | fd を閉じる |
  | CANCELLING | REJECTED | CANCELLED | fd を閉じる |
  | CANCELLING | printd の EOF | CANCELLED | |
  | DONE・FAILED・CANCELLED | cancel | 同じ | result INVALID |
  | DONE・FAILED・CANCELLED | STATE（遅れて来た） | 同じ | 捨てて log に残す（終わった状態は動かない） |
  | 任意 | その printer を remove | 終わっていない job に cancel と同じ処理 | remove は result OK |

  不変条件: DONE・FAILED・CANCELLED に入る時、backend は持っている fd を必ず閉じる。JOB の行の 1 byte でも socket に書いた job は「送った」として扱う（送信の queue の項目は送り終えるまで fd を持つ）。待ち・送信中は 16 個まで、終わった job は最後の 16 個を一覧に残す（古い物から消す）。
- OS ごとの tree（zedbsd・linux・freebsd）には何も足さない。

## 5. keiland-printd（`userland/desktop/printd/`、`/usr/libexec/keiland-printd`）

### 5.1 backend との約束（fd 3 の socketpair、1 行 1 命令、UTF-8、行は 1024 byte まで）

| 向き | 行 | 意味 |
| --- | --- | --- |
| backend → printd | `JOB <job> <ipp\|lpd> <host> <port> <path-or-queue> <title>` + SCM_RIGHTS で fd 1 つ | job の ID は backend の物。title は最後の項（空白を含みうる、空でもよい） |
| printd → backend | `ACCEPTED <job>` / `REJECTED <job> <detail>` | spool に複写できたか |
| printd → backend | `STATE <job> <sending\|waiting\|done\|failed\|cancelled> <detail>` | |
| printd → backend | `PATH <job> <path>` | IPP の path を探して見つけた時（backend が設定に覚える） |
| backend → printd | `CANCEL <job>` | printd は複写中なら一時 file を消し、送信中なら §5.4・§5.5 の取り消しをして `STATE <job> cancelled` を返す。終わった job には今の状態を返し、知らない job には `STATE <job> cancelled unknown` を返す |
| backend → printd | `NAME <seq> <host> <port>` | 足した時だけ。IPP の Get-Printer-Attributes で名前と path を問う |
| printd → backend | `NAMED <seq> <path> <text>` / `NAMED <seq>`（取れない） | |
| printd → backend | `SPOOL <dir>` | 起動の最初に、自分の spool の dir（backend が printd の EOF の後に消す） |
| printd → backend | `FATAL <語>` | 起動を断る時（runtime dir の権限など）、終わる前に。backend は crash と区別して起動を繰り返さない |
| printd → backend | `IDLE <n>` | 仕事が無くなって 60 秒。n は printd がこれまでに受けた命令の数 |
| backend → printd | `BYE <n>` | backend が送った命令の数が n と等しい時だけ。printd は n が自分の数と等しい時だけ終わり、違えば捨てる（IDLE と JOB の行き違いで終わらない） |

- **fd の対応**: SCM_RIGHTS の fd は byte の流れと別に届くので、printd は受けた fd の FIFO を持ち（compositor の `zwl_take_fd` と同じ形）、JOB の行 1 つが FIFO の先頭を 1 つ取る。検めで捨てた JOB の行も fd を取って閉じる。JOB かどうか分からない壊れた行（命令の語が読めない、1024 byte を超える）は protocol の異常として socket を閉じて終わる（backend は起動し直す）。受信は `MSG_CMSG_CLOEXEC`、`MSG_CTRUNC` なら同じく終わる。1 回の recvmsg に複数の fd が来てもよい。
- 両側とも、行の各項に C0・DEL・C1 の制御文字が無く、host・path・queue に空白が無いことを検め、外れた行は捨てて log に残す。printer から来た text（名前）は printd が §2 の規則で直してから NAMED に入れる。
- printd は起動の最初に `closefrom(4)`、SIGPIPE を `SIG_IGN` にし、fd 3 を自分で O_NONBLOCK にし、全ての送信に `MSG_NOSIGNAL` を付ける。ACCEPTED を socket に書き終えてから network への送信を始める（ACCEPTED が backend に届く前に crash して二重に送られない）。
- **fd 3 の EOF・HUP**: printd は全ての送信を中止し、spool を消して直ちに終わる（logout や compositor の終了で printd が孤児として印刷を続けない）。backend は自分から socket を閉じない。異常（`MSG_CTRUNC`、printd から fd が来た）の時は `shutdown(SHUT_WR)` だけにし、printd の EOF を 10 秒待ってから job を送り直す（古い printd と新しい printd が同じ job を送らない）。`kl_backend_print_close` も shutdown して EOF を待つ（kill は使わない。`home.c` の `waitpid(-1)` が先に回収して pid が使い回されうる）。
- 検めに外れた行のうち、job の番号が読める物には `REJECTED <job> protocol` を返す（job が止まったままにならない）。JOB の行が来たのに fd の FIFO が空なら protocol の異常として終わる。

### 5.2 spool

- `$XDG_RUNTIME_DIR/keiland-print/` の下に printd ごとの dir。printd は先に lock file `keiland-print/<name>.lock` を作って `flock` を持ち、それから `keiland-print/<name>/`（0700）を作る（`<name>` は pid と乱数、`O_CREAT|O_EXCL` で重なれば作り直す）。起動の時、lock の取れる（持ち主のいない）lock file の dir だけを消す（作ったばかりの他の printd の dir を消さない）。同じ利用者の 2 つの compositor の printd が同じ dir を使わない。`XDG_RUNTIME_DIR` が無い・利用者の物でない・077 が 0 でない時は起動を断る（終了の status で。backend は以後の依頼を FAILED daemon）。
- JOB を受けたら fd を fstat して S_ISREG・大きさ（0 と 256 MiB 超は REJECTED toobig）、合計 512 MiB と 16 job の上限（超えれば REJECTED busy）、`job-<job>.pdf` を `O_CREAT|O_EXCL|O_NOFOLLOW|O_WRONLY`、0600 で開き、`pread` で offset 0 から 64 KiB ずつ、min(fstat の大きさ, 256 MiB) byte で止めて複写する（複写の間に file が伸びても上限を超えない。合計の上限はこの値で予約する。複写の間も poll の loop に戻る）。先頭 1024 byte に `%PDF-` が無ければ REJECTED format。送る時の大きさは fstat の値でなく実際に複写した byte 数（複写の間に file が縮んでも合う）。終われば fd を閉じて ACCEPTED。spool の file は IPP の Print-Job の応答を受けた時、LPD を送り終えた時、job が終わった時に消し、合計は今ある file と複写中の予約で数える。
- zedBSD では spool は RAM（`/run` は kernel の tmpfs、`src/kern/vfs.c`）。Linux の `/run/user/UID` は大きさの上限が小さいことがあり（ENOSPC は REJECTED io）、FreeBSD と Linux の console の起動（`keiland-desktop.in` の `$HOME/.cache/keiland-runtime`）では disk。printd は `XDG_RUNTIME_DIR` を環境から読む（backend は spawn の環境に入れる）。sessiond は logout の時に `/run/user/UID` を消さないので、printd が終わる時（BYE）に自分の dir を消し、crash の時は backend が EOF で消す。未送の job は logout で失われる（段 1 の制限）。

### 5.3 printd の内部

- （2026-10-08 ws177-p022 の注記: 実装は job ごとの thread と blocking I/O・timeout のまま、main の thread が同時の数（printer ごと 1・全体 4）を数えて抑える。外から見える要件は満たし、非同期の状態機械への書き直しは見送った。Q1 了解。）
- 1 つの thread の poll の loop。connect は nonblocking、送信は分けて書き、送信の間も読みを poll して早く来た応答（401・413・426 など）を取る。
- 名前解決: host が IPv4 の literal ならそのまま。host 名なら名前解決ごとに補助の thread（同時に 4 つまで、結果は pipe で loop に返す）。getaddrinfo の結果を順に試し、zedBSD で AAAA の結果が EAFNOSUPPORT なら次を試す。名前解決は 10 秒で timeout（接続の 10 秒とは別）。
- 同時に送るのは printer ごとに 1 つ、全体で 4 つ（IPP の Get-Job-Attributes の見張りはこの数に含めない。見張りの間も同じ printer の次の job を送れる）。
- network から来る応答の上限: HTTP の状態の行・header の行は 1024 byte、header の合計 16 KiB、応答の本体は 64 KiB まで残し、それを超える分は読み捨てながら必要な属性だけを取る（全ての属性を返す printer でも印刷できる）。IPP の attribute は名前の数で 256 個（1setOf の追加の値は数えない）、各値 1024 byte（超えた値は読み捨て）。HTTP の 1xx は読み飛ばし、本体は Content-Length・chunked・接続の終わりのどれでも解く。
- timeout: 接続 10 秒、送受信は**進みの無い時間**（byte が増えない時間）が 60 秒で timeout（遅い printer で数 MB の本体に数分かかってもよい）。LPD の data file の後の最後の ack は 5 分待ち、来なければ DONE（unconfirmed）。

### 5.4 IPP（RFC 8010・8011、平文の HTTP/1.1）

| 項目 | 値 |
| --- | --- |
| operation | Print-Job 0x0002、Cancel-Job 0x0008、Get-Job-Attributes 0x0009、Get-Printer-Attributes 0x000B |
| version | Get-Printer-Attributes を 2.0 で送り、`server-error-version-not-supported`（0x0503）なら 1.1 で送り直す。決まった version を同じ printer の Print-Job 以降に使う（文書を 2 回送らない） |
| request-id | 1〜2^31−1 の printd の通し番号。応答の request-id が違えば protocol |
| delimiter tag | operation-attributes 0x01、job-attributes 0x02、end-of-attributes 0x03、printer-attributes 0x04、unsupported-attributes 0x05。他の 0x00〜0x0F は未知の群として中身を読み捨てる |
| value tag（送る） | integer 0x21、enum 0x23、uri 0x45、charset 0x47、naturalLanguage 0x48、mimeMediaType 0x49、keyword 0x44、nameWithoutLanguage 0x42 |
| value tag（読む） | 上に加え textWithoutLanguage 0x41、textWithLanguage 0x35・nameWithLanguage 0x36（言語の部分を除いて text として読む、`printer-info` などはこれで来ることがある）。boolean 0x22・dateTime 0x31・resolution 0x32・rangeOfInteger 0x33・collection 0x34/0x37/0x4A・no-value 0x13・拡張 0x7F・他の全ては長さで読み飛ばす |
| Print-Job の属性（順） | `attributes-charset`=utf-8、`attributes-natural-language`=en、`printer-uri`=`ipp://host:port/path`、`requesting-user-name`（login 名）、`job-name`（title、空なら "Document"）、`document-format`=application/pdf |
| Cancel-Job・Get-Job-Attributes | `attributes-charset`、`attributes-natural-language`、`printer-uri`、`job-id`（integer、Print-Job の応答の物）、`requesting-user-name`。Get-Job-Attributes は `requested-attributes`=`job-state`・`job-state-reasons` |
| Get-Printer-Attributes | 上の 3 つと `requested-attributes`=`printer-state`・`document-format-supported`・`printer-info`・`printer-make-and-model` |

- HTTP: `POST /path HTTP/1.1`、`Host: host:port`、`Content-Type: application/ipp`、`Content-Length`（IPP の message と複写した byte 数の合計、chunked は使わない）、`Connection: close`（1 要求 1 接続）。本体は IPP の message（attribute の群と end-of-attributes）に続けて文書。
- 送る前に Get-Printer-Attributes。`application/pdf` が無ければ FAILED format。`printer-state` が stopped（5）でも送る。path は設定の物を使い、HTTP 404 か `client-error-not-found`（0x0406）なら `/ipp/print` → `/ipp` → `/` を試し、通った path を `PATH` で返す。
- 応答の status: successful-ok 系（0x0000〜0x00FF）なら job-id を取り WAITING。`client-error-document-format-not-supported`（0x040A）は format、`client-error-not-authenticated`（0x0402）・`not-authorized`（0x0403）は auth、他の client-error は refused、`server-error-busy`（0x0507）は 30 秒後に 3 回まで送り直して busy、`server-error-not-accepting-jobs`（0x0506）は stopped、HTTP 401・403 は auth、426 は tls、他の HTTP の 4xx・5xx は refused。HTTP/1.0 の応答も Content-Length か接続の終わりで読む。
- その後 Get-Job-Attributes で `job-state` を 5 秒ごとに見る: pending（3）・processing（5）は WAITING、pending-held（4）・processing-stopped（6）も WAITING（detail は held・stopped）、canceled（7）は CANCELLED、aborted（8）は FAILED printer、completed（9）は DONE。30 分たっても終わらなければ DONE（unconfirmed）にして見るのをやめる。job-state を返さない printer は受け付けで DONE（unconfirmed）。
- 取り消し: 本体を送っている途中なら接続を切る（printer は job を受け付けない）→ CANCELLED。本体を送り終えた後は接続を切らずに応答を待ち（timeout 付き）、job-id を得てから Cancel-Job を送り、成功なら CANCELLED。job-id を得られない・Cancel-Job が失敗した時は CANCELLED でなく FAILED（detail unconfirmed: 印刷されたかもしれない）と報告する。
- ipps（TLS）は範囲の外（後の段。TLS の library の判断が要る）。

### 5.5 LPD（RFC 1179）

- TCP の port（既定 515）に接続。RFC は送り元の port を 721〜731 と定めるが、利用者の権限では使えないので普通の port で送る（それを拒む printer では refused、段 1 の制限）。
- `\x02<queue>\n` → 応答 0 → **data file を先に** `\x03<長さ> dfA<nnn><host>\n` → 0 → spool の file → `\0` → 0 → control file を `\x02<長さ> cfA<nnn><host>\n` → 0 → 本体 → `\0` → 0（BSD の lpr と同じ順。control file が先だと data が揃う前に処理を始める実装がある）。0 以外の応答は refused。
- `<nnn>` は 3 桁で、printd の起動の時刻から決めた種に job の ID を足した数の 1000 の剰余（compositor を起動し直しても前の job の名前と重なりにくい）。`<host>` は送り手の host 名を `[A-Za-z0-9.-]` で検めて 31 byte まで。
- control file の行: `H<host>`（31 byte まで）、`P<user>`（31 byte まで）、`J<title>`（99 byte まで）、`N<title>`（99 byte まで）、いずれも UTF-8 の文字の境界で切る、`ldfA<nnn><host>`、`UdfA<nnn><host>`。title は §2 の規則で検めた物なので改行を含まない。
- 取り消し: data file を先に送るので、control file を送り終える前に接続を切れば job は成立しない → CANCELLED。subcommand `\x01\n`（abort job）は、ある file の ack を受けた後、次の subcommand の前にだけ送る（file の途中では送らない）。control file を送った後は取り消せないので、cancel は FAILED（unconfirmed）と報告する。状態の問い合わせ 03・04 と remove 05 は使わない。

### 5.6 その他

- 待ち受けの socket を持たない（外向きの接続だけ）。setuid でない。
- detail の語（固定の一覧、Settings の文と試験の照合に使う）: `unreachable`・`refused`・`format`・`toobig`・`busy`・`auth`・`tls`・`timeout`・`protocol`・`printer`・`held`・`stopped`・`daemon`・`io`・`unconfirmed`・`cancelled`・`unknown`。
- log は syslog: job の ID・printer の id・protocol・結果の語。printer の address・文書の中身・title は残さない。

## 6. Settings の Printers の頁（p004）

- 今の stub（`se_soon_draw`）を、printer の一覧（名前・address・protocol・既定の印）と「Add Printer...」・選んだ printer の「Make Default」・「Remove」にする。Add の form は address・port（protocol で既定を入れる）・protocol（IPP / LPD の 2 択）と Add・Cancel。結果は語ごとの文。
- 一覧の下に job の一覧（状態と detail の文）と、終わっていない job の Cancel。
- 頁は `kl_system_printers_*` だけを使う（file を読まない）。

## 7. 試験（細かい修正ごとに回さない。WS の最後に T1）

| 層 | 試験 | 場所 |
| --- | --- | --- |
| 符号化 | host: printd の IPP の要求を独立の decoder（Python）で解いて属性と順・request-id・job-id を比べる、応答の解析（壊れた長さ・上限の超過・未知の tag・textWithLanguage・delimiter 0x05・1xx・接続の終わりで終わる本体の fuzz の corpus）、LPD の control file（長さの切り詰め、制御文字の拒否、data file が先） | `plan/ws145/tests/host-ipp.c`・`ipp-decode.py` |
| printd の通し | host: Python の模擬の IPP server（高い番号の port。Get-Printer-Attributes・Print-Job・Get-Job-Attributes・Cancel-Job。PDF を受けない・404・busy・1.1 だけ・426・chunked を拒む・processing-stopped が続く・改行を含む printer-info・全ての属性を返す・ゆっくり読む printer を選べる）と模擬の LPD server（abort、最後の ack を遅らせる）に、printd を socketpair で動かして送る。受けた文書が元と同じ（SHA-256） | `plan/ws145/tests/mock-ipp.py`・`mock-lpd.py`・`run-host-printd.sh` |
| printd の寿命 | host: printd の kill（ACCEPTED の前・後）、IDLE と JOB の行き違い、BYE の後の依頼、2 つの printd が同じ runtime dir（別の dir、古い dir だけを消す）、合計と数の上限、FIFO・socket の fd、offset を進めた fd、XDG_RUNTIME_DIR の権限、行の分割・1 回に 2 つの fd・不正な JOB の行の後の正しい JOB（別の文書が印刷されない）、ACCEPTED 前の cancel で印刷されない、printer に届いた後の cancel が CANCELLED と報告されない（本体の後に応答を遅らせる模擬）、IDLE と JOB の行き違いで終わらない（IDLE n・BYE n）、backend の shutdown の後に二重に印刷しない、fd 3 の EOF で printd が spool を消して終わる、複写の間に伸びる file、2 つの printd の起動が重なっても互いの dir を消さない、printer が途中で切った時に SIGPIPE で死なない | 同上 |
| backend | host: printers.conf の読み書き（壊れた行、16 個、既定の削除、2 つの session の同時の足し（両方が残り id が重ならない）、id を使い直さない）、状態遷移の表の各行、fd の数が依頼の前後で変わらない（漏れ）、printd が死んだ後の送信で SIGPIPE が起きない、送信の queue の部分送信で fd が二重にならない、waitpid の ECHILD | `run-host-print-backend.sh` |
| protocol | host: capability と CHANGED の bit が他と重ならない、libkeiland の `system_manager_requests[]` の添字と `KL_SYSTEM_MANAGER_GET_*` が一致する、却下の経路で `zwl_take_fd` と close、fd が後から届く（EAGAIN でやり直す）、2 つの client が同じ request の番号で依頼しても result を取り違えない、続けて 2 つ印刷して job_of が両方を返す、version 9 の compositor と新しい libkeiland（ENOTSUP）（`plan/ws131/tests/host-system.sh` の形） | `plan/ws145/tests/host-system-printers.c` |
| QEMU | T1: guest の Settings で printer（host の模擬の server、user-net の 10.0.2.2 の高い番号の port）を足し、`printtest` で PDF を印刷、模擬の server が受けた文書の SHA-256 と job の DONE、zedBSD の posix_spawn で printd が起動すること、Printers の頁の PNG | `plan/ws145/tests/print-guest.sh` |
| Linux・FreeBSD | T1・T2: Debian の QEMU+KVM の guest と WS109 の FreeBSD 15 の guest で Keiland の printd と printtest（同じ模擬の server） | p005 |
| 実機の printer | UAT（D4 の機種で） | — |

## 8. Phase の改訂（ws.md の表へ）

| Phase | 内容 | 依存 |
| --- | --- | --- |
| p002 | keiland-printd（約束の行・spool・IPP・LPD・寿命）と host の符号化・通し・寿命の試験 | p001 |
| p003 | backend の print（設定の file・printd の起動と通信・状態遷移）、compositor の printers の object、protocol version 10、libkeiland の口（KL_VERSION 34）、`printtest`、host の backend と protocol の試験 | p002 |
| p004 | Settings の Printers の頁 | p003 |
| p005 | Linux・FreeBSD の build と install（Makefile.linux・freebsd、`keiland-linux.mk`・`keiland-freebsd.mk`、`KEILAND_LIBEXECDIR` が `/opt/keiland` の時）と Debian の QEMU+KVM・FreeBSD 15 の guest の確認 | p003 |
| p006 | 全文の規約の確認と回帰、T1 の QEMU の試験（最後） | p002〜p005 |
| （別の WS の案） | 変換の filter（PDF → PWG raster・PostScript・PCL）。単一目標の外なので新しい WS（D4 の機種が PDF を受けなければ、受け入れの前に必要） | p002 |
| （p007 案） | PDF Viewer の File > Print（D6 の判断次第） | p003 |

## 9. 危険と制限

- 段 1 は平文の IPP と LPD だけ。printer が ipps だけを受けると使えない。
- LPD の特権の port を使わないので、それを要する古い printer では失敗する。
- spool は login の間だけ。
- PDF を直接受けない printer は filter の WS まで使えない。
- IPP の path が `/ipp/print`・`/ipp`・`/` 以外の printer は、D3 の「詳しい設定」が無ければ使えない。
- zedBSD では IPv6 の printer に送れない（kernel に IPv6 が無い）。
