<!-- awesome-plan project=zedbsd record=ws335 -->

# WS335 libssl-compat と libcrypto-compat: OpenSSL の libssl・libcrypto と互換の口を持つ、zedBSD の自前の TLS と暗号の library

- Master: [plan/master.md](../master.md)（2026-11-01 作成）。Primary Milestone: MG002。
- Status: incomplete（2026-11-01 01:20 新設。2026-11-01 05:45 p001 の設計が cleared、header の基点 b7dfd3952、実装の段へ）
- 目標: OpenSSL の libssl の API（と、それが要る libcrypto の部分）と source の互換を持つ library を、外部の library を取り込まずに自前で実装する。
  zedBSD の program（libbrowser の `net/tls.c` など）と、Windows の OmniGrammaton（[WS334](../ws334/ws.md) の決定 16）が、OpenSSL から link を替えるだけで切り替えられる形にする。
- 進め方: 標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)）。p001 は設計の担当（Opus）が仕様と module・header・試験の道具を作り、
  p002 以降は実装の担当（Sonnet 5.5 high）が module ごとに実装する。
- 関係: [WS336](../ws336/ws.md)（自前の sshd・ssh、この WS の暗号の口を使う。並べて進め、依存は待ち合わせる）、[WS334](../ws334/ws.md)（決定 16: 当面 OpenSSL、libssl-compat ができたら切り替える）、[WS172](../ws172/ws.md)（passkey の FIDO2 の暗号が OpenSSL の libcrypto を使う例外、
  Guardrail の例外の表「リリースの前に独自実装に置き換える」）。

## ユーザーの決定

- （2026-11-02 10:52）ユーザーから、WS335 の完了の後に、Windows 版のブラウザ（WS334）の OpenSSL を libssl-compat・libcrypto-compat に切り替えて OpenSSL への依存を無くす、という趣旨の指示を受けた（p036 の切り替えを Windows にも、WS335 の後）。
- （2026-11-02 10:07）ユーザーから、WS335 を completed まで進める、Sonnet なら 3 並列で実装してよい、という趣旨の指示を受けた。
- 2026-11-01 ユーザーから、WS を立てて libssl-compat を実装する、標準廉価実装手順を使う、今 P6 で始める、という趣旨の指示を受けた。
- 2026-11-01 ユーザーから、いったん libbrowser に要る範囲だけを実装すればよい、header は他の compat の library と同じ置き方にし、本家（OpenSSL）の header とは分ける、という趣旨の指示を受けた（他の compat は `include/libc/compat/<名前>/`、例 `include/libc/compat/png/png.h`）。
- 2026-11-01 ユーザーから、最終的には OpenSSH も独自の sshd・ssh に置き換えるので、libssl-compat は OpenSSH の libcrypto の使い方に合わせる必要は無く、（後の独自の ssh が使える）口だけ作っておけばよい、という趣旨の指示を受けた（Q1 の解釈: 暗号の部品（hash・MAC・AEAD・鍵の交換・署名・乱数）を内部の口として分けて持ち、OpenSSH 互換の libcrypto の API は作らない）。
- 2026-11-01 ユーザーから、後の独自の sshd・ssh（OpenSSH の置き換え）は、鍵は ed25519 だけ使えればよく、鍵の交換も最新の定番を 1 つ持ち、OpenSSH の sshd と独自の sshd の両方に接続できればよい、という趣旨の指示を受けた（libssl-compat の暗号の口はこれを満たす部品を揃える: Ed25519 の署名と確かめ、鍵の交換の 1 つ（候補 mlkem768x25519-sha256 か curve25519-sha256、設計で案）、それに要る hash と AEAD）。
- 2026-11-01 ユーザーから、libcrypto-compat も作る、範囲は libbrowser と自前の ssh・sshd（[WS336](../ws336/ws.md)）で使う部分だけ、これは WS335 に足す、という趣旨の指示を受けた（前の「OpenSSH の libcrypto の互換は作らない」の読みを改める: OpenSSL の libcrypto と互換の API を、この 2 つが使う部分だけ作る。header は libssl-compat と同じく `include/libc/compat/` の下に本家と分けて置く）。
- 2026-11-01 ユーザーから、U1〜U6 は推しの案（どれも A）でよい、ただし Windows では（DLL を開くのでなく）static link にする、という趣旨の答えを受けた（U2 の Windows の部分と U4 の Windows の置き場所を直す）。
- 2026-11-01 ユーザーから、Windows では DLL を作らず 1 つの .exe にする、という趣旨の指示を受けた（libcrypto-compat・libssl-compat の .a は libbrowser.dll でなく OmniGrammaton の .exe に link する）。

