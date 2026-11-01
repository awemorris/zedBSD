<!-- awesome-plan project=zedbsd record=ws335-p037 -->

# ws335-p037: 試験の相手（scripted な TLS server）と試験の側の署名器（試験の道具）

Status: 実装済み・試験 PASS（2026-11-02 04:20 P6 q1904-i01。cleared は Q1。2026-11-01 p001 が design-review の指摘 1・12 を受けて足した。依存: p025・p026・p027・p011・p008）
Parent: [WS335](../ws.md)。担当: Opus（試験の道具の設計を含む。phase-runner を推す）。p029・p030・p031・p032 の試験が使う。

## 1. 読む物

- [design.md](../design.md) §8・§9、[design-review.md](../design-review.md) の 1・12。
- `userland/base/libssl-compat/sslc-tls.h`・`sslc-buf.h`・`sslc-record.h`・`sslc-keysched.h`・`sslc-msg.h`（定数と構造体）。
- 仕様（`/home/awe/zedBSD-claude1/build/sources/specs/tls/`）: rfc8446.txt（4・5・7、4.4.3 の署名の内容）、rfc5246.txt（7.4）、rfc8422.txt（5.4）、nist-fips-186-5.pdf（6.4.1 ECDSA の署名、印刷の頁 N は PDF の N+9）。

## 2. 作る物

- `plan/ws335/tests/unit/tlspeer.h`（試験の道具。p029 以降の Phase は変えない）:
  - 試験の側の ECDSA P-256 の署名器（FIPS 186-5 6.4.1、試験の固定の k、cryc_ecp と cryc_bn の検証済みの部品を使う。署名は p011 の確かめに通ること）。RSA の秘密鍵は持たない（RSA の server の試験は p033 の OpenSSL の s_server で）。
  - TLS 1.3 の server の flight の組み立て（ServerHello・HelloRetryRequest・EncryptedExtensions・Certificate・CertificateVerify・Finished、NewSessionTicket・KeyUpdate）と、
    TLS 1.2 の ECDHE の flight（ServerHello・Certificate・ServerKeyExchange・ServerHelloDone・CCS・Finished）。message は RFC の構文から試験の側で書く（sslc-msg の writer を使わない）。
    鍵の schedule と record の保護は p026・p027 の検証済みの関数を使ってよい（RFC 8448 で照合済み）。
  - client の ClientHello を読んで中身（random・session id・share・拡張）を返す最小の parser（試験の側で、sslc-msg を使わない）。
  - 壊し方の印（tag の 1 bit、Finished の 1 byte、unsolicited な拡張、downgrade の印、2 回目の HRR、ServerHello と EE の同じ record）。
  - 試験の証明書: derwriter.h で作る ECDSA P-256 の root と leaf（SAN に host 名、固定の有効期限）。
- `plan/ws335/tests/unit/test-tlspeer.c`: 道具そのものの試験（作った CertificateVerify が cryc_signature_verify で通る、作った record が sslc_record_open で開く、1.3 の Finished が sslc_tls13_finished と一致）。
- 結果の節、ws.md の行。

## 3. 終わりの条件

- `sh plan/ws335/tests/run-unit.sh tlspeer all` と `sh plan/ws335/tests/run-unit.sh --win32 tlspeer` が PASS（tlspeer.h は Windows でも compile できる C89 にする）。check-skeleton.sh（BASE なし）PASS。commit。
- p029〜p032 の phase.md が使い方を読めるよう、tlspeer.h の冒頭に API の説明（関数ごとの comment）。

## 結果

2026-11-02（host の `date` は 03:53〜04:20 JST）P6（Opus、q1904-i01）。worktree p6-ws335c（main 641a2f195）。

