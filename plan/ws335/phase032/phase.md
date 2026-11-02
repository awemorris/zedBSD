<!-- awesome-plan project=zedbsd record=ws335-p032 -->

# ws335-p032: 公開の SSL・SSL_CTX

Status: 実装済み・試験 PASS（2026-11-02 15:30 Sonnet q1953-i01。cleared は Q1。依存: p031、p022）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libssl-compat/sslc-api.h`、`include/libc/compat/ssl/ssl.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - OpenSSL 3.5 の API の文書（互換の振る舞いの正）: /home/awe/zedBSD-claude1/build/distfiles/openssl-3.5.8.tar.gz（SHA-256 a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2、展開は自分の build/tmp の新しい dir に。`doc/man3`・`doc/man7` の .pod は振る舞いの仕様として読み、文・code を写さない）: doc/man3/SSL_CTX_new.pod、SSL_CTX_set_verify.pod、SSL_CTX_load_verify_locations.pod、SSL_CTX_set_options.pod、SSL_CTX_set_min_proto_version.pod、SSL_CTX_set_cert_store.pod、SSL_new.pod、SSL_set_fd.pod、SSL_set1_host.pod、SSL_connect.pod、SSL_read.pod、SSL_write.pod、SSL_get_error.pod、SSL_pending.pod、SSL_shutdown.pod、SSL_get_verify_result.pod、SSL_CTX_set_tlsext_servername_callback.pod（SSL_set_tlsext_host_name）。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libssl-compat/api-ssl.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-api-ssl.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- SSL_get_error の表と errno の保存。SSL_write の再試行の約束。
- reason の表（own wording）を cryc_err_register_ssl で登録。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- p037 の `plan/ws335/tests/unit/tlspeer.h`。全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh api-ssl     # この module
sh plan/ws335/tests/run-unit.sh --win32 api-ssl     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

実装済み・試験 PASS（2026-11-02 15:30 Sonnet q1953-i01。cleared は Q1）。commit: 8a2938c572（最初）、c269b8b316（わざと壊す確かめの反映）。

### 実行した command と結果

- `WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors sh plan/ws335/tests/run-unit.sh api-ssl`: `test-api-ssl: PASS (5129 checks)`、`run-unit: PASS`。
- `sh plan/ws335/tests/run-unit.sh --win32 api-ssl`: `test-api-ssl: PASS (5570 checks) (win32)`、`run-unit: PASS`（loopback の TCP と Wine。Windows の loopback は届くのが遅れるので、alert を待つ所は `rig_settle_alerts` で数える）。
- `sh plan/ws335/tests/run-unit.sh all`（vectors 付き、最後の変更の後）: 58 行 PASS、FAIL 0、`run-unit: PASS`。
- `BASE=4eae22020 sh plan/ws335/tests/check-skeleton.sh`: `check-skeleton: PASS`（cc・clang・zedBSD の clang・llvm-mingw 2 arch、名前の範囲、BASE からの header・道具の変更 0）。
- `git diff --check`: 空。

### 試験の内容（test-api-ssl.c）

reason の表（57 件の番号を OpenSSL 3.5.8 sslerr.h と RFC 8446 B.2 の数で、text の重複なし、`ERR_error_string_n` で全件が text 付き）、context（method・options・ctrl・store・参照・NULL の引数）、PEM file（良い・悪い block・無い・空・path 引数）、SSL（socket・SNI 名・SSL_set1_host の名前と address・SSL 自身の version の bound・未開始の SSL）、
handshake 8 通り（TLS 1.3 の 3 suite・TLS 1.2 の 3 suite、x25519 と P-256（HRR）、P-256 と P-384 の証明書）、検証の失敗 6 種（issuer 不明・名前・address・期限切れ・まだ有効でない・VERIFY_NONE）と alert の値、verify callback、version の範囲と option、write（13 の長さ、record の数）、socket が詰まる write（再試行の約束、BAD_WRITE_RETRY）、read（部分読みと SSL_pending、NewSessionTicket・KeyUpdate をまたぐ）、
read・write が最初の handshake を走らせる場合、close（close_notify 双方、EOF、IGNORE_UNEXPECTED_EOF、致命の alert、socket の失敗 SYSCALL と errno、handshake 中の EOF・close_notify・致命の alert・shutdown、context を先に解放）、4 thread で 1 つの context。期待の値は仕様（RFC の alert 番号・record の上限・ClientHello の拡張・RFC 6066 の SNI）と OpenSSL 3.5.8 の数値だけから。他の実装の出力は使っていない。

