<!-- awesome-plan project=zedbsd record=ws336 -->

# WS336 zedBSD の自前の sshd と ssh（OpenSSH と互換の protocol、ed25519 の鍵と最新の鍵の交換 1 つ）

- Master: [plan/master.md](../master.md)（2026-11-01 作成）。Primary Milestone: MG005。
- Status: incomplete（2026-11-01 01:56 新設。2026-11-01 04:54 p001 の設計が cleared、実装の段へ）
- 目標: OpenSSH（`userland/packages/network/openssh`）を置き換える、zedBSD の自前の sshd と ssh を作る。OpenSSH の sshd と自前の sshd の両方に接続でき、
  OpenSSH の ssh から自前の sshd に接続できる。暗号の部品は [WS335](../ws335/ws.md) の libcrypto-compat（OpenSSL 3.5 互換の API）を使う（下の決定 4）。
- 進め方: 標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)）。設計を終えてから実装に入る。WS335 と並べて進め、
  WS335 の暗号の module の口に依る所は待ち合わせる（WS335 の設計の凍結を待ってから、その header を使う実装の Phase を始める）。

## ユーザーの決定

- 2026-11-01 ユーザーから、次の趣旨の指示を受けた。
  1. 互換の sshd と ssh を作る WS を立てる。
  2. 鍵は ed25519 だけ使えればよい。鍵の交換は最新の定番を 1 つ持ち、OpenSSH の sshd と自前の sshd の両方に接続できればよい。
  3. WS335 での決定（暗号の部品の口を WS335 が持つ、OpenSSH の libcrypto の互換は作らない）をこの WS に反映する。
  4. （2026-11-01 追記）暗号は WS335 に足した libcrypto-compat（libbrowser と自前の ssh・sshd が使う部分だけの OpenSSL 互換の API）を使う。
  5. WS335 と並行して進め、依存の所では待ち合わせる。標準廉価実装手順で実装する。P5 で始める。
- 2026-11-01 ユーザーから、Q1〜Q9 はまず推しの案でよい、後で実装できるように、今は作らない物の TODO の一覧を WS に入れる、という趣旨の答えを受けた。

## Resume point

- 2026-11-01 Q1〜Q9 は推しの案に決まった（[user-questions.md](user-questions.md)）。設計・header は変えない（基点は d4ab182d2 のまま）。今は作らない物は下の「未実装（後で実装する物）」。
- p001（設計）を P5 が q1791 で進めている。成果: [design.md](design.md)、header と骨格 `userland/base/ssh/`、試験の道具 `plan/ws336/tests/`、[design-review.md](design-review.md)、[user-questions.md](user-questions.md)。
- header の基点: `d4ab182d2`（p001 の design-review を反映した header。実装の Phase の `BASE=d4ab182d2 sh plan/ws336/tests/check-skeleton.sh`。header を変える時は Q1 が新しい基点をここに書く）
- 「WS335 header」= WS335 の libcrypto-compat の header（`include/libc/compat/crypto/*.h`）の凍結が main に入った後に始める。「WS335 実装」= libcrypto-compat の該当の実装が main に入り zedBSD の build にある後。他の Phase は host の OpenSSL 3.5 で試すので WS335 を待たない（design.md §7・§12）。
- 2026-11-01 04:4x 確かめ: WS335 の header の凍結（基点 60760d2a0）は main に入っている（`git merge-base --is-ancestor`）。`BASE=d4ab182d2 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`（4 番目の ssh-crypto.c を libcrypto-compat の header だけで compile する照合を含む）。p003 の「WS335 header」の待ちは解けた。
- 残る WS335 の待ち（p021〜p024 の「WS335 実装」）の中身（2026-11-01 WS335 の design.md §4.3・phase.md で確かめ）: host での libcrypto-compat の確かめ（crypto-contract・単体・相互運用）は WS335 の p023・p024（とその前の p002・p003・p006・p008・p009・p013・p014・p015・p021）の後。**zedBSD の build（`libcrypto-compat.so` の規則と package の登録）は WS335 の p033（統合）にあり、p033 は TLS の全部（p025〜p032）の後**。WS336 の p021〜p024 はそのままでは WS335 の p033 を待つ。案（Q1・P6 が決める、WS335 の計画の事）: A WS335 の p033 を待つ。B（推し）WS335 が libcrypto-compat の .so の規則と登録だけを p024 の後の小さな Phase に分け、WS336 の p021 はそれと p023・p024 だけを待つ（libssl-compat の .so と TLS の統合は p033 に残す）。