作った物: `plan/ws335/tests/unit/tlspeer.h`（約 3800 行、header だけの試験の道具）、`plan/ws335/tests/unit/test-tlspeer.c`。他に `plan/ws335/phase029/phase.md` に 1 行（Q1 が許した RSA の注記）。product の header・他の module の .c・試験の道具（run-unit.sh ほか）は変えていない。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh tlspeer` | `test-tlspeer: PASS (35566 checks)` / `run-unit: PASS`（ASan・UBSan、leak 検出あり） |
| `sh plan/ws335/tests/run-unit.sh --win32 tlspeer`（x86_64、Wine、static link） | `test-tlspeer: PASS (35566 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh all` | `run-unit: PASS`（test 28 本・ct 15 本、test-tlspeer は 35544 checks の時点の版） |
| `sh plan/ws335/tests/check-skeleton.sh`（BASE なし）と `BASE=641a2f195 sh plan/ws335/tests/check-skeleton.sh` | どちらも `check-skeleton: PASS`（warning 0）。`BASE=b7dfd3952` は FAIL: main 641a2f195 の時点で `run-unit.sh` が b7dfd3952 と違う（Q1 の p015 の LC_ALL の変更）ため。Q1 の指示で以後の基点は 641a2f195 |
| host の clang（`-std=c89 -Wall -Wextra -Werror`）で test-tlspeer を build・実行 | warning 0、`test-tlspeer: PASS` |
| `git diff --check` | 出力なし |

試験の中身（test-tlspeer.c）:

- 署名器: CAVP 186-4 `SigGen.txt`（d と k を含む版。zip の中にあり vectors.py は .rsp だけを rec にするので、P-256/SHA-256 の 3 件と P-384/SHA-384 の 2 件を出典つきで試験に写した）で Q と (r, s) が一致、cryc_ecdsa_verify で通る。固定の k は決定的で、hash が変われば変わる。DER の形の往復、k = 0・k = n・d = 0・d = n の拒否、hash が order より短い・長い場合。
- 証明書: P-256・P-384 の root と leaf を cryc_x509_parse（p017）で読み、version・有効期限（TLSPEER_NOT_BEFORE_TIME・_AFTER_TIME）・鍵・basicConstraints・keyUsage・extKeyUsage・SAN（dNSName、iPAddress）・SKI と AKI の値を確かめ、tbsCertificate の署名を issuer の鍵で cryc_ecdsa_verify（試験の側の DER の decode）で確かめる。同じ spec は同じ bytes。
- ClientHello の reader: RFC 8448 3 と 5 の ClientHello の各 field、全ての切れ目（長さを直した物と直さない物、圧縮の後で切った物だけが拡張なしの正しい ClientHello）、後ろの 1 byte、同じ拡張 2 回、型の誤り、奇数の suite の長さ。
- RFC 8448 の再生: 3 節（1-RTT）と 5 節（HRR の後に secp256r1）の server の側を、trace の random・鍵・cookie・EncryptedExtensions・（RSA の）CertificateVerify を設定に与えて流し、ServerHello・HRR・EE・Certificate・Finished の message、server の handshake と application の鍵、server の全ての record（平文の ServerHello、暗号化された flight、NewSessionTicket、application data、alert）が trace と byte ごとに一致し、trace の client の record（Finished、data、alert）を受け取る。壊した client Finished は EBADMSG、壊した client の record は EBADMSG、cookie の無い 2 回目の ClientHello は EPROTO。
- SAME_RECORD を KeyUpdate の後に立てると、KeyUpdate と NewSessionTicket が古い鍵の 1 つの record に入る。
- 自前の証明書での handshake: 試験の側の client（RFC 8446・RFC 5246 から書き、鍵の schedule と record の保護は sslc の物）で TLS 1.3（x25519、HRR の後の secp256r1、AES-128-GCM・AES-256-GCM・ChaCha20、P-256 と P-384 の鍵、CCS・SNI_ACK・CERT_REQUEST の選択肢、1 byte の record と 1 byte ずつの入力、7 byte の record）と TLS 1.2 ECDHE（x25519・secp256r1、AES-GCM・ChaCha20、EMS あり・なし、RI なし、ec_point_formats、CertificateRequest）。CertificateVerify・ServerKeyExchange の署名が cryc_ecdsa_verify で通る、全ての record が sslc_record_open で開く、Finished が sslc_tls13_finished・sslc_tls12_finished と一致する、data・NewSessionTicket・KeyUpdate（両向き、request あり）・close_notify が通る。
- 壊し方と選択肢: FINISHED、SIGNATURE（形は正しく確かめに通らない）、RECORD_TAG（一度だけ、開くと EBADMSG）、SAME_RECORD（ServerHello と EE が 1 つの平文の record）、SESSION_ID（空の id と 32 byte の id、1.2 の echo）、CERT_CONTEXT、UNSHARED、ZERO_SHARE（1.3 と 1.2）、SECOND_HRR、RETRY_NO_GROUP、DOWNGRADE12・11、NO_RI・NO_EMS、余分な拡張（SH・HRR・EE・CR・CertificateEntry）、空でない client Certificate の拒否、手で作る record・bytes・HelloRequest、設定の範囲外の拒否。
- わざと壊す確かめ: tlspeer.h の写し（`build/tmp/p037/mut/`、tree の file は変えていない）を 24 通り壊した（HRR の random、message_hash の長さ、SH の拡張の順、署名の加算、n-2、EMS、Finished の比較、同じ拡張、KeyUpdate の鍵、壊し方の一度きり、keyUsage、CV の context、downgrade の印、読む鍵の切り替え 2 つ、cookie、session id、SKI、SKE の範囲、server_name、alert の分割、1.2 の server Finished の label、NewSessionTicket の長さ）。最初は 22 通りが FAIL、見逃した 2 つ（1.3 の空でない session id の echo、SKI の値）に試験を足し、24 通りとも FAIL になった。

設計の決定（header が黙る内部の事）:

- header だけの道具。全ての関数は static で unused の印。冒頭に API の宣言と関数ごとの説明を置き、定義は後ろ。
- 2 つの使い方: message（`tlspeer_handshake`・`tlspeer_change_cipher_spec`・`tlspeer_next`、sslc_hs の試験向け）と record（`tlspeer_input`・`tlspeer_take`、接続の試験向け）。1 つの peer でどちらか一方だけ。
- 独立: message は道具の自前の writer で RFC の構文から書き、ClientHello は自前の reader で読み、transcript は cryc_hash で自前（sslc-buf・sslc-msg・sslc_transcript を使わない）。鍵の schedule（sslc-keysched）と record の保護（sslc-record）は sslc の物（RFC 8448 で照合済み、ここでも再生で照合）。試験の client は RFC の定数（HRR の random、downgrade の印、CV の context）を道具とは別に持つ。
- 交渉しない: 設定が版・suite・group・scheme・証明書・選択肢・壊し方を決め、client の申し出によらずそれを送る。client の側（ClientHello の構文、group の share、client Finished、record の tag、cookie）は確かめ、最初の失敗を `peer->error`・`peer->why` に残す。
- 固定の k は HMAC（P-256 は SHA-256、P-384 は SHA-384）を d で key して hash と counter の byte にかけ mod n にした物（RFC 6979 ではない。決定的で message ごとに違う）。明示の k も渡せる（vector 用）。
- 証明書: v3、ECDSA（issuer が P-256 なら SHA-256、P-384 なら SHA-384）、Name は commonName 1 つ、UTCTime、SKI は SHA-256(公開鍵) の先頭 20 byte、AKI は issuer の SKI。root は basicConstraints cA・keyUsage keyCertSign|cRLSign、leaf は basicConstraints 空・digitalSignature・serverAuth・SAN。鍵は label "tlspeer root"・"tlspeer leaf" の hash。
- 記録の書き方: 同じ鍵の handshake message は record を共有（record_size まで）、alert と raw の record は分けない（RFC 8446 5.1）。TLS 1.3 の client の平文の alert はいつでも受ける（鍵を持つ前に失敗した client が送るため）。読む鍵が変わった時に handshake の bytes が残っていれば EPROTO。
- 足した物（p029〜p032 の試験が要る範囲）: CertificateRequest と空の client Certificate の受け取り、NewSessionTicket、KeyUpdate（request に自動で応える）、手で作る message・record・bytes、CERT_CONTEXT・UNSHARED・ZERO_SHARE・SESSION_ID・RETRY_NO_GROUP、RFC 8448 の再生のための `ee_raw`・`cv_raw`。
- 作らなかった物: RSA の秘密鍵（Q1 の判断、RSA の server は p033 の s_server）、ClientHello の writer（p029 の「固定の random で送る bytes の一致」は `tlspeer_parse_client_hello` の field と拡張の順で照合する）。
- cryc_signature_verify（p018）は main 641a2f195 では骨格（ENOSYS）。試験は cryc_ecdsa_verify と試験の側の DER の decode で確かめ、cryc_signature_verify は ENOSYS を返さなくなった時だけ数える（Q1 了承）。p018 の merge の後に test-tlspeer を流し直すと、その確かめも入る。

p029〜p032 が知るべき事:

- 典型: `tlspeer_pki_init(&pki, CRYC_CURVE_P256, host)`、`tlspeer_config_init(&config, SSLC_TLS13 か SSLC_TLS12, &pki)`、設定を変えて `tlspeer_new`。client は `pki.root`（`pki.root_size` byte の DER）を trust store に入れ、`TLSPEER_PKI_TIME` で確かめる。
- message の使い方で、client の `SSLC_HS_SET_READ_KEYS` の鍵は `peer->write_keys[item.epoch]`、`SET_WRITE_KEYS` は `peer->client_handshake_keys`・`client_application_keys` と比べられる。`tlspeer_next` の view は次の送る呼び出しまで有効。
- `peer->breaks` は途中で変えてよい（例: data の前に RECORD_TAG）。SAME_RECORD は OPT_CCS と組まない（CCS が間に入る）。KeyUpdate の後の同じ record は `tlspeer_send_key_update` の後に SAME_RECORD を立てて次の handshake message（例: `tlspeer_send_ticket`）を送る。
- 大きさ: peer は heap に約 600 KiB。1 byte の record では 1 byte が 23 byte前後になる（出力の上限 256 KiB）。

未実施・残り:

- Windows の aarch64 は compile と link だけ（Wine が無い）。check-skeleton の 6・7 は tests/unit の道具を対象にしない（library の名前の範囲だけ）。
- cryc_signature_verify の確かめは p018 の後に初めて数えられる。

消してよい物（Q1 に頼む）: `build/tmp/p037/`（smoke の source、clang の実行 file、わざと壊す確かめの作業 directory `mut/`、log）。scratchpad の外には他に作っていない。