## Resume point

- p001（設計）を P6 が q1789 で進めている。
- 2026-11-01 P6: 設計を [design.md](design.md)、問い U1〜U6 を [user-questions.md](user-questions.md) に書いた。header（libcrypto-compat 24＋公開 6、libssl-compat 8＋公開 1）・.c の骨格（29＋10）・試験の道具（[tests/](tests/)）を置いた。
  実装の Phase p002〜p038（提案の p036 を含む）の phase.md を置いた。design-reviewer の 27 件を全部反映した（[design-review.md](design-review.md)）。
  最初の凍結の基点は 60760d2a0。
- 2026-11-01 P6（q1789-i02）: U1〜U6 の答え（全部 A、Windows は DLL を作らず 1 つの .exe への static link）を入れた。Windows は `libcrypto-compat.a`・`libssl-compat.a` を使う側の build が `sources.mk` から作り .exe に link（design.md D23・D26、§4.3・§4.4）、
  `cert.pem` は .exe の隣、Disallowed は鎖の全部で拒む（D24）、名前の約束（D25）。header 5 個・骨格 4 個・試験の道具 2 個（`run-unit.sh --win32`、check-skeleton の 6・7）を直した。WS336 のために libcrypto-compat.so だけを先に作る p039（p023・p024 の後、D27）を足し、p033 から外した。
  design-reviewer の 2 回目を反映した（[design-review.md](design-review.md)）。
  **header と試験の道具の凍結: 基点 b7dfd3952**（`BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh`）。起動の順は design.md §9。仕様と試験の data は共有の `/home/awe/zedBSD-claude1/build/sources/specs/tls/`・`/home/awe/zedBSD-claude1/build/sources/tls-vectors/`（`WS335_VECTORS` に渡す）。

## Phase

