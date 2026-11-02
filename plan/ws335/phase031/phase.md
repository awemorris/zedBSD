<!-- awesome-plan project=zedbsd record=ws335-p031 -->

# ws335-p031: 接続（record の I/O・再組み立て・alert・close）

Status: 実装済み・試験 PASS（2026-11-02 13:15 Sonnet q1943-i01。cleared は Q1。前: planned、2026-11-01 p001 が作成。依存: p030、p015、p037）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libssl-compat/sslc-conn.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `rfc8446.txt`（SHA-256 `47871bc8820a2c3b6ea89f061055577058862cf543686b82d10131239702b3bd`）: 5、6。
  - `rfc5246.txt`（SHA-256 `2ebe1b8e651696aeacc8ab3848bedb465505737901ea3b691acf40c392958feb`）: 6、7.2。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: rfc8448。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libssl-compat/sslc-conn.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-conn.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- 試験の transport は memory の pipe（1 byte ずつ、EAGAIN を混ぜる）。相手は p037 の tlspeer.h。
- read ahead をしない事の試験: transport が求められた大きさを記録し、どの read も今の record の残り（header の 5 byte、次に length byte）を越えないこと。
- key change の境の試験: ServerHello と EncryptedExtensions を 1 つの record に入れる、KeyUpdate の後に同じ record で data を続ける（どちらも unexpected_message）。
- close_notify・EOF・alert・進まない record の上限。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- p037 の `plan/ws335/tests/unit/tlspeer.h`。全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh conn     # この module
sh plan/ws335/tests/run-unit.sh --win32 conn     # 同じ試験を Windows（x86_64、static link、Wine）で
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

2026-11-02（host の `date` は 12:30〜13:15 JST）Sonnet 5.5（q1943-i01、phase-runner-low）。worktree s-ws335-p031（branch agent/s-ws335-p031、main d32ba22f36 から）。

