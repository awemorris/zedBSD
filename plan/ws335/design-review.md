<!-- awesome-plan project=zedbsd record=ws335-design-review -->

# WS335 p001 の design-reviewer の記録（2026-11-01）

- review の対象: P6 の worktree `agent/p6-ws335` の 093dd4b3a（design.md・header・骨格・phase.md・試験の道具）。design-reviewer が 1 本、敵対的に読んだ（worktree は変えていない）。
- reviewer が自分で確かめた事: header の OpenSSL の定数（SSL_R_*・X509_V_*・ERR_LIB_*/ERR_R_*・flag の bit・SSL_CTRL_*・SSL_OP_*・SSL_ERROR_*・EVP_PKEY の NID・OSSL_PARAM の配置）は 3.5.8 と一致。FIPS 203・197-upd1・186-5 の節・algorithm・式の番号。header の単独の compile（cc・clang、-std=c89 -Wpedantic）。骨格の .c が全部どれか 1 つの Phase に入る。primitive の header に TLS の型が無い。
- reviewer が流していない物: check-skeleton.sh・run-unit.sh・run-ct.sh（worktree の build/ に書くため）。P6 が反映の後に流した（下）。
- 指摘 27 件（高 3・中 9・低 15）。全部を反映した。見送りは無い。27 は推定の指摘で、p038 で件数を見てから決める形にした。