| ID | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計（標準廉価実装手順 §4）: 範囲（TLS の版・暗号の組・証明書の確かめ・OpenSSL の API の部分集合）、仕様の入手、module・header・.c の骨格・試験の道具、実装の Phase の表、design-reviewer、ユーザーへの問い | cleared（2026-11-01 05:45 Q1、main 8e44fa17e。基点 b7dfd3952、Windows は 1 つの .exe に static link、p039 を足した） | — |
| [p002](phase002/phase.md) | 土台（約束・定数時間の補助・byte 順）、SHA-2、hash の選択・HMAC・HKDF（test-util、test-sha2、test-hash） | cleared（2026-11-01 06:59 Q1、main 6ea18b1a1。土台・SHA-2・hash の選択・HMAC・HKDF 実装、run-unit util 169160・sha2 14092・hash 12965 checks PASS（posix は Q1 再実行で確認、win32 は担当の Wine で PASS）、run-ct PASS。header の黙る所で HKDF の NULL と長さの組を EINVAL に（header は ENOTSUP だけを約束、規約の見直しの Phase で header の comment を直す）） | p001 |
| [p003](phase003/phase.md) | SHA-3・SHAKE（Keccak）（test-sha3） | cleared（2026-11-01 09:04 Q1、main b4c57515e。08:55 Sonnet、test-sha3 629616 checks、posix・win32・ct PASS、13 通りの破壊で FAIL） | p002 |
| [p004](phase004/phase.md) | AES（bitsliced、定数時間）と counter mode（test-aes） | cleared（2026-11-01 20:55 Q1、main adf13ed5c。main で test-aes 28516 checks・ct-aes PASS を流し直した。P6 が決めた 5 点（schedule の配置、S-box の回路、NULL key の EINVAL、size 0、counter は公開の値）は妥当と判断。壊す確かめは 24 通りが FAIL、試験の失敗と build の誤りの区別は 5 通りだけ）。前: 実装済み・試験 PASS（2026-11-01 20:55 P6。test-aes 28516 checks、posix・win32・ct PASS、24 通りの破壊で FAIL。cleared は Q1） | p002 |
| [p005](phase005/phase.md) | AES-GCM（test-gcm） | cleared（2026-11-01 21:33 Q1、main 72bb7908a。main で run-unit all 17 本・run-ct gcm PASS を流し直した）。前: 実装済み・試験 PASS（2026-11-01 21:29 P6。test-gcm 235197 checks、posix・win32・ct PASS、54 通りの破壊のうち 50 通りが試験の失敗で FAIL、見逃し 4 通りは等価か実行できない。cleared は Q1） | p004 |
| [p006](phase006/phase.md) | ChaCha20・Poly1305・AEAD（と 64 bit の counter の ChaCha20）（test-chacha） | cleared（2026-11-01 09:23 Q1、main d0e048532。Sonnet、chacha 52156 checks、posix・win32・ct PASS） | p002 |
| [p007](phase007/phase.md) | 固定幅の整数と Montgomery の算術（test-bn） | cleared（2026-11-01 22:26 Q1、main 3666e69f0。main で run-unit all・run-ct bn PASS を流し直した） | p002 |
| [p008](phase008/phase.md) | GF(2^255-19) と X25519（test-f25519、test-x25519） | cleared（2026-11-01 09:45 Q1、main 7ebb5367e）。実装済み・試験 PASS（2026-11-01 09:50 Sonnet。f25519 22903 checks、x25519 4538 checks、posix・win32・ct PASS、21 通りの破壊で FAIL。cleared は Q1） | p002 |
| [p009](phase009/phase.md) | Ed25519（test-ed25519） | cleared（2026-11-02 01:21 Q1、main 1331cb45b。main で test-ed25519 7289・ct-ed25519 PASS。k を L で reduce しない選択は RFC 8032 5.1.7 の文面どおりとして認めた） | p008 |
| [p010](phase010/phase.md) | P-256・P-384 の群（test-ecp） | cleared（2026-11-01 23:19 Q1、main 47700ab9c。main で run-unit all・run-ct ecp PASS を流し直した） | p007 |
| [p011](phase011/phase.md) | ECC: 鍵の生成・ECDH・ECDSA の確かめ（test-ecc） | cleared（2026-11-01 23:51 Q1、main 2d7097dd3。main で run-unit all・run-ct ecc PASS） | p010 |
| [p012](phase012/phase.md) | RSA の署名の確かめ（PKCS#1 v1.5・PSS）（test-rsa） | cleared（2026-11-02 01:07 Q1、main 9aa6bae4d。test-rsa 31057 PASS） | p007、p002 |
| [p013](phase013/phase.md) | ML-KEM の多項式・符号化・sampling（test-mlkem-poly） | cleared（2026-11-02 01:44 Q1、main 3b0b0dd1e。main で test-mlkem-poly 10117・ct PASS。不正な d の扱いは p035 で header の comment に足す） | p003 |
| [p014](phase014/phase.md) | K-PKE と ML-KEM-768（test-mlkem） | cleared（2026-11-02 02:04 Q1、main f533fd4cd。main で test-mlkem 8135・ct-mlkem PASS。Wycheproof 4 file 全部、暗黙の拒否も一致） | p013 |
| [p015](phase015/phase.md) | OS の層（POSIX と Windows）（test-os） | cleared（2026-11-02 03:02 Q1、main で test-os posix 1450・win32 1455 PASS。Q1 の判断: Wine の試験は run-unit.sh が LC_ALL=C.UTF-8 を付けて流す（host の locale に依らず日本語の file 名を作れる）。header の comment の補いは p035）。実装済み・試験 PASS（2026-11-02 02:59 P6。test-os posix 1450・win32 1455 checks、run-unit all・check-skeleton PASS、79 通りの破壊のうち 63 通りが check の FAIL・4 通りが sanitizer の中断・4 通りが hang で落ち、見逃し 8 通りは等価か実行できない。Wine の locale は `LC_ALL=C.UTF-8` が要る。cleared は Q1） | p002 |
| [p016](phase016/phase.md) | DER の reader と PEM・base64（test-der、test-pem） | cleared（2026-11-02 01:47 Q1、main 03a1ab446。main で test-der 1967・test-pem 666 PASS。bundle 121 枚・limbo 19,587/19,589 枚が読める） | p002 |
| [p017](phase017/phase.md) | X.509 の parse（証明書・公開鍵・署名の algorithm）（test-x509） | cleared（2026-11-02 02:46 Q1、main で test-x509 159986 PASS。Q1 の判断: critical でない nameConstraints も受けて処理する（RFC 5280 4.2.1.10 は CA に critical を求めるが、読む側が拒むと制約の付いた root を読めず、制約は処理すれば守れる）。header・試験は p020 の前に直す。ECDSA の parameters の header の comment は p035）。実装（2026-11-02 02:36 Sonnet。test-x509 159986 checks、posix・win32 PASS、254 通りの破壊のうち 241 通りが試験の失敗、3 通りが sanitizer、10 通りは等価。limbo 39,936 枚のうち 16 枚を拒否（1 枚は expected SUCCESS: header の nameConstraints critical 必須との食い違い）。cleared は Q1） | p016 |
| [p018](phase018/phase.md) | 署名の確かめ（algorithm ごと）（test-sigver） | cleared（2026-11-02 06:09 Q1、main で test-sigver 492861 PASS）。実装済み・試験 PASS（2026-11-02 04:21 Sonnet q1902-i01。test-sigver 492859 checks、posix・win32 PASS、70 通りの破壊のうち 63 通りが試験の失敗か crash、見逃し 7 通りは等価か冗長。cleared は Q1） | p017、p011、p012 |
| [p019](phase019/phase.md) | 名前（Name の比較・host と address の照合・name constraints）（test-name） | cleared（2026-11-02 06:09 Q1、main で test-name 255567・test-x509 PASS。dNSName の制約の先頭 dot は header に従い p020 で見直す）。実装済み・試験 PASS（2026-11-02 Sonnet。test-name 255567 checks、posix・win32 PASS、1633 通りの破壊のうち 1470 通りが検出、159 通りは等価、4 通りは build の誤り（別の形 7 通りで検出）。Q1 の承認済みの変更: critical でない nameConstraints を受理（cryc-x509.c・header の 1 語・test-x509 の拒否 id 一覧、limbo の拒否は 16 枚から 13 枚）。limbo の nameConstraints の case 9538 のうち 4 件は expected と違い p020 へ。cleared は Q1） | p017 |
| [p020](phase020/phase.md) | trust store と path の検証（test-chain） | cleared（2026-11-02 10:10 Q1、main で test-chain 22305 PASS。limbo 179 件のうち 46 件の違いは理由付きで固定、p019 の残り 4 件は RFC 5280 6.1 で決めた）。実装済み・試験 PASS（2026-11-02 Sonnet 5.5 q1912-i01。test-chain 22305 checks、posix・win32 PASS、limbo 179 件のうち 133 件が一致・46 件は理由を付けた違い（試験の表に固定）、1165 通りの破壊のうち 949 通りが検出で見逃し 216 通りは観測できる違いが無い物。cleared は Q1） | p018、p019、p015 |
| [p021](phase021/phase.md) | 誤りの queue と ERR_・RAND_bytes・OPENSSL_cleanse・CRYPTO_memcmp（test-err） | cleared（2026-11-02 03:44 Q1、main で test-err 4590・ct-err PASS）。実装済み・試験 PASS（2026-11-02 Sonnet 5.5。test-err posix 4590・win32 4587・ct-err 5 checks、わざと壊す 158 通りのうち 149 が check の FAIL・10 が sanitizer の中断・見逃し 3 は実行できない経路。cleared は Q1） | p015 |
| [p022](phase022/phase.md) | 公開の X509（d2i_X509 ほか）と内部の store の生成（test-api-x509） | cleared（2026-11-02 11:14 Q1、main で test-api-x509 PASS）。実装済み・試験 PASS（2026-11-02 Sonnet 5.5 q1927-i01。test-api-x509 posix 507799・win32 507796 checks、わざと壊す 784 通りのうち build できる 772 通りで 732 検出・見逃し 40 は OOM の経路 14 と等価 26。cleared は Q1） | p020、p021 |
| [p023](phase023/phase.md) | 公開の EVP の digest・ChaCha20・Poly1305（test-api-evp） | cleared（2026-11-02 06:09 Q1、main で test-api-evp 190733・ct PASS。鍵の解放の 36 checks は p024 の後に流し直す）。実装済み・試験 PASS（2026-11-02 Sonnet 5.5。test-api-evp 190733 checks、posix・win32・ct PASS、940 通りの破壊のうち 658 が check の FAIL・132 が sanitizer の中断、見逃し 137 は等価か実行できない経路、13 は型の誤りで build できず。cleared は Q1） | p021、p006、p002 |
| [p024](phase024/phase.md) | 公開の EVP_PKEY（Ed25519・X25519・ML-KEM-768）（test-api-pkey） | cleared（2026-11-02 09:40 Q1、main で test-api-pkey 40701・test-api-evp 190769（鍵の解放の 36 も）・ct PASS）。実装済み・試験 PASS（2026-11-02 Sonnet 5.5 q1913-i01。test-api-pkey 40701 checks、posix・win32・ct PASS、test-api-evp の鍵の解放 36 checks が流れて 190769 PASS、WS336 の契約の試験 108 checks PASS、わざと壊す 1715 通りのうち 1370 が試験の失敗か sanitizer、見逃し 225 は 3 を試験で塞ぎ残りは等価か実行できない経路、120 は型の誤りで build できず。cleared は Q1） | p021、p009、p008、p014 |
| [p025](phase025/phase.md) | TLS の定数・suite の表・presentation language の reader/writer（test-tls、test-buf） | cleared（2026-11-02 01:07 Q1、main で test-tls 2230・test-buf 1559755 PASS） | p002 |
| [p026](phase026/phase.md) | record の保護（test-record） | cleared（2026-11-02 02:15 Q1、main で test-record 240579 checks・ct-record PASS）。実装（2026-11-02 02:15 Sonnet。test-record 240579 checks、ct-record 49 checks、posix・win32・ct PASS、63 通りの破壊のうち 58 通りが試験の失敗で FAIL、見逃し 5 通りは全部等価。cleared は Q1） | p025、p005、p006 |
| [p027](phase027/phase.md) | transcript と鍵の schedule（1.3 と 1.2）（test-keysched） | cleared（2026-11-02 01:41 Q1、main で test-keysched 79369・ct-keysched PASS。段の順は呼び出す側の p029 の責任） | p025、p002 |
| [p028](phase028/phase.md) | handshake の message の encode と syntax の確かめ（test-msg） | cleared（2026-11-02 02:34 Q1、main で test-msg 936784 checks PASS。Q1 の判断: ServerHello・HRR で、知っていてその message に指定の無い拡張は RFC 8446 4.2 どおり illegal_parameter、送っていない拡張は unsupported_extension に分ける。code と header の comment は p035 で直す）。実装（2026-11-02 02:30 Sonnet q1893-i01。test-msg 936784 checks、posix・win32 PASS、153 通りの破壊のうち 146 通りが試験の失敗で FAIL、見逃し 7 通りは等価。cleared は Q1） | p025 |
| [p029](phase029/phase.md) | handshake の共通と TLS 1.3（test-hs13） | cleared（2026-11-02 11:25 Q1、main で test-hs13 18565 PASS。client の handshake の write の鍵は server の Finished の後（ServerHello の直後の alert は平文）、p033 の相互運用で OpenSSL の扱いを確かめる）。実装済み・試験 PASS（2026-11-02 11:23 P6 q1928-i01、Opus。test-hs13 18565 checks、posix・win32 PASS、run-unit all・check-skeleton（BASE=4eae22020）PASS、わざと壊す 120 通りのうち 112 が試験の失敗・2 が ASan の検出、残り 6 は等価。TLS 1.2 の SH の固有の確かめは p030 の sslc_hs12_server_hello。cleared は Q1） | p027、p028、p020、p011、p008、p037 |
| [p030](phase030/phase.md) | TLS 1.2 の handshake（test-hs12） | cleared（2026-11-02 12:21 Q1、main で test-hs12 17451 PASS。RSA の道の 13 件の見逃しは p033 で、1.2 の ServerHello の cookie の alert の種類は p035 で決める）。実装済み・試験 PASS（2026-11-02 12:20 Sonnet q1937-i01。test-hs12 17451 checks、posix・win32 PASS、run-unit all・check-skeleton（BASE=4eae22020）PASS、わざと壊す 166 通りのうち 140 が試験の失敗、残り 26 は RSA の道 13（p033）・redundant な守り 6・観測できない 7。cleared は Q1） | p029、p037 |
| [p031](phase031/phase.md) | 接続（record の I/O・再組み立て・alert・close）（test-conn） | cleared（2026-11-02 13:41 Q1、main で test-conn 164417 PASS。reason の表と alert 43・109・ENOMEM の未試験は p032・p033 で）。実装済み・試験 PASS（2026-11-02 13:15 Sonnet q1943-i01。test-conn 164417 checks、posix・win32 PASS、run-unit all・check-skeleton（BASE=4eae22020）PASS、わざと壊す 216 通りのうち 202 が検出・生き残り 14 は等価な守り 11・試験の道具で作れない 3。cleared は Q1） | p030、p015、p037 |
| [p032](phase032/phase.md) | 公開の SSL・SSL_CTX（test-api-ssl） | cleared（2026-11-02 15:49 Q1、main で test-api-ssl PASS）。実装済み・試験 PASS（2026-11-02 15:30 Sonnet q1953-i01。test-api-ssl posix 5129・win32 5570 checks、run-unit all・check-skeleton（BASE=4eae22020）PASS、わざと壊す 113 通りのうち 106 が検出・生き残り 7 は等価な守り 5・未試験 2（malloc の失敗・2 GiB の SSL_pending）。SSL_new でなく最初の handshake の call で設定を読む点を Q1 に返す。cleared は Q1） | p031、p022 |
| [p033](phase033/phase.md) | 統合: zedBSD の libssl-compat.so と exports、Windows の static library の一覧、相互接続（OpenSSL s_server、Wine も）・x509-limbo・net/tls.c の host 試験（Opus） | cleared（2026-11-02 17:43 Q1、f31fbecbd を main に merge d2e15818e、main で libssl-compat.so・libwebp-compat.so warning 0、check-exports PASS）。実装済み・試験 PASS（2026-11-02 17:35 P1 q1971-i01。libssl-compat.so（amd64・aarch64）warning 0・check-exports PASS、run-interop host 110 件（known limit 8: TLS 1.2 × ECDSA P-384 の leaf、p040）・Wine 44 件（cert.pem と Disallowed）・zedBSD の QEMU 30 件、run-limbo 9,792 件で理由の無い違い 0、net/tls.c の host 試験 http 15・loader 11 が OpenSSL と同数、run-faults（確保の失敗・alert 109）PASS、run-unit all・--win32 all・run-ct all・check-skeleton（BASE=4eae22020）PASS。guest の usb-net の不具合は BUG-416。cleared は Q1） | p032、p023、p024、p039 |
| [p034](phase034/phase.md) | 安全の review と hardening: 定数時間・fuzz・checklist（Opus） | cleared（2026-11-02 21:35 Q1、a37f972c7 を main に merge ec48eb04d、main で test-chain・conn・api-ssl・hs13・name PASS、check-skeleton は BASE=a37f972c7 に移して PASS）。実装済み・試験 PASS（2026-11-02 21:35 P1 q1986-i01。指摘 21 件: 直した 12（clang の ct の分岐 bn と潜在 5 か所、Disallowed の ECDSA 署名の書き換えでの通り抜け、Windows の EKU、iPAddress の mask、close_notify の後の送り、SSL_connect、1.2 の CCS、消去 2）、見送り 9 は理由付き。ct matrix 72/72、fuzz 11 target × 1 時間 crash 0、run-unit all・--win32 all・interop・limbo PASS。check-skeleton（BASE=ddc678f690）は承認済みの cryc-chain.h の comment だけで FAIL。cleared は Q1）。前: in-progress（2026-11-02 18:41 P1 q1986） | p033 |
| [p035](phase035/phase.md) | 規約の全文の見直しと header の持ち越しの直し | cleared（2026-11-12 Q1 判定: P2 q2019 で全試験 PASS（posix・win32 の unit 59 本ずつ、ct 19 本と matrix 76、interop host 139・win32 47、limbo 9,792 件、fuzz 11 target）、check-skeleton は許可した header の 18 file だけで FAIL。check-skeleton の BASE を 9e6a72f402 に移す） | p034 |
| [p036](phase036/phase.md) | 提案（U2 は A で答え済み）: net/tls.c の libssl-compat への切り替え（zedBSD は開く名前、Windows は static link の表）。WS074・WS334 と調整 | planning | p033、WS334 の p022・p030 |
| [p037](phase037/phase.md) | 試験の道具: scripted な TLS server（tlspeer.h）と試験の側の ECDSA の署名器（Opus。p029〜p032 が使う） | cleared（2026-11-02 03:59 Q1、main で test-tlspeer 35566 PASS）。実装済み・試験 PASS（2026-11-02 04:20 P6 q1904-i01。test-tlspeer 35566 checks、posix・win32 PASS、RFC 8448 3・5 節の server の側を byte ごとに再生、24 通りの壊しは全部 FAIL。RSA の秘密鍵は持たない（Q1 の判断、RSA の server は p033）。cleared は Q1） | p025、p026、p027、p011、p008 |
| [p038](phase038/phase.md) | x509-limbo の SERVER 全 9,792 件の照合と違いの理由の分類（Opus） | cleared（2026-11-02 11:50 Q1、limbo 9,792 件のうち 9,733 件が一致・59 件は理由付き・理由の無い違い 0。Q1 の判断は ws.md の「Q1 の判断（p038）」）。実装済み・試験 PASS（2026-11-02 P7 q1929-i01。test-limbo 95464 checks、posix・win32 PASS、9,792 件のうち一致 9,733・理由つきの違い 59（issuance 11・cabf 23・no-crl 10・name 4・no-mldsa 3・anchor 2・key-usage 2・nc-leaf 2・depth 1・nc-noncritical 1）・理由の無い違い 0。cryc の欠陥は無し。16 枚を越える list は名前で辿れる certificate に減らしてから切る（bettertls pathbuilding の 20 件が path の組み立てを経る）。anchor の制約・CA/B Forum の規則・先頭の dot は Q1 に案を返した。cleared は Q1） | p020 |
| [p039](phase039/phase.md) | zedBSD の libcrypto-compat.so（build の規則・exports・package と image の登録、`sources.mk`）。TLS は無し。WS336 の zedBSD の Phase が待つ | cleared（2026-11-02 10:21 Q1、main で libcrypto-compat.so の build warning 0・check-exports PASS。version node は置かない（Q1、p035 で design.md を直す）。dlclose の NODELETE は BUG-409）。実装済み・試験 PASS（2026-11-02 10:20 Sonnet 5.5 q1925-i01。amd64・aarch64・riscv64 の libcrypto-compat.so が warning 0 で build、check-dynamic-elf と check-exports PASS、QEMU の serial で crypto-smoke 47 checks PASS、run-unit all・check-skeleton PASS。map は version node 無し（Q1 承認）。ld.so が NODELETE を守らない件は phase039 の結果に。cleared は Q1） | p023、p024 |
| [p040](phase040/phase.md) | secp384r1 の group: supported_groups で申し出る（x25519・secp256r1 の後）、TLS 1.2 の ServerKeyExchange の P-384 の ECDHE、TLS 1.3 の HRR の後の P-384 の key share、共有の秘密 48 byte（凍結の header の変更は Q1 の承認の後）。p033 の 8 件の known limit を繋がる側に戻す | cleared（2026-11-02 18:41 Q1、ddc678f69 を main に merge 4535e4137、main で test-hs13・hs12・api-ssl PASS、check-skeleton は BASE=ddc678f69 に移して PASS）。実装済み・試験 PASS（2026-11-02 18:30 P1 q1981-i01。run-unit all・--win32 all（57 本）・run-ct all PASS、run-interop host 139 件・Wine 47 件が known limit 0 で PASS、run-limbo PASS、ecc384.badssl.com が繋がる、zedBSD の .so warning 0・check-exports PASS、check-skeleton（BASE=4eae22020）は承認済みの sslc-hs.h・sslc-keysched.h の差だけで FAIL。cleared は Q1） | p033（p034 の前） |