## Phase

設計は [design.md](design.md)。並べて起動できる組は design.md §12。

| ID | 目的 | 状態 | 依存 | WS335 の待ち |
| --- | --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計（標準廉価実装手順 §4）: SSH の protocol の範囲（RFC 4251〜4254・8709 ほか）、鍵の交換の 1 つ、暗号の組、sshd と ssh の機能の範囲、module・header・骨格・試験の道具、実装の Phase の表、WS335 との待ち合わせ、design-reviewer、問い | cleared（2026-11-01 04:54 Q1、main 451dd0f9e。基点 d4ab182d2、TODO の一覧 T1〜T31） | — | — |
| [p002](phase002/phase.md) | ssh-common・ssh-buf | cleared（2026-11-01 05:10 Q1、main 87c8caf2d。ssh-common・ssh-buf 実装、run-unit common buf（2316・323 checks）PASS（Q1 再実行で確認）、ASan・UBSan、c89） | p001 | — |
| [p003](phase003/phase.md) | ssh-crypto（libcrypto-compat の呼び出し） | planned | p002 | WS335 header（解けた: 基点 60760d2a0 が main にある） |
| [p004](phase004/phase.md) | ssh-cipher・ssh-packet | planned | p003 | p003 経由 |
| [p005](phase005/phase.md) | ssh-key | planned | p003 | p003 経由 |
| [p006](phase006/phase.md) | ssh-hostfile | planned | p005 | p003 経由 |
| [p007](phase007/phase.md) | ssh-kex | planned | p004 | p003 経由 |
| [p008](phase008/phase.md) | ssh-transport（Opus） | planned | p005・p007 | p003 経由 |
| [p009](phase009/phase.md) | ssh-auth | planned | p005 | p003 経由 |
| [p010](phase010/phase.md) | ssh-ttymodes・ssh-channel | in-progress（2026-11-01 05:10 q1814、Sonnet） | p002 | — |
| [p011](phase011/phase.md) | ssh-config | planned | p006 | p003 経由 |
| [p012](phase012/phase.md) | ssh-privsep（frame） | in-progress（実装済み・試験 PASS 2026-11-01 06:16、cleared は Q1 が決める、Sonnet） | p002 | — |
| [p013](phase013/phase.md) | ssh-sftp（Q3 の A: sftp-server のため） | in-progress（2026-11-01 05:10 q1816、Sonnet） | p002 | — |
| [p014](phase014/phase.md) | ssh-keygen の program | planned | p006 | p003 経由 |
| [p015](phase015/phase.md) | ssh の program・host の相互運用 A（Opus） | planned | p006・p008・p009・p010・p011 | p003 経由 |
| [p016](phase016/phase.md) | sshd の monitor と login の確かめ（Opus） | planned | p009・p011・p012・p014 | p003 経由 |
| [p017](phase017/phase.md) | sshd の pre-auth と session の process（Opus） | planned | p008・p010・p016 | p003 経由 |
| [p018](phase018/phase.md) | sshd の listener・相互運用 B（Opus） | planned | p015・p017 | p003 経由 |
| [p019](phase019/phase.md) | sftp-server の program（Q3 の A: 旧の scp の受け手は作らない） | planned | p013・p018 | — |
| [p020](phase020/phase.md) | 敵対的な入力と privsep の見直し（Opus） | planned | p018・p019 | — |
| [p021](phase021/phase.md) | package と amd64 の build・libcrypto-compat での確かめ（Opus） | planned | p020 | **WS335 実装** |
| [p022](phase022/phase.md) | QEMU の相互運用（Opus） | planned | p021 | **WS335 実装** |
| [p023](phase023/phase.md) | 他の arch の build | planned | p021 | **WS335 実装** |
| [p024](phase024/phase.md) | OpenSSH からの入れ替え（Opus、Q6 の A） | planned | p022・p023 | **WS335 実装** |
| [p025](phase025/phase.md) | 規約の全文の見直し（conformance-reviewer） | planned | p024 | — |

## 未実装（後で実装する物）