### わざと壊す確かめ

実装を git の外で 113 通りに壊した（比較の向き・境界・条件の反転・代入の削除・定数・table の番号と text・error の種類・queue への登録・参照の数・lock 周り。道具は build/tmp/p032-mut/mutate.py、ASan・UBSan 付きで test-api-ssl を流す）。結果: 106 通りが試験の失敗か sanitizer で検出、7 通りが生き残り（最初に compile できなかった 3 通り（未使用の引数）は書き直して検出に数えた）。生き残りの理由:

- `write-after-close needs CONNECTED dropped`（SSL_OPERATION_WRITE の EPIPE で CONNECTED の確かめを外す）: CLOSE_SENT は接続の後にしか立たないので等価。
- `ipv4 digits > 4`、`ipv4 fields >= 4`: 後ろの確かめ（値 255 の上限、3 つの dot）が先に同じ入力を拒むので等価な二重の守り。
- `set1_host empty not handled`: 空の text は X509_VERIFY_PARAM_set1_host が同じく消して 1 を返すので等価。
- `pending clamp`（0x7fffffff の切り詰め）: 2 GiB の buffer が要る。未試験。
- `enter does not reset errno`（`last_errno` を各 call の初めに 0 に戻す）: `last_errno` は同じ call で socket が失敗した時にしか読まれないので観測できず等価。
- `ENOMEM branch removed`: malloc の失敗の注入が要る（一般の経路は INTERNAL_ERROR になる）。未試験。

最初の確かめで見つかった見逃し（verify_mode の mask、`SSL_set_fd(ssl, 0)`、address を NULL・空・不正な text で消すこと、成功した call の後の `SSL_get_error(ssl, 0)`）は試験を足して塞いだ。ECONNRESET の枝と、call の後の verify_result の代入は、既定の枝・`SSL_get_verify_result` が同じ結果を出す二重だったので実装から外した。

### 自分で決めた点（header が黙っている所）