| # | 重さ | 指摘（要点） | 扱い |
| --- | --- | --- | --- |
| 1 | 高 | RFC 8448 の full の trace は handshake で一致しえない（ClientHello の拡張と session id が違う、server の鍵が RSA-1024） | 反映: RFC 8448 は p026〜p028 だけに。handshake の試験の相手は新しい p037 の試験の道具 `tlspeer.h`（scripted な server と試験の側の ECDSA の署名器）。design D22、p029〜p031・p033 を直した。乱数の引く順を sslc-hs.h に書いた |
| 2 | 高 | 先読みで libbrowser が待ちに入る（SSL_pending==0 で poll） | 反映: sslc-conn.h に「今の record の残りだけを求める」、design D21、p031 に試験 |
| 3 | 高 | cryc_ed25519_sign が呼び手の公開鍵を取る（違う A で 2 回署名すると秘密の scalar が漏れる） | 反映: 引数から公開鍵を外し、seed から内部で導く（cryc-ed25519.h、骨格も） |
| 4 | 中 | TLS 1.3 の key change は record の境に（同じ record に残った message） | 反映: sslc-conn.h（1.3 と 1.2 の CCS の両方）、p031 に試験 |
| 5 | 中 | handshake の確かめの抜け（1.3 の ServerHello の許されない拡張、HRR の後の version と group、1.2 の session id の echo、1.3 の Certificate の context、EE の server_name、CCS を送る時、PSS の salt、1.2 の ECDSA は曲線を縛らない、1.2 の HelloRequest、legacy_version の alert） | 反映: sslc-hs.h の確かめの一覧に全部 |
| 6 | 中 | record の最小の長さ、長さ 0 の規則の場所、2^14 の上限の意味 | 反映: sslc-record.h（最小 17・24・16 は EBADMSG、長さ 0 は復号後、TLSInnerPlaintext は padding 込みで 2^14+1） |
| 7 | 中 | EVP_Cipher の戻り値（OpenSSL 3.5 は長さと -1） | 反映: evp.h |
| 8 | 中 | writer の深さ 4 が足りない | 反映: 6 に、comment を直した |
| 9 | 中 | vecfile.h の 1 byte の heap の溢れ | 反映: NUL の前にも伸ばす |
| 10 | 中 | 出典が file の先頭にある（仕様の版の文、論文、ctgrind、Layout referenced、ssh・net/tls.c の言及） | 反映: 版の文は include の後の block に移した。Montgomery の論文は `cryc_bn_mont_mul` の前の「Procedure referenced:」、ctgrind は ct.h の macro の block の「Procedure referenced:」、types.h は「Values referenced:」に。先の利用者の名前（ssh・libbrowser）は機能の言い方に替えた。header の先頭の「何の規格を実装するか」の 1 文（例 RFC 8017 の RSA）は機能の説明として残した（libz-compat の zlib.h の前例、RFC 1950） |
| 11 | 中 | SSL_write の再試行で長さが違う、長さ 0 と負 | 反映: ssl.h（BAD_WRITE_RETRY・BAD_LENGTH）、定数を足した |
| 12 | 中 | handshake・接続の試験の独立の相手が無い | 反映: p037（1 と同じ） |
| 13 | 低 | 接続の前の SSL_shutdown・SSL_free、handshake の途中の shutdown、fatal の後の close_notify | 反映: ssl.h・sslc-api.h・sslc-conn.h（SHUTDOWN_WHILE_IN_INIT、PROTOCOL_IS_SHUTDOWN） |
| 14 | 低 | SSL_set1_host の振る舞いが OpenSSL 3.5 と違う | 反映: ssl.h（host と address を消してから、IP の文字列は address に） |
| 15 | 低 | d2i_X509 の誤りの道と後ろの bytes | 反映: x509.h（*out を放して NULL、後ろの bytes は読まない） |
| 16 | 低 | 出典の番号の誤り（6.4.2 step、6.1.4 (g)、OpenSSL の counter の繰り上がり） | 反映: step 6・step 3、6.1.3 (b)(c)、evp.h の counter の注 |
| 17 | 低 | 点の decode の確かめの順で ENOTSUP が出ない | 反映: cryc-ecp.h に順を書いた |
| 18 | 低 | 負の serial を読む道が無い | 反映: cryc-x509.h（cryc_der_expect で読む） |
| 19 | 低 | sslc-tls.h の範囲に ALPN、SSL_CIPHER_get_name の言及 | 反映: 消した |
| 20 | 低 | name constraints の仕事の上限が無い | 反映: CRYC_NAME_MAX_CHECKS（4096、越えたら ENOTSUP） |
| 21 | 低 | KeyUpdate の flood、1.3 の user_canceled、NST の lifetime | 反映: sslc-conn.h（KeyUpdate と user_canceled を進まない record に数える、user_canceled は closure）、sslc-msg.h（lifetime を見ない） |
| 22 | 低 | lock の順が無い | 反映: cryc-chain.h・sslc-api.h（ctx の lock → store の lock、file の読みと callback は lock の外） |
| 23 | 低 | 依存の直列が多すぎ、p020 が大きすぎ、phase.md の §4 と §5 の矛盾 | 反映: p027 は p025・p002、p028 は p025 に。limbo の全件は新しい p038（Opus）に分けた。§4 の文を「内部の事は決める、interface の不備は止める」に |
| 24 | 低 | check-skeleton の BASE が試験の道具と消した header を見ない、run-unit の名前の誤り、ct-*.c が ASan で走らない | 反映: BASE は header と試験の道具を git の pathspec で（足した・消した物も）、fetch.sh の名前、run-unit が `ct-<名前>` も流す |
| 25 | 低 | SIGPIPE、Windows の CA file の module の言い方 | 反映: MSG_NOSIGNAL のある system では send(MSG_NOSIGNAL)、ENOTSOCK で write、zedBSD は write。CA file は libcrypto-compat の DLL の隣 |
| 26 | 低 | SSL_get_error の振る舞い | 反映: ssl.h（呼んだ時に決めた分類を返し、SYSCALL では errno を戻す） |
| 27 | 低（推定） | trust anchor の pathLen と basicConstraints を見ない事が limbo と違う | 反映の形: p038 が件数を見てから Q1 に案を返す（phase038 に書いた） |

## 反映の後の確かめ（2026-11-01 P6）

結果は下の commit の記録（ws.md の Resume point）にも書く。