2026-11-01 のユーザーの決定（Q1〜Q9 は推しの案、今は作らない物の一覧を WS に置く）による。この WS の範囲の外で、ここに載せる事は実行の許可ではない（作る時は新しい WS か Queue で、ユーザーが決める）。
載せたのは design.md で範囲の外とした物と、OpenSSH 10.5p1 の標準の機能（`build/sources/specs/ssh/` の ssh.1・sshd_config.5・ssh_config.5・ssh-keygen.1・PROTOCOL*）だけ。
「手掛かり」は今の header（基点 d4ab182d2）に既にある物と、足す時に変える所。大きさは実装の Phase の数の目安（標準廉価実装手順の 1 Phase = header 1〜3 本）。「WS335」は libcrypto-compat に公開の口を足す必要がある物（WS335 の範囲の判断が要る）。

| # | 項目 | 何か | 今は作らない理由 | 依存 | 手掛かり（今ある物 → 変える所） | 大きさ |
| --- | --- | --- | --- | --- | --- | --- |
| T1 | port の転送 `-L`・`-R`・`-W` | `direct-tcpip`・`forwarded-tcpip` の channel、`tcpip-forward`・`cancel-tcpip-forward` の global request（RFC 4254 §7）。ssh の `-L`・`-R`・`-W`、LocalForward・RemoteForward、sshd の AllowTcpForwarding・PermitOpen・PermitListen・GatewayPorts | Q2 の A。今の道具で `-L` の相手は Linux の host だけ。sshd の攻撃の面が広がる | 無し（WS335 を待たない） | ssh-channel.h は `session` 以外の open を ADMINISTRATIVELY_PROHIBITED で断り、global request は全部 FAILURE（D16）。layer は I/O をしない（D9）ので TCP の connect・listen は program の側に足せる。session の process は user の uid なので、connect は user の権限で済み monitor の要求は増えない → ssh-channel.h に channel の種類と direct-tcpip・forwarded-tcpip の put・parse、global request の受け、ssh-config.h の keyword | 3（channel 1、ssh 1、sshd 1） |
| T2 | 動的な転送 `-D` | ssh が SOCKS4・SOCKS5（RFC 1928）の proxy を開き、接続ごとに direct-tcpip を開く。DynamicForward | Q2 の A | T1 | 新しい module（SOCKS の parse・reply）、ssh-main.c の listener | 1 |
| T3 | ssh-agent・ssh-add と agent の転送 `-A` | 秘密鍵を持つ常駐の program（unix socket、`SSH_AUTH_SOCK`）と、`auth-agent@openssh.com` の channel・`auth-agent-req@openssh.com` の request（PROTOCOL.agent、draft-ietf-sshm-ssh-agent）。転送した agent を守る `publickey-hostbound-v00@openssh.com`（PROTOCOL §3.1） | Q4 の A。今の道具は使わない。秘密鍵の常駐で攻撃の面が広い | T1 の channel の種類。T7 と組むと価値が出る | 仕様（draft-ietf-sshm-ssh-agent-16・PROTOCOL.agent）は specs にある。ssh-key は秘密鍵の符号に agent draft §3.2.3 の形を既に使う。D16 で auth-agent-req を受けて捨てる → ssh-auth.h に「署名を外から受ける」publickey の put（今の `ssh_auth_put_publickey_signed` は seed を持つ `struct ssh_key` を要る。署名される data は `ssh_auth_put_signed_data` で作れる）、新しい module ssh-agent、program ssh-agent・ssh-add | 3 |
| T4 | zedBSD の上の scp と sftp の client | zedBSD から外へ送る `scp`（OpenSSH 9.0 以降の既定と同じ SFTP の protocol）と `sftp`（対話と `-b`） | Q3 の A。今の道具は host の scp・sftp から guest へ送るだけ | 無し | ssh-sftp.h は server の側（request の parse・reply の put）だけ → 同じ header に client の側（request の put・reply の parse）、ssh を子として起こす（`ssh -s … sftp`、ssh に `-s` を足す） | 2〜3 |
| T5 | 旧の SCP の protocol（`scp -O`、`-t`・`-f`） | 相手に `scp` の program が要る古い方式の受け手と送り手 | Q3 の A（`-O` を使う道具 4 本は p024 で `-O` を外す） | T4（送り手の側） | design.md §4.2 の scp の行と p019 が置き場だった → protocol は RFC に無く scp.1 だけなので、design に protocol の節を足す | 1 |
| T6 | sftp の拡張 | `posix-rename@openssh.com`・`statvfs@openssh.com`・`fsync@openssh.com`・`hardlink@openssh.com`・`limits@openssh.com` ・`copy-data` など（PROTOCOL §4.2〜4.12） | ssh-sftp.h は vendor の拡張を announce しない（v3 だけで host の scp・sftp が動く） | 無し | `ssh_sftp_put_version` が拡張の pair を出さない → VERSION に pair、EXTENDED の request の parse | 1 |
| T7 | passphrase 付きの秘密鍵の file | openssh-key-v1 の cipher `aes256-ctr` と KDF `bcrypt`（PROTOCOL.key）。ssh-keygen の `-N <pass>`・`-p`、ssh の passphrase の問い | Q9 の A・D11。今の道具の鍵は全部 passphrase 無し | **WS335**: `EVP_aes_256_ctr`（AES は WS335 p004 に内部の部品があるが公開の EVP の範囲の外）。bcrypt_pbkdf（Blowfish と SHA-512、OpenSSL の API に無い）の置き場は別の判断 | `ssh_key_parse_private` が暗号化した鍵を ENOTSUP で断る（ssh-key.h）→ ssh-key.h に passphrase の引数、ssh-crypto に AES-CTR・SHA-512・bcrypt_pbkdf、`/dev/tty` の問い | 2（WS335 の追加を除く） |
| T8 | 鍵の種類 `rsa-sha2-256`・`rsa-sha2-512` | RSA の host key と user key | D2・ユーザーの決定（ed25519 だけ） | **WS335**: RSA の署名と鍵の生成（今の WS335 は確かめだけ、p012）。user key の rsa-sha2 には T14（server-sig-algs）が要る | `struct ssh_key` は ed25519 の固定の field、交渉は 1 つの名前（D10）→ `struct ssh_key` を種類つきに（key・hostfile・auth・kex・privsep の header に及ぶ）、host key の名前の一覧の交渉 | 3〜4 |
| T9 | 鍵の種類 `ecdsa-sha2-nistp256`・`-nistp384`・`-nistp521` | ECDSA の host key と user key（RFC 5656） | D2 | T8 の種類つきの鍵。**WS335**: ECDSA の署名（今は確かめだけ、p011）、P-521（WS335 に無い） | T8 と同じ | T8 の後 1〜2 |
| T10 | FIDO の鍵 `sk-ssh-ed25519@openssh.com`・`sk-ecdsa-sha2-nistp256@openssh.com` | security key に秘密を置く鍵（ssh-keygen `-t ed25519-sk`・`-K`） | D2 | T8 の種類つきの鍵、FIDO2 の認証器との通信（zedBSD の側の口は未調査）。仕様 PROTOCOL.u2f は specs に無い（取得が要る） | ssh-key は他の種類を ENOTSUP で「この鍵ではない」とする → T8 と同じ | 2〜3 |
| T11 | 鍵の交換 `curve25519-sha256`（と `sntrup761x25519-sha512`） | Q7 の B。OpenSSH 9.9 より前の相手（例 Debian 12 の 9.2）につなぐため。sntrup761 は OpenSSH の私的な物 | Q7 の A・D1（最新の定番を 1 つ） | curve25519 は無し（X25519・SHA-256 は既に使う）。sntrup761 は **WS335** に無い | ssh-kex.h は `SSH_KEX_NAME` 1 つと hybrid の message → 交渉の一覧（D10 を改める）、ECDH の message（RFC 8731・RFC 5656 §4）、ssh-transport の分岐 | 1〜2 |
| T12 | cipher `aes256-gcm@openssh.com`・`aes128-gcm@openssh.com` | AES-GCM の AEAD（RFC 5647 の OpenSSH の形） | Q8 の A・D3 | **WS335**: 公開の EVP の AES-GCM（内部 p005 はあるが EVP は範囲の外、design.md §7） | ssh-cipher.h は `SSH_CIPHER_NAME` と `struct ssh_chachapoly` だけ → cipher の種類の抽象（ssh-packet が使う形）、kex の cipher の一覧 | 2 |
| T13 | AEAD でない cipher と MAC | `aes128-ctr`・`aes256-ctr`（RFC 4344）と `hmac-sha2-256`・`hmac-sha2-512`（RFC 6668）とその `-etm@openssh.com`（PROTOCOL §1.5）、`umac-64@openssh.com` など | D3（AEAD だけ、MAC の一覧は空）。相手を増やすだけで推しは低い | **WS335**: `EVP_aes_*_ctr`、`EVP_MAC` の "HMAC" | ssh-packet は AEAD の padding の規則だけ → packet に MAC の段（encrypt-then-MAC と MAC-then-encrypt）、kex の MAC の交渉 | 2 |
| T14 | EXT_INFO（RFC 8308）と `ping@openssh.com` | `server-sig-algs` などの拡張の知らせ | D5（ed25519 だけなので要らない。受けたら捨てる） | T8 で要る | ssh-transport が EXT_INFO を受けて捨てる → put・parse | 1（T8 と組） |
| T15 | 圧縮 `zlib@openssh.com` | 認証の後の packet の圧縮（`-C`、Compression） | design.md §4.1 の範囲の外。今の道具は使わない | deflate・inflate（base の program は外部の library を使わないので自前が要る。zedBSD の base の有無は未調査） | ssh-packet に段が無い → 圧縮の段、kex の圧縮の一覧 | 1〜2 |
| T16 | X11 の転送 `-X`・`-Y` | `x11-req` の request と `x11` の channel、xauth | §4.1 の範囲の外。zedBSD の desktop（Keiland）は Wayland | T1 の channel の種類 | D16 で `x11-req` を受けて捨てる → x11-req の parse、session の側の X の socket | 1〜2 |
| T17 | ProxyJump `-J`・ProxyCommand | 踏み台の host を経由する。command の stdin・stdout を transport にする | §4.1 の範囲の外 | `-J` は T1 の `-W`（client の側の direct-tcpip） | layer は I/O をしない（D9）ので、bytes の出入りを socket から pipe に替えるのは ssh-main.c だけ → ssh-config.h に keyword | 1 |
| T18 | 多重化（ControlMaster・ControlPath・ControlPersist、`-M`・`-S`・`-O`） | 自前の ssh が 1 本の接続を unix socket で他の ssh と共有する（PROTOCOL.mux） | 今の道具は host の OpenSSH の ssh で使い、自前の ssh では使わない。sshd の側は 1 接続に最大 10 session を既に持つ（§4.1） | 無し | PROTOCOL.mux は specs にある。ssh-channel は 1 接続に複数の session を持つ → 新しい module（mux の message）、ssh-main.c の listener | 2 |
| T19 | 証明書 | `*-cert-v01@openssh.com` の鍵、ssh-keygen `-s`・`-I`・`-n`、known_hosts の `@cert-authority`、sshd の TrustedUserCAKeys・HostCertificate・RevokedKeys（KRL、ssh-keygen `-k`） | §4.1 の範囲の外 | T8 の種類つきの鍵（証明書は鍵の種類の 1 つ）。仕様 PROTOCOL.certkeys・PROTOCOL.krl は specs に無い（取得が要る） | ssh-hostfile は `@cert-authority` の行を飛ばす（`@revoked` は守る）→ key・hostfile の header、monitor の確かめ | 3 |
| T20 | known_hosts の hash | HashKnownHosts、`|1|` の行、ssh-keygen `-H`・`-F` | D12（HMAC-SHA1 が libcrypto-compat の範囲の外） | **WS335**: `EVP_sha1` と `EVP_MAC` の "HMAC" | ssh-hostfile は `|1|` の行を飛ばして skipped に数える → hash の照合、ssh-crypto に HMAC-SHA1 | 1 |
| T21 | ssh の他の option と ssh_config の keyword | option: `-4`・`-6`・`-b`・`-B`・`-c`・`-m`・`-E`・`-e`・`-f`・`-G`・`-g`・`-K`・`-k`・`-Q`・`-s`・`-V`・`-w`・`-y`・`-P`（転送・agent・X11・proxy・多重化は T1〜T3・T16〜T18）。keyword: Match・Include・SendEnv・SetEnv・IdentitiesOnly・AddKeysToAgent・UpdateHostKeys（`hostkeys-00@openssh.com`）・CanonicalizeHostname・LocalCommand・KnownHostsCommand・EscapeChar・VisualHostKey など | §4.2（今の道具が使う物だけ） | 一部は上の行 | ssh-config.h は知らない keyword を行の番号つきの誤りにし、害の無い物を「受けて無視」の一覧に置く → keyword ごとに `struct ssh_config` と一覧 | 数個で 1 |
| T22 | ssh-keygen の他の機能 | `-t rsa`・`-t ecdsa`・`-p`・`-s`・`-H`・`-F`・`-r`（SSHFP）・`-Y`（SSHSIG の署名と確かめ）・`-k`（KRL）・`-e`・`-i`（RFC 4716・PEM の出し入れ）・`-K`・`-M` | §4.2（ed25519 と `-N ''` だけ、D11） | T7・T8・T10・T19・T20 | §4.2 の一覧に無い option は断る → 機能ごと | 機能ごと 1 |
| T23 | 認証 `keyboard-interactive` | RFC 4256 の INFO_REQUEST・INFO_RESPONSE（OTP など） | §4.1（PAM が無く password で足りる） | 無し | ssh-auth.h の `SSH_AUTH_OTHER` で失敗にする → INFO の put・parse、monitor の要求の種類 | 1〜2 |
| T24 | 認証 GSSAPI と hostbased | GSSAPI（RFC 4462、Kerberos）・`hostbased`（RFC 4252 §9） | §4.1。zedBSD に Kerberos が無い。hostbased は host の間の信頼の設定が要る | GSSAPI は Kerberos の library（別の WS の規模） | 同上 | GSSAPI は大、hostbased 1〜2 |
| T25 | password の変更 | PASSWD_CHANGEREQ（RFC 4252 §8）で期限切れの password を login の時に変える | §4.1（変更の要求は断る） | 無し | `struct ssh_auth_request` の `change` の field で受けて断る → monitor に変更の要求 | 1 |
| T26 | sshd の Match の block | 接続（User・Group・Address・Host）ごとに設定を替える | ssh-config.h（Match の行は誤り） | 無し | `sshd_config_parse` が Match を EINVAL → 接続ごとに設定を評価し直す形、monitor と pre-auth の間の CONFIG | 2 |
| T27 | ChrootDirectory・ForceCommand・`internal-sftp` | user を directory に閉じ込める、command を固定する | 今の image で使わない | 多くは T26 の中で使う | session の process は user の uid で起こす（D8）→ chroot は ids を替える前なので、monitor が session を起こす順に手が要る | 1〜2 |
| T28 | sshd の他の keyword | AllowUsers・DenyUsers・AllowGroups・DenyGroups・AuthenticationMethods・PermitTTY・Banner・AcceptEnv（今は LANG・LC_*・TERM に固定）・PermitUserEnvironment・AuthorizedKeysCommand・PerSourcePenalties・MaxStartups の rate:full・ListenAddress の複数・AddressFamily any・LogLevel・SyslogFacility・`sshd -T` | §4.2・ssh-config.h の一覧に無い | RevokedKeys は T19 | `sshd_config_parse` の一覧に足す。許す・断るの判定は monitor の側（ssh-privsep） | 数個で 1 |
| T29 | authorized_keys の options | `command=`・`from=`・`restrict`・`no-pty`・`permitopen=`・`environment=` など | D13（制限つきの鍵を制限なしで受け入れない） | 一部は T1・T26 | ssh-hostfile は options つきの行を飛ばす → options の parse と結果の struct、monitor から session へ制限を渡す | 2 |
| T30 | 権限の分離の C（pre-auth を `sandbox_spawn` で） | 認証の前の process の system call を絞る（WS168 の sandbox） | Q5 の B | **WS335**: zedBSD 向けの libcrypto-compat の static の library（今の WS335 の設計は zedBSD で `.so`、static の `.a` は Windows 向け。WS335 D1） | pre-auth は sshd 自身を `-Zpreauth` で execve して始める（D8）→ その起こし方を static の image の `sandbox_spawn` に替える。pre-auth が要る system call と src/kern/sandbox.c の集合の照合 | 2 |
| T31 | 他の channel と request | `tun@openssh.com`（`-w`）、streamlocal（unix socket の転送、`direct-streamlocal@openssh.com` など）、`hostkeys-00@openssh.com`（host key の入れ替えの知らせ） | §4.1 の範囲の外 | T1 の channel の種類。hostkeys-00 は T21 の UpdateHostKeys | D16 で受けて捨てる | 各 1 |