1. **設定を読む時**: SSL_CTX の設定（options・verify mode・version の bound）は、SSL_new でなく最初の handshake の call（SSL_connect・SSL_read・SSL_write）で lock の下に読む。SSL_new は参照を持つだけ。SSL 自身の version の bound（SSL_ctrl）は handshake の前は `conn.config.min_version`・`max_version` に置き、0 は context の値を使う。sslc-api.h の「SSL_new copies its settings」とは違う（`ssl_st` に写しを置く場所が無い）。context の設定を SSL_new と最初の call の間に変えると効く。Q1 の判断を求める。
2. **verify callback**: ctx の callback を handshake の config にいつも adapter として入れる。adapter は X509_V_ の code を `ssl->verify_result` に残し、`store_ctx.error` と `depth`（`hs.verify.depth`）を渡し、ctx の callback（その時に lock の下に読む。NULL なら chain の判定のまま）を lock の外で呼ぶ。SSL_VERIFY_NONE では handshake が callback を呼ばないので呼ばれない（検証の結果は記録される）。`SSL_get_verify_result` は handshake が生きていれば hs の verdict、failed なら adapter が残した値。VERIFY_NONE の handshake が証明書の後の別の理由で失敗すると、hs が消えるので結果は X509_V_OK のまま（OpenSSL は残す）。
3. **SSL_get_error の分類**（conn の戻りから）: EAGAIN は wants の WRITE bit で WANT_WRITE、他は WANT_READ。EPIPE は、この side の close_notify の後の write なら SSL_ERROR_SSL の PROTOCOL_IS_SHUTDOWN、CLOSE_RECEIVED（close_notify、IGNORE_UNEXPECTED_EOF の EOF、handshake 中の close_notify も）なら ZERO_RETURN で戻り値 0、そうでなければ（handshake 中の EOF）SSL_ERROR_SSL と conn.reason。ECONNRESET（EOF の close_notify 無し）と他の失敗は SSL_ERROR_SSL と conn.reason（0 なら ERR_R_INTERNAL_ERROR）。EIO は transport の wrapper が socket の失敗の errno を `last_errno` に残した時だけ SSL_ERROR_SYSCALL（queue に ERR_LIB_SYS と errno。SSL_get_error が errno を戻す）、残さない EIO（乱数の失敗）は SSL_ERROR_SSL。ENOMEM は ERR_R_MALLOC_FAILURE。既に failed の connection への次の call は 1 回ずつ SSL_R_PROTOCOL_IS_SHUTDOWN。
4. **戻り値**: ZERO_RETURN の時は 0、それ以外の失敗と待ちは -1（SSL_connect も）。SSL_get_error は result > 0 なら SSL_ERROR_NONE、他は call が置いた分類（各 call の初めに NONE に戻す。SSL_shutdown の 0 は NONE）。
5. **SSL_shutdown**: started でない・handshake の途中は SHUTDOWN_WHILE_IN_INIT。致命の alert の後（handshake の途中で failed になった場合も）は PROTOCOL_IS_SHUTDOWN（failed の確かめを in-init より先）。2 回目の call は close_notify を送り直さない。
6. **SNI と host**: SSL_set_tlsext_host_name は、IP address の text（4 つの 10 進の欄、または ':' を含む）なら 1 を返して何も送らない（RFC 6066 3、D10）。空の text・不正な名前は 0（名前は変えない）。NULL は消して 1。handshake の後の呼びは 0（UNSUPPORTED）。SSL_set1_host は先に host と address を両方消し、address の form なら X509_VERIFY_PARAM_set1_ip_asc、他は set1_host。address の判定は、':' を含む、または 4 つの 10 進の欄（0〜255、先頭の 0 無し）の text。NULL と空の text は消すだけで 1。
7. **file**: SSL_CTX_load_verify_locations は path が NULL でなければ 0（UNSUPPORTED）、file が NULL なら 0（PASSED_NULL）。store の失敗は X509 library の 1 つの error（ENOMEM は MALLOC_FAILURE、ENOBUFS は UNSUPPORTED、他は PASSED_INVALID_ARGUMENT）。
8. **version**: SSL_CTX_ctrl・SSL_ctrl の SET は 0・TLS1_2・TLS1_3 だけ（他は 0 と PASSED_INVALID_ARGUMENT）、min が max より大きい設定は SET では拒まず、handshake の開始で NO_PROTOCOLS_AVAILABLE（SSL_OP_NO_TLSv1_2・NO_TLSv1_3 も同じ範囲で考える。何も送らない）。handshake の後の SET は 0（UNSUPPORTED）。SSL_CTX_new の options は空から。
9. **reason の表**: SSL_R_ 29 件と、RFC 8446 B.2・RFC 5246 7.2.2 の alert 28 件（1000 + description）の own wording の text。SSL_CTX_new の最初の call で `cryc_err_register_ssl`。p031 の alert → reason の表（decode_error→BAD_LENGTH、decrypt_error→BAD_SIGNATURE、protocol_version→UNSUPPORTED_PROTOCOL、証明書の alert→CERTIFICATE_VERIFY_FAILED、拡張の alert→BAD_EXTENSION、他→SSL_HANDSHAKE_FAILURE、受けた致命の alert→1000+description）は、公開の API でもそのまま使えた（変更の案は無し）。
10. **NULL の引数**: SSL・context が NULL の関数は、失敗の値（0・-1・NULL・SSL_ERROR_SSL・`X509_V_ERR_UNSPECIFIED`）と SSL library の PASSED_NULL_PARAMETER を 1 つ queue する（free 系は何もしない）。

### 試験の注意

- socket が詰まる write（再試行の約束）は POSIX だけで WANT_WRITE を必須にした（Windows は 1 MiB を受け取りうる）。
- Windows の `long` は 32 bit なので、TIME_AFTER は `int64_t`。
- 試験は PEM file を試験の program の directory に書く（名前は `api-ssl-*.pem`。消さない。`build/tmp/ws335-unit/` の中）。

### 未実施・限界

- malloc の失敗の注入（ENOMEM の分類、SSL_CTX_new・SSL_new の各 allocation の失敗）、2 GiB 超の SSL_pending（わざと壊す確かめの生き残り 2 件）。
- 実機の Windows（Wine のみ）、TLS 1.2 の RSA の server（p033）、実 server との相互接続（p033）。
- `SSL_CTX_set_default_verify_paths` は 1 を返して queue が空であることだけ確かめた（Windows の ROOT・Disallowed の内容は p015 の試験）。

### Q1 に返す物

- header の comment（p035）: sslc-api.h の「SSL_new copies its settings」は実装と違う（上の 1）。ssl.h の `SSL_set1_host` の comment は「NULL clears both」に空の text も足すと正確。
- 消す物: `build/tmp/p032/`、`build/tmp/p032-mut/`、`build/tmp/p032-ossl/`（worktree の中。OpenSSL 3.5.8 の doc と header の抜粋、わざと壊す確かめの出力）。
