<!-- awesome-plan project=zedbsd record=ws335-p015 -->

# ws335-p015: OS の層（POSIX と Windows）

Status: 実装済み・試験 PASS（2026-11-02 P6。cleared は Q1。依存: p002）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-os.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-os-posix.c`、`userland/base/libcrypto-compat/cryc-os-win32.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-os.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- POSIX: getrandom（include/libc/sys/random.h）、pthread、read/write、time。zedBSD の libc にある関数だけを使う（check-skeleton の zedBSD の compile が確かめる）。
- Windows: BCryptGenRandom、SRWLOCK、InitOnceExecuteOnce、FlsAlloc（FlsFree は呼ばない。header のとおり、static library は .exe に入り process の終わりまで map されている）、recv/send/WSAGetLastError、CertOpenStore(CERT_STORE_PROV_SYSTEM_W, …)・CertEnumCertificatesInStore（header の 4 つの組と順: current user と local machine の "Disallowed" を add_distrusted へ、次に "ROOT" を add_der へ。ROOT は Disallowed と照らさない。CertGetEnhancedKeyUsage の property が serverAuth を持たない物と X509_ASN_ENCODING でない物は飛ばす。CertOpenSystemStoreW は current user しか開けないので使わない）、GetModuleFileNameW(NULL, …)（.exe の directory。buffer を 32768 の UTF-16 の単位まで伸ばす）、WideCharToMultiByte（`WC_ERR_INVALID_CHARS`）・MultiByteToWideChar（`MB_ERR_INVALID_CHARS`）・_wfopen(…, L"rb")。winsock2.h を windows.h の前に。W の API を名指しで呼ぶ（Windows の build は `-DUNICODE` を付ける。A と W の macro に頼らない）。Windows の挙動は MSDN の文書（Microsoft Learn）を参照する。
- Windows の試験は `run-unit.sh --win32 os`（Wine の上で動かす）: 乱数（長さ 0・1・256・257・大きい物）、時刻、mutex を 2 thread で、thread の値と thread の終わりの destroy（CreateThread か `_beginthreadex` の thread が終わった後に destroy が 1 回呼ばれた事）、UTF-8 の名前（日本語を含む）の file の読み・EFBIG・ENOENT、loopback の TCP の組（127.0.0.1 に listen・connect、非 blocking で EAGAIN）、cryc_os_system_roots: Wine の prefix の current user の "Disallowed" に試験の証明書を `CertAddEncodedCertificateToStore` で足すと add_distrusted に渡る事、"ROOT" に足した証明書が add_der に渡る事、その証明書に serverAuth を持たない拡張鍵用途の property（`CertSetEnhancedKeyUsage`）を付けると渡らない事（Wine の ROOT には host の CA が 100 以上あるので、件数でなく試験の証明書の有無で判定する）。足した証明書は試験の終わりに `CertDeleteCertificateFromStore` で消し、始めにも前の run の残りを消す（prefix は run の間で残る）。試験の .exe の隣に置いた `cert.pem` の path が add_file に渡る事（`GetModuleFileNameW` で試験の側が自分の directory を求めて比べる）。試験の側の Windows だけの部分は `#ifdef _WIN32` でよい（library の .c ではない）。
- posix の試験（`run-unit.sh os`）: socketpair の読み書き、EAGAIN、EOF、cryc_os_system_roots が add_file を `/etc/ssl/cert.pem` で 1 回だけ呼び、add_distrusted・add_der を呼ばない事。
- EINTR の retry、部分の read/write。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh os     # この module
sh plan/ws335/tests/run-unit.sh --win32 os     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

- Windows の 2 arch の static link は check-skeleton の 6 が確かめる（x86_64 は `run-unit.sh --win32 os` でも .exe に link して走らせる）。DLL は作らない（[design.md](../design.md) D23）。

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

2026-11-02（host の `date` は 02:59 JST）P6（Sonnet 5.5）、Queue q1897-i01。worktree p6-ws335b（main 9d9929b05）。実装と試験の commit は 211f15c3a8。