- 2026-11-02 03:56 Q1: check-skeleton の BASE は 641a2f195 に移す（p015 で Q1 が run-unit.sh に Wine の LC_ALL=C.UTF-8 を足したため。header と道具はこの commit で固定）。


## Q1 の判断（2026-11-02 11:50、p038 の問い）

- 信頼の起点（anchor）: 知らない critical の拡張を持つ anchor は断る（B）。basicConstraints は anchor に求めない（Windows の ROOT の store の版 1 の root を断らないため）。cryc-chain.h の 1 文と実装の変更は p035 で行う。
- CA/Browser Forum の規則（23 件）は守らせない。D11（RFC 5280 6.1 の web PKI の client）のとおり。private な CA（例: mail の試験の server の証明書は EKU を持たない）を断らないため。
- dNSName の制限の先頭の「.」は、今の header の規則（その下の名前だけ）のまま。
- run-unit.sh の 1 本あたりの timeout（600 秒）に test-limbo（ASan で約 6 分）が近い。p035 で timeout を 1200 秒にする（試験の道具の変更、Q1 の判断）。

## Q1 の判断（2026-11-02 15:49、p032 の報告）

- SSL の設定（option・verify の mode・版の範囲）は、今は最初の handshake の呼び出しで context から読む。OpenSSL と header の約束（SSL_new が写す）に合わせ、SSL_new の時に写す形に p035 で直す（ssl_st に写しの field を足す。header の変更は p035 の範囲）。
- SSL_VERIFY_NONE で verify の callback を呼ばない点、SSL_get_error の写しの表、SSL_shutdown の扱い、SNI の IP の文字列を送らない扱いは、p032 の結果のとおりでよい。
- p035 に回す header の comment の直し: sslc-api.h の「SSL_new copies its settings」、ssl.h の SSL_set1_host の空の文字列。