作った物: `userland/base/libssl-compat/sslc-conn.c`（骨格の ENOSYS を置き換え）、`plan/ws335/tests/unit/test-conn.c`。header・tlspeer.h・他の module の .c・試験の道具・Makefile は変えていない（interface の不備は見つからなかった）。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh conn` | `test-conn: PASS (164417 checks)` / `run-unit: PASS`（ASan・UBSan、leak 検出あり） |
| `sh plan/ws335/tests/run-unit.sh --win32 conn`（x86_64、Wine、static link） | `test-conn: PASS (164417 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh all` | `run-unit: PASS`（56 本が全部 PASS、FAIL 0。test-conn 164417 checks） |
| `BASE=4eae22020 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS` |
| `git diff --check` | 出力なし |

`run-ct.sh` は流していない（ct-conn.c は作らなかった）。この module が秘密から作る値で分岐するのは公開の値（record の型・長さ・alert・flag）だけで、秘密の計算は record・keysched・hs の ct 試験の中。

試験の中身（test-conn.c。相手は tlspeer.h の record mode を memory の pipe につないだ物。pipe は 1 byte ずつ・EAGAIN・EOF・失敗・嘘の件数（要求より多い done、0 の write）を台本で起こし、client の read の要求の大きさを、渡した byte の record の枠（header の残り、次に body の残り）と照らして監査する）:

- handshake 8 通り × 4 つの transport（普通・1 byte ずつ・EAGAIN が多い・小さい piece と EAGAIN）: TLS 1.3 の 3 suite、HRR の後（P-384）、server の CCS・SNI ack・CertificateRequest、TLS 1.2 の 3 suite。client が書く record の並び（`hcd`・`hchd`・`hhch`）、最初の ClientHello だけ legacy version 0x0301、CCS の record が 6 byte、両端が接続済み、保持が無い、読みすぎが 0、wants が正しい。
- application data: 11 の大きさ（1〜40000、2^14 の前後）で write と read（4 つの AEAD、2 つの transport）、record の数と byte 数（手計算の overhead）、peer が復号した内容、buffer 100 byte と大きい buffer の read。SSL_pending の値、buffer からの read が transport を呼ばないこと、read の大きさ 0 は EINVAL、write の大きさ 0 は何も送らない。
- 先読みしない: 3 つの record を pipe に置き、1 回の read が 2 回の transport 呼び出しでちょうど 1 つの record だけを取り、残りの byte が pipe に残る。
- write の再試行: transport が受けない時 EAGAIN（wants = WRITE）、封をした record は 1 つ全部のまま保たれ（taken に数える）、次の record を封しない、再試行で各 record が 1 回だけ送られる、最後の record を封した後の EAGAIN の再試行は送るだけ、taken より小さい size の再試行は EINVAL で何も変えない。
- alert: close_notify（1.3 は level を問わない、1.2 は warning）で先の data を返した後 EPIPE、その後も transport を呼ばない、書く側は使える、shutdown が close_notify を 1 回だけ送り both = 1。1.3 の user_canceled は level を問わず無視、他は全部 fatal（reason 1000 + 説明、返事を送らない）。1.2 の warning は無視・fatal は fatal・level 0 と 3 は illegal_parameter を送る。長さ 0・1・3 の alert は decode_error。handshake の間の alert（平文）、version が分かる前の 0x0301〜0x0303 の close_notify。
- 進まない record の上限: 4 種（user_canceled・空の data・NewSessionTicket・1.2 の warning）× 1・31・32・33・34・100 個（32 までが通り 33 個目が unexpected_message）、data で数えが戻る、handshake の前の CCS・alert の 31〜33 個（handshake の message で戻る。server の CCS の option と TLS 1.2 の CCS の前後で）。
- key change の境: ServerHello と EncryptedExtensions が 1 つの record、KeyUpdate と次の message が 1 つの record は unexpected_message（平文と protected の alert）、2 つの record なら通る。record の途中に別の kind の record（data・alert・CCS・空の data）は 1.3・1.2 とも unexpected_message、message が 2 つの record に分かれるのは通る、handshake と alert の空の fragment は decode_error。
- 壊れた record 22 通り（type・version・長さの過大と過小・tag・protected でない alert・CCS の長さ・protected の CCS・手で作った record の inner type・padding・inner plaintext の過大）と、handshake の間の raw な byte 24 通り（record の型・version・長さ・alert・CCS・record の途中・message の長さの上限 102404 と 102405・ちょうど上限の message を 7 つと 8 つの record で）。それぞれ error・reason・送った alert（byte 単位）を確かめ、失敗の後は全 operation が EPROTO・何も送らない・鍵と secret と buffer が消えている。
- EOF: handshake の間は EPIPE（失敗にならない）、接続後は ECONNRESET（SSLC_CONN_IGNORE_EOF では EPIPE と close_notify 受信扱い、both = 1）、buffer の data が先、record の途中の EOF、EOF の後も write と shutdown が使える。transport の失敗（EIO と os_error、alert を送らない）、嘘の件数・0 の write は EIO。
- shutdown: handshake の前は EPROTO で何も変えない、1 回だけ、EAGAIN の後の再試行、止まっている write を先に送る、peer の close_notify の後の both。init と free（NULL・複写・flag の mask・汚れた memory・2 回の free・途中の message の free）、start の失敗（乱数・範囲外の configuration）、sequence の wrap（write と read）、先に write・read を呼ぶ（handshake が先に走る）、KeyUpdate（要求あり・なし、3 回続けて、双方向の data、答えが transport に止められる時の EAGAIN）、record を 1・2・3・7・100・1000 byte に切った handshake と data、手で作った server（key schedule だけ tlspeer、record は RFC 8446 5.2 から自前で封をする）の中の handshake の protected な CCS・存在しない inner type。
- handshake の失敗の写し: Finished（decrypt_error）、信頼されない root（unknown_ca）、期限切れ（45）・host の不一致（42）・callback の拒否（46）、session id（illegal_parameter）、1.2 の renegotiation_info 欠如、申し出ていない拡張（110）、TLS 1.3 だけの client と 1.2 の server（70）、壊れた EncryptedExtensions（50）、HRR の後の乱数の失敗（internal_error）。

わざと壊す確かめ（`build/tmp/p031-mut/`、`mut.py` が sslc-conn.c の写しを 1 か所ずつ書き換え、他の module は 1 度 build した object、ASan・UBSan 付き、16 並列、150 秒で kill。元の file は変えていない）: 216 通り（読む大きさ、上限の `>` と `>=`、数えの reset、key change の境の条件、alert の level と版の分岐、reason と alert の取り違え、flag の設定、EAGAIN と再試行、flush の位置、EOF の分岐、消去と解放、表の項目など）。最初の 219 通り中 44 通りが生き残った（試験を足す前）。表の 3 項目（hs が出さない alert）を source から外し、試験を足して（CCS option の handshake、手で作った server、ちょうど上限の message の重なり、乱数の失敗、証明書の失敗の alert、先に write・read、2 つの message の間の出力、失敗の後の secret の確認 ほか）、最後は 216 通りのうち 202 通りが検出された（190 通りが試験の失敗、9 通りが sanitizer の中断、2 通りが hang で timeout、1 通りは追加した試験で検出）。生き残り 14 通りの内訳:

- 等価・到達できない守り（11 通り）: `plain_size` を SET_READ_KEYS の境から外す（handshake の record の処理の間は 0 しか無い）、ENOBUFS の守り 2 通り（message の buffer の room の確かめ、SEND の範囲の確かめ。呼び出す側の不変条件が先に止める）、CCS を平文の direction で封する（hs の action の順では write 鍵が入る前に出るので同じ byte。順が変わった時のための守り）、DONE で empty を戻す（その前に handshake の message で戻っている）、action の失敗の後の loop を止める（失敗後に出す byte が無い）、action の wipe（stack の消去で観測できない）、buffer の後の pointer を NULL にする（plain_size が 0 なら使われない）、shutdown の最初の flush（後の flush が同じ順で送る）、2 回目の失敗の守り（失敗を 2 回呼ぶ経路が無い）、`seal` の room を 100 byte 減らす（out に 100 byte の余裕が要る場面が無い）。
- 試験の道具で作れない（3 通り）: alert 43（unsupported_certificate）と 109（missing_extension）の reason の表の項目（tlspeer が未対応の署名・key_share の無い ServerHello を作れない）、malloc の失敗（ENOMEM）の経路（malloc を失敗させる道具が無い）。

自分で決めた点（header が黙る所）:

1. reason: handshake の失敗は hs の alert から表（SSL_R_ の最も近い物）で引く（unexpected_message→UNEXPECTED_MESSAGE、decode_error→BAD_LENGTH、decrypt_error→BAD_SIGNATURE、protocol_version→UNSUPPORTED_PROTOCOL、証明書の alert 42・43・45・46・48→CERTIFICATE_VERIFY_FAILED、handshake_failure と表に無い alert→SSL_HANDSHAKE_FAILURE、拡張の alert→BAD_EXTENSION、internal_error→0）。record 層と connection 自身の失敗は見つけた所で reason を決める（bad_record_mac→DECRYPTION_FAILED_OR_BAD_RECORD_MAC、長さの超過→PACKET_LENGTH_TOO_LONG、handshake の message の過大→EXCESSIVE_MESSAGE_SIZE、alert・空の fragment の長さ→BAD_LENGTH、record の version→WRONG_VERSION_NUMBER、EOF→UNEXPECTED_EOF_WHILE_READING、受けた fatal alert→1000 + 説明）。ENOMEM・transport・sequence の wrap は reason 0（errno が言う）。p032 が別の写しを望むなら表だけ直せばよい。
2. 戻り値: hs の失敗はその module の error をそのまま返す。record の失敗は record の error（EBADMSG・EMSGSIZE・EOVERFLOW）、connection の決まりの違反は EPROTO。transport の失敗は EIO（失敗にして alert を送らない）。read が 0 の done を返す・write が要求より多く done を返す transport は EIO（os_error 0）。NULL の引数と read の size 0 は EINVAL（失敗にしない）。
3. 失敗の動き: 失敗の時に fatal alert を封し（出力に残る byte の後ろへ、room が無ければ送らない）transport に 1 回だけ渡す（EAGAIN なら送らない）。その後で hs・鍵・input・message の buffer を消す。sslc_hs_start の失敗は ClientHello が出ていないので alert を送らない。
4. flush の位置: handshake の message を hs に渡す前に必ず出力を空にする（1 つの record の中の次の message が答えを出す時に room が足りるように）。read は loop の先頭でも出力を flush し、止まっていれば EAGAIN（wants = WRITE）を返す（KeyUpdate の答えや、前の write の止まった record。OpenSSL の read が WANT_WRITE を返す場合と同じ）。write が止まった record は out に 1 つ全部のまま残り、他の operation（read・shutdown）もそれを先に送る。
5. 再組み立ての buffer: 最大の message（102404）+ 1 つの record の plaintext（16384）の 1 つの malloc（最初の handshake の byte で確保、接続後に空になれば解放）。1 つの record の plaintext は全部 buffer に写し（record の境を message の境と無関係に扱う）、whole な message を取り出す。SET_READ_KEYS の境の確かめは「その message を取り出した後に buffer が空」。
6. EOF: 全ての EOF（record の途中も）を同じに扱う（SSL_OP_IGNORE_UNEXPECTED_EOF の OpenSSL の扱い）。handshake の間は常に EPIPE。EOF は失敗にしない（shutdown で close_notify を送れる）。
7. alert の読み: 版が分からない間（hs.version = 0）と 1.2 は RFC 5246 7.2 の level で、1.3 は RFC 8446 6 で（close_notify は level を問わず閉じ、user_canceled は無視、他は fatal）。1.2 で level が 1・2 以外は illegal_parameter を送る。close_notify を handshake の間に受けたら reason = 1000 に。
8. 進まない record の数え: 警告・user_canceled・CCS・空の data・接続後の handshake の record（1 つの record ごとに 1）。進んだ（nonempty の data、handshake の間の handshake の byte、DONE）時に 0 に戻す。32 個まで通り 33 個目が unexpected_message。
9. record の header の version: hs.version が分かる前は 0x0301〜0x0303、後は 0x0303。header の EPROTO は type（存在しない→unexpected_message）→version（protocol_version）→長さ 0（decode_error）→その他（unexpected_message）の順で alert を決める。
10. TLS 1.3 の protected な record の inner type が change_cipher_spec は unexpected_message（外側が change_cipher_spec の protected でない物だけが compatibility record）。inner の type がその 4 つ以外も unexpected_message。
11. shutdown: 失敗後と handshake の完了前は EPROTO（何も送らず、前者以外は状態を変えない）。close_notify を封した時に CLOSE_SENT を立て、再試行は出力の残りだけを送る。flush が終わるまで both は決まらない（peer の close_notify の受信だけを返し、待たない）。write は CLOSE_SENT の後 EPIPE。
12. CCS の封: compatibility change_cipher_spec は常に鍵の無い direction で封する（write 鍵が入った後でも平文、sequence に触れない）。
13. SSLC_HS_SEND の record 化: 1 つの SEND の bytes を 2^14 ごとの handshake record に分け、first_hello を最初の ClientHello の record だけに渡す。

header の持ち越し: なし。phase.md の食い違い: なし（phase.md の `BASE=b7dfd3952` は依頼の `4eae22020` を使った）。

未実施・残り:

- malloc の失敗（ENOMEM）の経路、alert 43・109 の reason は試験していない（上の「試験の道具で作れない」）。
- 実際の server との相互運用・実機は p033。Windows の aarch64 は check-skeleton の compile と link だけ。
- `run-ct.sh`（上の理由）。

消してよい物（Q1 に頼む）: `build/tmp/p031-mut/`（わざと壊す確かめの script・写し・object・log）、`build/tmp/p031-scratch/`、`build/tmp/ws335-unit.run.*` と `ws335-skeleton.run.*` のうち今の symlink が指さない物。作業中に `rm -f /dev/null` を誤って 1 回打ったが Permission denied で何も消していない。