作った物: `userland/base/libcrypto-compat/cryc-os-posix.c`（623 行）、`userland/base/libcrypto-compat/cryc-os-win32.c`（1053 行）、`plan/ws335/tests/unit/test-os.c`。header・試験の道具・他の module・Makefile は変えていない。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`、Wine は `LC_ALL=C.UTF-8`、下の「自分で決めた点」の 12）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh os`（ASan・UBSan。5 回続けて） | `test-os: PASS (1450 checks)` / `run-unit: PASS` |
| `LC_ALL=C.UTF-8 sh plan/ws335/tests/run-unit.sh --win32 os`（x86_64、Wine 10.0。3 回続けて。prefix は run の間で残る） | `test-os: PASS (1455 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh all`（test-os は 1434 checks の版。実装の file はその後変えていない） | 全部 PASS、最後の行 `run-unit: PASS`（10 分 32 秒） |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh`（最後の版の試験の後） | `check-skeleton: PASS`（warning 0、cc・clang・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲） |
| `git diff --check` | 出力なし |

run-ct は流していない（この module は秘密を扱わない: 乱数の出力は呼び出し側が秘密にする物で、この module の分岐は秘密に依らない）。

実装:

- POSIX: 乱数は getrandom を 256 byte ずつ（EINTR は再試行、0 byte の返りは EIO、失敗は out 全体を wipe）。時刻は time(NULL)（読めない時は 0）。mutex は malloc した pthread_mutex_t。atomic は `__atomic_add_fetch`（SEQ_CST）。thread の値は pthread_key（pthread_once で作り、削除しない）。file は fopen "rb" と fread を buffer の倍々で（max+1 byte まで読み、後ろに 0 を 1 byte。最後に realloc で size+1 に縮める）。socket は read・send(MSG_NOSIGNAL)（MSG_NOSIGNAL が無い system と ENOTSOCK は write）。system_roots は add_file(`/etc/ssl/cert.pem`) を 1 回。
- Windows: BCryptGenRandom（BCRYPT_USE_SYSTEM_PREFERRED_RNG）、`_time`/time、SRWLOCK（malloc）、InterlockedExchangeAdd、FlsAlloc（InitOnceExecuteOnce、FlsFree しない）、`MultiByteToWideChar(MB_ERR_INVALID_CHARS)` と `_wfopen(L"rb")`、recv/send（WSAEINTR は再試行、WSAGetLastError を errno に写す表）、CertOpenStore(CERT_STORE_PROV_SYSTEM_W) の 4 つの store（header の順）、拡張鍵用途の property の判定、GetModuleFileNameW（512 から 32768 まで倍々）と WideCharToMultiByte(WC_ERR_INVALID_CHARS) で `<exe の directory>\cert.pem`。

試験（posix 1450 checks、win32 1455 checks）:

- 乱数: 長さ 0・1・2・15〜17・31〜33・63〜65・127〜129・254〜258・300・511〜513・1000〜1025・4096・4097・100000 の各々で、4 通りの fill（0x00・0xff・0x5a・0xa5）の buffer のどれにも同じ位置が全部 fill のままの byte が無い事（書かれた byte が 4 つとも一致する確率 2^-32）、前後 32 byte の guard が無傷、AddressSanitizer 用の exact-size の buffer。size 0 と NULL、NULL の buffer と size 1 の EINVAL、cryc_random_fn 型での呼び出し、2 回の出力が違う事、1 MiB+3 byte の byte の度数（各値 3500〜4700、平均 4096・標準偏差 64）と bit の数。
- 時刻: time(NULL) と 3 秒以内、2023-11 以降 2100 年より前、1.1 秒待って 1〜4 秒進む。
- mutex: NULL・0 の構造体、init・lock・unlock・destroy・二重 destroy・再 init、4 thread × 50000 回の臨界区間（同時に 2 thread が中に居ない事の flag と、非 atomic の counter の和）。全 thread が走り出すまで待つ gate を置いた（置かないと thread が重ならない）。
- atomic: 1 thread の ±1・LONG の端、4 thread × 20000 回の +1 が返す値が 1..80000 の各々ちょうど 1 回、続けて -1 が返す値が 79999..0 の各々ちょうど 1 回。
- thread の値: 最初は NULL、thread が 2 つの値を続けて入れると destroy が最後の値で 1 回だけ呼ばれる、main には見えない、値を入れない thread・NULL を入れた thread では destroy が呼ばれない、main の値は別、64 thread を順に 64 回、8 thread を同時（全員が入れ終わるまで待ってから自分の値を読む）に 8 回。destroy が thread の終わりに（join の前に）呼ばれる事は Wine でも確かめた。
- file: 0〜1048577 byte の 25 通りの size（buffer の倍々の境の前後）で max が size・size+1・CRYC_OS_FILE_MAX・size-1（EFBIG で出力が NULL と 0）、全 byte 値の pattern の一致と後ろの 0、CR・LF・0x1A・0 byte が変わらない（binary）、ENOENT（出力を消す）、file を directory として開く path の ENOENT、directory を file として読む EIO、NULL と max 超えの EINVAL、日本語と絵文字（surrogate pair）の名前、CRYC_OS_FILE_MAX ちょうど（疎な file）は読め +1 は EFBIG。Windows では不正な UTF-8（単独の 0xFF・過長 C0 80・surrogate ED A0 80・途中で切れた E3 83）が EINVAL。
- socket（POSIX は socketpair、Windows は 127.0.0.1 の TCP の組）: 短い読み書き、room が多い・少ない読み、両方向、os_error が NULL、size 0（write は 0 で done 0、read は EINVAL）、NULL の引数、非 blocking の EAGAIN と os_error、EOF（0 byte で成功、繰り返し）、room が尽きるまで書いた時の EAGAIN（done 0・os_error）とその全 byte の読み戻し、1 MiB+17 byte の一括の転送（POSIX は部分書き込みを確認）、相手が閉じた後の書き込みが EIO で os_error≠0 かつ errno が EPIPE・ECONNRESET・ECONNABORTED のどれか、開いていない descriptor の EIO（POSIX は EBADF、Windows は os_error WSAENOTSOCK と errno ENOTSOCK）。POSIX は更に: pipe（send が ENOTSOCK の時の write への切り替え）、int の範囲を越える descriptor と、int に wrap すると開いている pipe の端になる値（上にも下にも）が EIO・EBADF で、pipe の中身が減らない事、blocking の read と write が SIGALRM（SA_RESTART 無し）で EINTR になっても再試行して終わる事（3 回目の signal の handler が相手へ 1 byte 書く・相手から読み出す）。
- system_roots: NULL の callback は EINVAL で何も呼ばない。POSIX は add_file が `/etc/ssl/cert.pem` で 1 回、add_distrusted・add_der は 0 回、ENOENT は 0、EIO・ENOMEM はそのまま。Windows は試験の側で作った 6 通りの証明書（DER を derwriter.h で作った、署名はしていない）を current user の Disallowed と ROOT に `CertAddEncodedCertificateToStore` で足し、Disallowed の 1 つが add_distrusted と add_der の両方へ、plain な ROOT・serverAuth と clientAuth の property・拡張鍵用途の拡張（property でなく証明書の中）だけで clientAuth の物が add_der へ渡り、clientAuth だけの property と空の property の物は渡らない事、callback の順（distrusted → der → file）、add_file の path が試験の側で GetModuleFileNameW から作った `<exe の directory>\cert.pem` と同じで、その file を cryc_os_read_file で読める事、distrusted・der の callback の誤りでそこで止まり後ろを呼ばない事、add_file の ENOENT は 0・EIO はそのまま、試験の終わりに証明書を消した後は 1 つも渡らない事。始めにも前の run の残りを消す。

わざと壊す確かめ（git の外。`build/tmp/p015/brk.py`（POSIX、-O1・ASan・UBSan、1 通りごとに time の上限 60 秒）と `build/tmp/p015/brkwin.py`（Windows、llvm-mingw・Wine、上限 120 秒）。実装の file は触らず複写を書き換え、最後に sha256 が変わっていない事を script が確認した。BUILD-ERROR・TEST-FAIL・SANITIZER・TIMEOUT・MISSED を区別。build の誤りで落ちた物は結果に数えていない）:

- POSIX 49 通りのうち、build の誤り 1（pattern が 2 か所に合った。数えない）、試験の check の FAIL 37、ASan・UBSan・LeakSanitizer の中断 4（NULL の buffer を拒まない・lock の NULL の確認を外す・init の NULL を拒まない・読み取りの失敗で buffer を解放しない）、hang で time の上限 2（unlock が何もしない・buffer を +1 ずつ伸ばす）、見逃し 5。
- Windows 32 通りのうち、build の誤り 1（pattern が 2 か所に合った。数えない）、check の FAIL 26、hang で time の上限 2（unlock が何もしない・size 0 の recv が blocking で止まる）、見逃し 3。最初の run では atomic の非 atomic・不正 UTF-8 の許可・EAGAIN の変換が落ちなかった（thread が重ならない・変異が 1 か所だけだった・変異の書き間違い）ので、gate を足し変異を直して全部 FAIL にした。
- 見逃しの理由（全部）:
  - POSIX: (1) getrandom の 256 byte の上限を外す: Linux の getrandom は大きな長さを受けるので観測できない（getentropy の上限は zedBSD の getrandom の実装による。移植のための上限で等価）。(2) mutex の init 失敗の値を ENOMEM から EIO にする: pthread_mutex_init を失敗させる手段が無い。(3) `os_key_ready` を set の後に重ねて 1 にする: 等価。(4) descriptor の `< 0` の検査を外す: `(uintmax_t)` への cast で負の値は大きな値になり、`> 0x7fffffff` の検査が拒むので等価。(5) write の size 0 でも send を呼ぶ: send の 0 byte は 0 を返すので等価。
  - Windows: (1) write の `*done = size`: Windows の非 blocking の send は大きな buffer を全部受けるか WSAEWOULDBLOCK を返すかで、部分書き込みが起きない（loopback の TCP と Wine で確認）ので観測できない。(2) 証明書の encoding の判定を `== 0x10001` にする: 試験の証明書は X509_ASN_ENCODING（1）だけで、0x10001 の物を store に足す手段が無かった。(3) store を開く flag の変異: 効かない値で、等価。

自分で決めた点（header が黙っていた所。Q1 が確認する）:

1. `cryc_os_random`: NULL の out で size > 0 は EINVAL（header は 0 か EIO だけを約束。cryc_random_fn の呼び手の誤りなので落ちるより明示する）。0 byte が返って進まない getrandom は EIO（無限 loop を避ける）。
2. `cryc_os_read_file`: max が CRYC_OS_FILE_MAX を越えたら EINVAL（header は「at most」）。data・size が NULL は EINVAL。ENOENT は POSIX では ENOENT と ENOTDIR、Windows では ENOENT だけ（UCRT は途中の directory が無い時も ENOENT）。directory を開いた時は EIO。
3. `cryc_os_socket_read`: size 0 は EINVAL（0 を返すと呼び出し側が end of stream と読む）。`cryc_os_socket_write`: size 0 は 0 で done 0（system を呼ばない）。done が NULL は EINVAL、os_error が NULL は許す。EAGAIN の時も os_error に system の code を入れる（header は EIO の時だけ書く）。成功の時は 0。
4. POSIX で int の範囲を越えた・負の descriptor は EIO・os_error EBADF（system を呼ばない。wrap して別の fd を読み書きしない）。Windows は SOCKET に cast するだけ（不正な値は WSAENOTSOCK）。1 回の呼び出しは 0x40000000 byte まで。
5. `cryc_os_system_roots`: 3 つの callback のどれかが NULL なら両方の system で EINVAL（POSIX は add_file しか使わないが、移植した呼び出し側が同じに動くように）。
6. Windows の encoding の判定は `(dwCertEncodingType & X509_ASN_ENCODING) == 0` を飛ばす（header は「X509_ASN_ENCODING でない物」。X509_ASN_ENCODING|PKCS_7_ASN_ENCODING の証明書を飛ばさない側）。拡張鍵用途の property の読みで CRYPT_E_NOT_FOUND は「property 無し」（入れる）、それ以外の失敗は制限と見て入れない（安全側）。callback の誤りの時は証明書の context と store を閉じてから返す。
7. Windows の thread の値の destroy: FLS の callback が `os_key_destroy` を呼ぶ。destroy が NULL なら何もしない。Windows の key は InitOnceExecuteOnce の Parameter で最初の destroy を渡す。POSIX は pthread_once が引数を取らないので、最初の呼び出しが static に置く（全部の呼び出しが同じ関数を渡す約束。`__atomic` で読み書き）。
8. POSIX の atomic は `__atomic_add_fetch`（clang・gcc の組み込み。zedBSD の clang の x86_64・aarch64 で syntax を確かめた）、Windows は InterlockedExchangeAdd で、wrap は unsigned で計算（符号付きの overflow を避ける）。
9. mutex: init の pthread_mutex_init の失敗は ENOMEM。init・lock・unlock・destroy は NULL の構造体と NULL の impl を許す（何もしない）。
10. file の buffer: 最初 16384 byte、倍々で max+2 byte まで、最後に realloc で size+1 に縮める（呼び出し側の過読みを AddressSanitizer が拾える）。
11. 試験の file の置き場所: 試験の program の directory（run-unit.sh が run ごとに新しく作る）。file の削除は走らせない。Windows の試験は GetModuleFileNameW、POSIX は argv[0]。疎な 16 MiB の file が 2 つ残る（Windows は実際に 0 を書く）。
12. **Wine の locale**: `LANG=en_US.UTF-8`（この host では未生成）のままだと Wine は UTF-8 の file 名（日本語・絵文字）を作れず、試験の 6 check が FAIL する（試験が「LC_ALL=C.UTF-8 で流す」と表示する）。`LC_ALL=C.UTF-8 sh plan/ws335/tests/run-unit.sh --win32 os` で PASS する。run-unit.sh は変えていない（Q1 が決める: 試験の道具の中で wine に `LC_ALL=C.UTF-8` を渡すのが筋）。
13. **Wine の証明書の store**: システムの論理 store（`CERT_STORE_PROV_SYSTEM_W`）で見つけた証明書を `CertDeleteCertificateFromStore` すると、成功を返すが registry から消えない（Wine 10.0、`CertEnumCertificatesInStore` の loop が終わらない事を確認）。試験の後始末は同じ key の registry store（`CERT_STORE_PROV_REG`）を開いて消す。これは Wine の挙動への対処で、実際の Windows の挙動は未確認。

phase.md と仕様の食い違い・header の不備: 見つからなかった。header の comment の持ち越し（規約の見直しの Phase へ）: (a) `cryc_os_random` の NULL の out の EINVAL、(b) `cryc_os_socket_read` の size 0 の EINVAL と EAGAIN の時の os_error、(c) `cryc_os_system_roots` の NULL の callback の EINVAL。header は変えていない。

未実施: 実機・QEMU（範囲外）、zedBSD での実行（getrandom・pthread・send の挙動。syntax は zedBSD の clang で確認）、aarch64 Windows の実行（check-skeleton で build と link のみ）、実際の Windows での実行（Wine のみ）、`cryc_os_random` の getrandom が失敗する経路（EIO と wipe。失敗させる手段が無い）、`cryc_os_mutex_init` の ENOMEM、`FlsAlloc` の失敗、HKLM の store（Wine の prefix に足していない）、Windows の `GetModuleFileNameW` が 512 単位を越える長い path。run-ct。