## Q1 の判断（2026-11-02、p033 の問い。P1 が 16:47 に記録）

- TLS 1.2 で ECDSA P-384 の leaf の server に繋がらない件（RFC 8422 5.1: TLS 1.2 の server は client の supported_groups の曲線の上の ECDSA の証明書だけを使う。libssl-compat は x25519 と secp256r1 だけを申し出る）は B: secp384r1 を足す。web の client は Chrome・Firefox（x25519・secp256r1・secp384r1 を申し出る）と同じ所に繋がるべきなので。ただし p033 の中ではしない。
  p033 は run-interop.sh の 8 件を「known limit」（server が no shared cipher で断る事を確かめる）として別に数えて終える。足す作業は [p040](phase040/phase.md)（p033 の後、p034 の前。凍結の header の変更は Q1 の承認の後、design.md D8 の文は案として phase040 に）。


## Q1 の判断（2026-11-02 17:43、p040 の header の変更）

- phase040 §3 の凍結の header の変更（sslc-hs.h の struct sslc_hs の P-256 の field を曲線を問わない ec_* に、鍵の共有の大きさ、sslc-keysched.h の comment、試験の道具 tlspeer.h の group）を承認する。HAL ではない。check-skeleton の BASE は p040 の後に Q1 が移す。D8 の案（x25519・secp256r1・secp384r1 の順、key share は x25519 の 1 つ、HRR で他）を採る。design.md への反映は p035。

## Q1 の判断（2026-11-02 18:41、p040 の merge）

- check-skeleton の BASE を ddc678f690（p040 の終わり）に移す。p035 に回す header の comment: sslc-tls.h の file の comment と sslc-msg.h:35 の「group は x25519 と secp256r1 だけ」、design.md の D8 の本文。

## Q1 の判断（2026-11-02、p034 の問い。P1 が 20:25 に記録）

- Windows の Disallowed の印（D24）は、DER の全体でなく tbsCertificate の SHA-256 にする（案 A。DER が parse できない時は全体の SHA-256）。ECDSA の署名は (r, n − s) に書き換えても有効なので、DER の全体の印では、盗まれた中間を Disallowed に入れても、署名を書き換えた写しが通ってしまうため。凍結の header cryc-chain.h の 2 つの comment（struct cryc_store と cryc_store_add_distrusted）の変更を承認する。design.md の D24 の文の直しは p035 で。
- 各 fuzz の target を 1 時間、11 個を並べて流す事は、ユーザーの承認を Q1 が伝えた（2026-11-02）。

## Q1 の判断（2026-11-02 21:35、p034 の merge）

- check-skeleton の BASE を a37f972c7a（p034 の終わり）に移す。WS343 p002（P2）が libcrypto-compat の公開の header を広げるので、その merge の後に Q1 がもう一度移す。

## Q1 の判断（2026-11-02 22:20、WS343 p002 の merge）

- check-skeleton の BASE を b8c4adaf6c に移す（WS343 p002 が公開の暗号の header と cryc-aes.h・cryc-api.h・cryc-err.h、check-skeleton.sh の名前の決まりを広げた）。p035 は design.md §5 と evp.h の範囲の note に passkey の部分集合（HMAC、EVP_KDF の HKDF、OSSL_PARAM、EC P-256 の PKEY、AES-256-CBC）を書き入れる。
- 2026-11-12 Q1: `run-fuzz.sh` を同時 4 本までにした（q2021）。check-skeleton の BASE をこの merge に移す（BASE は `git log -1 --format=%h -- plan/ws335/tests/run-fuzz.sh` の merge）。
