<!-- awesome-plan project=zedbsd record=ws336-design -->

# WS336 自前の sshd・ssh の設計（p001、標準廉価実装手順 §4）

- 作成: 2026-11-01（P5、q1791-i01）。基点: main e426d02a7（2026-11-01 01:5x に main 73976f01a を取り込み）。
- 範囲: [phase001/phase.md](phase001/phase.md) の 1〜7。code は header・.c の骨格・試験の道具だけ。
- 置き場所（推し、Q1 の確認が要る）: source は `userland/base/ssh/`、試験の道具は `plan/ws336/tests/`（WS の完了の時に今後も使う物を `plan/tools/ssh/` へ移す）。

## 1. 出典（仕様の置き場所と SHA-256）

仕様は git に入れない。置き場所は `/home/awe/zedBSD-claude1/build/sources/specs/ssh/`（main の checkout の共有の `build/sources/specs/` の下。worktree からは `build/sources/specs/ssh/` の symlink で見える）。
全部の SHA-256 は同じ directory の `SHA256SUMS`（`sha256sum -c SHA256SUMS` で照合）。text の仕様は `grep -n` で節を引ける。

| 文書 | file | SHA-256（先頭 16 桁） | 使う所 |
| --- | --- | --- | --- |
| RFC 4250 SSH Assigned Numbers | rfc4250.txt | eb3a9d7c3e1fae53 | message 番号・disconnect の理由・名前 |
| RFC 4251 Architecture | rfc4251.txt | 23a5052796d29edc | §5 data の型、§6 名前 |
| RFC 4252 Authentication | rfc4252.txt | a36b882cf77b1073 | publickey §7、password §8、none §5.2 |
| RFC 4253 Transport | rfc4253.txt | b471173305154f6c | §4.2 識別の行、§6 packet、§7 KEX、§7.2 鍵の導出、§9 re-key、§11 汎用の message |
| RFC 4254 Connection | rfc4254.txt | bfc727e9350067eb | channel・window・request、§8 端末の mode |
| RFC 8308 Extension Negotiation | rfc8308.txt | c7d36121bcd0f242 | EXT_INFO（使わない根拠） |
| RFC 8709 Ed25519 for SSH | rfc8709.txt | ea4aa3a8feec6bd5 | key と署名の blob |
| RFC 8731 Curve25519 KEX | rfc8731.txt | 26dcf1e8481d83e0 | K_CL の扱い（hybrid が参照） |
| RFC 9142 KEX method updates | rfc9142.txt | 68226c742986b835 | 選ばなかった案の根拠 |
| RFC 8439 ChaCha20・Poly1305 | rfc8439.txt | 25bef70fbf7a07ff | 試験の vector §2.4.2・§2.5.2 |
| RFC 8032 EdDSA | rfc8032.txt | （SHA256SUMS） | 試験の vector §7.1 |
| RFC 7748 X25519 | rfc7748.txt | （SHA256SUMS） | 試験の vector §6.1 |
| RFC 4648 base64 | rfc4648.txt | 84e14418f795d503 | §4、§3.5 |
| RFC 8160 IUTF8 | rfc8160.txt | 65eefa2fbc0db6bb | 端末の mode の opcode 42 |
| draft-ietf-sshm-mlkem-hybrid-kex-10 | draft-ietf-sshm-mlkem-hybrid-kex-10.txt | d7e1ff8723466a80 | §2.1〜2.5 の鍵の交換 |
| draft-ietf-sshm-chacha20-poly1305-04 | draft-ietf-sshm-chacha20-poly1305-04.txt | a8aa91696db885d2 | §3〜7 の cipher、Appendix A の完全な例（p004 の既知の答えの試験） |
| draft-ietf-sshm-strict-kex-02（と -miller-…-01） | draft-ietf-sshm-strict-kex-02.txt | c08126dcaaf63958 | §3 strict KEX |
| draft-ietf-sshm-ssh-agent-16 | draft-ietf-sshm-ssh-agent-16.txt | 2b83bc2029cce5d6 | §3.2.3 Ed25519 の秘密鍵の符号（鍵の file が使う） |
| draft-ietf-secsh-filexfer-02（SFTP v3） | draft-ietf-secsh-filexfer-02.txt | 3f69664d8050a28a | sftp-server（Q3 次第） |
| FIPS 203 ML-KEM | fips203.pdf（56 頁） | fe1f12f32a7e44ec | §7.2 の ek の検査（実装は WS335） |
| OpenSSH 10.5p1 PROTOCOL・PROTOCOL.key・PROTOCOL.agent・PROTOCOL.mux | openssh-10.5p1-PROTOCOL* | （SHA256SUMS） | 拡張の一覧、鍵の file の形 |
| OpenSSH 10.5p1 sshd.8・ssh.1・sshd_config.5・ssh_config.5・ssh-keygen.1・scp.1 | openssh-10.5p1-*.{1,5,8} | （SHA256SUMS） | authorized_keys・known_hosts の形、設定の keyword |

OpenSSH の文書は BSD の license の tarball（`build/distfiles/openssh-10.5p1.tar.gz`、SHA-256 は userland/packages/network/openssh/Makefile:24）から取り出した。
**出典の扱い**（[Guardrail](../guardrail.md) の出典の節、標準廉価実装手順 §7）: 仕様の本文から独立に書く。OpenSSH・libssh・Dropbear・PuTTY などの code・comment・構造・名前を写さない。
表（端末の mode の opcode、message 番号）は data として block ごとに「Values referenced: <文書> <節>」の複数行の comment で出典を書く。header の先頭に出典の一覧を書かない（各 header の comment は仕様の節を該当の宣言のそばで引く）。

## 2. 今の事実（file:line）

### 2.1 OpenSSH の package と image

- package: `userland/packages/network/openssh/Makefile`。版 10.5p1（:19）、OpenSSL の libcrypto に link（:96・:100 の `--needed libcrypto.so`）、privsep の path `/var/empty`・user `sshd`（:81〜82）、`install-nokeys`（:93）。
  導入する file（:136〜151）: `/usr/bin/ssh`・`/usr/sbin/sshd`・`/usr/bin/ssh-keygen`・`/usr/bin/scp`・`/usr/libexec/sftp-server`・`sshd-session`・`sshd-auth`・`/etc/ssh/sshd_config`（0600）・`/etc/ssh/ssh_config`・`/etc/service.d/sshd`・`/usr/libexec/sshd-start`。登録は menu `packages/network`、既定 n、要求 `security/openssl`（:122〜124）。
- `files/sshd_config`: Port 22・inet・0.0.0.0（:4〜6）、HostKey は ed25519 と rsa（:8〜9）、`PermitRootLogin yes`・`PasswordAuthentication yes`・`PermitEmptyPasswords no`・`PubkeyAuthentication yes`（:11〜14）、`UseDNS no`（:17）、`Subsystem sftp /usr/libexec/sftp-server`（:19）。
- `files/sshd-start`: 初回の起動で rsa・ecdsa・ed25519 の host key を `ssh-keygen -q -t <kind> -N ''` で作る（:33〜43）、`/var/empty` を作る（:24〜27）。
- `files/sshd.service`: `type=daemon`・`command=/usr/libexec/sshd-start`・`arguments=-D`・`after=syslogd,networking`（:1〜7）。`userland/base/etc/rc.conf:61〜63` が `sshd: enabled: true, optional: true`。
- `userland/base/etc/passwd:2` に `sshd:x:22:22:…:/var/empty:/sbin/nologin`、`group:3` に `sshd:x:22:`。
- 既定の config に openssh が入っている: `config/ci/config-amd64.mk:57`・`config/ci/config-rpi4.mk:57`（`… zlib curl openssh ca-certificates openssl …`）ほか rpi5・odroid-c5・zybo・visionfive2-keiland・amd64-server（ユーザーの物、変えない）。`plan/tools/guest/config-amd64-ssh.mk:8`。
- installer の image は menu `base`・`desktop`・`packages/%` の選べる package を全部入れる（Makefile:538〜551）。userland の Makefile は `userland/*/*/Makefile` を wildcard で全部読む（Makefile:497〜503）。→ `userland/base/ssh/Makefile` を置いた時点で package として登録され、installer の image に入る（§10 の段取りの理由）。

### 2.2 置き換えで壊してはならない使い方（試験の道具・実機）

| 使い方 | 例（file:line） | 自前で要る物 |
| --- | --- | --- |
| host の OpenSSH の ssh から guest の root に ed25519 の鍵で login、`BatchMode`・`StrictHostKeyChecking=no`・`UserKnownHostsFile=/dev/null` | plan/tools/guest/guest.py:128〜143 | publickey、exec、exit status |
| host の scp（既定の SFTP mode）で file の送受 | guest.py:280〜291（put・get） | **sftp-server**（OpenSSH 9.0 以降の scp は既定で SFTP を使う。scp(1) `-O` が旧の protocol） |
| `scp -O`（旧の SCP protocol、相手に `scp` の program が要る） | plan/tools/gpu-perf/measure.sh:97、plan/tools/rtx-video/rtx-*-check.sh（3 本） | 旧の scp の受け手、または道具から `-O` を外す（Q3） |
| ControlMaster（1 本の接続に複数の session） | gpu-perf/measure.sh:77、plan/tools/aat/aat:171 | 1 接続に複数の channel |
| password での login（sshpass） | sshpass を使う道具は plan/tools の 14 本。zedBSD が相手と確かめた物: plan/tools/zybo/ssh-check.sh、vf2/run.sh・boot-cycle.sh（hw5330/*.sh などの相手は未確認） | **password 認証**（Q1） |
| `ssh -tt`（pty） | plan/tools/sh/pty-keys.py、ls/guest-compare.py | pty-req・window-change・端末の mode |
| port の転送 `-L` | hw5330/README.md:158、i915-hw/hdmi/*-gdb.sh:36 | 相手は Linux の host（solaris10-man）で zedBSD ではない。zedBSD の sshd には今は不要（Q2） |
| image に rsa・ecdsa・ed25519 の host key を入れる | guest.py:58（`HOST_KEY_KINDS`）、:115〜119 | ed25519 だけを使い、他は無視して log に残す |
| zybo の 8 MiB の scp の往復 | plan/tools/zybo/ssh-check.sh（plan/tools.md:205） | sftp-server、速さ |

host の道具: `ssh -V` = OpenSSH_10.0p2 Debian（OpenSSL 3.5.6）。host の OpenSSL の開発の header（`/usr/include/openssl/evp.h`）と sshpass がある（2026-11-01 確かめ）。

### 2.3 zedBSD の libc・kernel の口（sshd が使う物）

- login の password の確かめ: `userland/base/login/verify.c:34〜71`（`login_verify`、passwd と shadow、locked は `!`・`*`）、`:75〜111`（`verify_hash`、`crypt` と比べる）。su・sudo・passwd・passkey が source を共有する（例 `userland/base/su/Makefile:5`）。
  比較は `strcmp`（verify.c:105）で定数時間ではない（漏れは hash の前方一致の長さだけ）。また存在しない user では getpwnam_r・getspnam_r の失敗（:50・:58）で crypt の前に返る。sshd は同じ関数を使い、時間は sshd.h の「Timing」の規則（応答を要求から `SSHD_FAIL_DELAY_MS` 以上後に揃え、存在しない・locked の user にも dummy の hash で crypt を走らせる）で隠す（§8）。
- login の記録: `userland/base/login/main.c:148`（`USER_PROCESS`）、`:155`（`DEAD_PROCESS`）、`pututxline`。`initgroups`・`setgid`・`setuid`（:120〜122）。
- pty: `include/libc/pty.h:20〜22`（`openpty`・`forkpty`・`login_tty`）、実装 `userland/base/libc/termios.c:549〜566`（`posix_openpt`・`grantpt`・`unlockpt`・`ptsname_r`）。kernel は pty の持ち主を開いた process の ruid にする（`src/kern/tty.c:3079〜3106`）→ user の uid の session process が自分で開けば chown が要らない。
- 権限: `closefrom`（include/libc/unistd.h:141）、`chroot`（:131）、`setgroups`（:251）、`setresuid`・`setresgid`（:253・:255）、`getentropy`（:132、syscall `KERN_SYS_getentropy` include/uapi/syscall.h:178）、`SCM_RIGHTS`（include/uapi/socket.h:121）。
- ptrace: kernel は identity を変えた process を他の process に trace させない（`src/kern/cred.c:194〜196`）。uid sshd の pre-auth の process どうしは互いの memory を読めない。
- sandbox: `sandbox_spawn`（include/libc/sandbox.h、include/uapi/sandbox.h）は static の image を system call の集合（src/kern/sandbox.c:41〜77: read・write・mmap・clock_gettime・getentropy ほか）だけで動かす。pre-auth の process に使える（Q5 の案 C）。
- 外部の library に link する base の program の例: `platform/common/userland-dynamic.mk:569〜588`（passkey-fido2 が OpenSSL の libcrypto.so を link する専用の規則）。ssh・sshd は同じ形で libcrypto-compat.so を link する（§10）。

### 2.4 OpenSSH 10.5p1 の既定（相手の振る舞い）

- 鍵の交換の第一は `mlkem768x25519-sha256`（openssh-10.5p1 `myproposal.h:27〜35`）。cipher の第一は `chacha20-poly1305@openssh.com`（:63〜66）。
- ssh は PQ でない鍵の交換に既定で警告を出す（`ssh_config.5` の WarnWeakCrypto、:2258〜2271「Warnings about connections that don't use a post-quantum key exchange」）。PQ の判定は `kex-names.c:60〜84` の `KEX_IS_PQ`（mlkem768x25519 と sntrup761x25519 が PQ、curve25519-sha256 は `KEX_NOT_PQ`）。
- OpenSSH 10.5p1 の振る舞いの確かめ（相互運用のための事実。code は写さない）: 送りの padding は AEAD の時に 4 byte の長さを除いて block に揃え（`packet.c:1280〜1282`）、受けは AEAD の時 `packet_length % block_size == 0`、鍵なしの時 `(4 + packet_length) % 8 == 0` を求め、外れると切る（`packet.c:1702〜1716`）。受ける packet の上限は 256 KiB（`packet.c:97`）。AEAD の cipher を選んだ時は相手の MAC の一覧を見ない（`kex.c:1020〜1022`）ので、空の MAC の一覧で通る。strict KEX の印は旧の名前 `kex-strict-[cs]-v00@openssh.com` だけを送り、見る（`kex.c:104〜105`・`:959〜968`）。
- host の OpenSSL 3.5.6 で、ssh-crypto.c が使う呼び方を全部確かめた（§11.2 の `crypto-contract.sh`、108 checks PASS、2026-11-01）。ML-KEM-768 の `EVP_PKEY_get_raw_public_key`（1184 bytes）、`EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", …)` の FIPS 203 §7.2 の検査（係数 4095 の ek を拒む）、暗黙の拒否、`EVP_chacha20` の IV が state の word 12〜15 になること、`EVP_MAC` "POLY1305" の同じ context での鍵の入れ直し、X25519 の全 0 の結果の拒否。

## 3. 決定（推し）と選ばなかった案

| # | 決定 | 根拠 | 選ばなかった案と理由 |
| --- | --- | --- | --- |
| D1 | 鍵の交換は `mlkem768x25519-sha256` の 1 つ | ユーザーの決定「最新の定番を 1 つ」。OpenSSH 10.x の第一（§2.4）。OpenSSH 10.1 以降の ssh は PQ でない交換に警告を出すので、curve25519-sha256 だけの sshd は host の ssh で毎回警告になる。P6 も同意（Q1 の中継、2026-11-01） | curve25519-sha256（RFC 8731）: 部品が少ない（ML-KEM・SHA-3 が要らない）が警告が出て、PQ の保護が無い。sntrup761x25519-sha512: 標準の draft でなく OpenSSH の私的な物、ML-KEM より新しくない。両方を持つ: ユーザーの「1 つ」に反する（Q7 で確かめる）。結果として OpenSSH 9.9 より前（例 Debian 12 の 9.2）には接続できない |
| D2 | host key・user key は `ssh-ed25519` だけ（RFC 8709） | ユーザーの決定 | rsa-sha2・ecdsa: 範囲の外。image に入る rsa・ecdsa の host key は読まずに log に残す |
| D3 | cipher は `chacha20-poly1305@openssh.com` だけ。MAC の一覧は空、圧縮は none | OpenSSH の第一。software で定数時間にしやすい。WS335 の P6 の部品が揃う | `aes256-gcm@openssh.com`: bitslice の AES は遅く、EVP の AES-GCM の口が追加で要る。MAC の一覧に名前を書く案: 実装しない MAC を名乗ることになる。AEAD を選んだ OpenSSH は MAC の一覧を見ない（draft-ietf-sshm-chacha20-poly1305-04 §5）。相互の試験で確かめる（§11.3、問題なら "none" を書く案に戻す） |
| D4 | strict KEX を必須にする（両方の名前を送り、相手がどちらも送らなければ断る） | Terrapin（draft-ietf-sshm-strict-kex-02 §2）は chacha20-poly1305 に効く。mlkem を話す相手（OpenSSH 9.9 以降）は必ず strict を持つ | 任意にする: 守りが落ちるだけで相手は増えない |
| D5 | EXT_INFO（RFC 8308）を送らず、`ext-info-c`・`ext-info-s` を名乗らない。受けたら捨てる | ed25519 だけなので server-sig-algs が要らない。ping@openssh.com も使わない | 送る: 実装と試験が増えるだけ |
| D6 | 暗号は全部 WS335 の libcrypto-compat の OpenSSL 3.5 互換の API。呼ぶのは `ssh-crypto.c` 1 file だけ | ユーザーの決定（暗号の部品は WS335、重ねて作らない。libcrypto-compat を使う） | 私的な `sslc_*` の header: ユーザーの決定（2026-11-01 の libcrypto-compat の追加）で取り下げ |
| D7 | 試験と開発は host の OpenSSL 3.5 で先に進め、libcrypto-compat とは同じ source を link だけ替えて確かめる | libcrypto-compat は OpenSSL 3.5 と名前・引数・戻り値の約束が同じ（P6、Q1 の中継）。`plan/ws336/tests/host-crypto/compat/crypto/*.h` が `<openssl/*.h>` を読む shim | WS335 の実装を待ってから始める: WS336 の全 Phase が WS335 の実装の終わりまで止まる |
| D8 | 権限の分離は「monitor（root）＋ pre-auth の process（chroot /var/empty・uid sshd）＋ session の process（user の uid）」。子は fork と execve（sshd 自身を `-Zpreauth`・`-Zsession` で）で始め、設定は monitor から CONFIG で受ける | 認証の前の network の解析を root でしない（OpenSSH と同じ深さ）。execve で address 空間を新しくし、host key の seed・shadow の検索の残り・listener の descriptor が子に残らない（design-review の 1）。zedBSD は chroot・setresuid・pty の持ち主の ruid・closefrom を持つ（§2.3） | A 分離なし（Dropbear の形）: 認証の前の bug がそのまま root。C pre-auth を sandbox_spawn の static image で: system call も絞れるが libcrypto-compat の static の build が要る。後の Phase の候補（Q5） |
| D9 | 各 layer は I/O をしない（bytes を入れて bytes を出す）。I/O と poll は program の側 | 単体試験で client と server を同じ process の中でつなげる。privsep の process の間で同じ code を使える | layer が socket を読む: 試験に socket が要る |
| D10 | 1 つの値・1 つの名前しか持たないので、交渉は「相手の一覧に自分の 1 つがあるか」 | RFC 4253 §7.1 の規則と同じ答えになる | 一般の交渉の表: 使わない一般化 |
| D11 | 秘密鍵の file は暗号化なし（openssh-key-v1、cipher none）だけ | passphrase は bcrypt_pbkdf と aes256-ctr が要り、どちらも libcrypto-compat の範囲の外（bcrypt は OpenSSL の API でもない） | 暗号化の鍵を読む: WS335 に Blowfish・AES-CTR が要る（Q9） |
| D12 | known_hosts の hash の行（`|1|`）は飛ばす。自分は hash せずに書く | HMAC-SHA1 が要る（libcrypto-compat の範囲の外）。zedBSD の側の known_hosts は自前の ssh が書く | 読む: HMAC-SHA1 を足す |
| D13 | authorized_keys の options（`command=` など）付きの行は飛ばす | 制限付きの鍵を制限なしで受け入れない。今の道具は options を使わない | options を実装: 範囲が広い（後で WS） |
| D15 | 自分が KEXINIT を送ってから NEWKEYS を送るまで、誰が始めた交換でも上の layer の送りを held に溜める | RFC 4253 §7.1（design-review の 2） | 自分が始めた交換だけ: 相手の re-key の間に送ってしまう |
| D16 | 拒む request（未知の channel request・global request）は名前と want_reply だけを読み、残りの data を見ない。長すぎる名前・値も誤りにせず FAILURE | OpenSSH の sshd は login の後に key blob 付きの hostkeys-00@openssh.com を送り、-X・-A の client は x11-req・auth-agent-req を送る（design-review の 3） | 末尾の余りを EINVAL: login の直後に接続が切れる |
| D14 | ssh の program は `userland/base/ssh/`（base）。package の名前は ssh・sshd・ssh-keygen・sftp-server | master-design-policy §2.1「base system は独自実装」。OpenSSH は外部の package（packages/network） | `userland/packages/`: 外部の物の置き場 |

## 4. 範囲

### 4.1 protocol

- transport（RFC 4253）: 識別の行 §4.2（自分は `SSH-2.0-zedBSD_1.0`、相手は `SSH-2.0-` か `SSH-1.99-`、client は前置きの行を 64 行・16 KiB まで飛ばす）、packet §6（受ける上限 256 KiB）、KEX §7（D1・D3・D4）、re-key §9（1 GiB・2^28 packet・1 時間。相手からの re-key にいつでも応じる）、汎用の message §11（DISCONNECT・IGNORE・UNIMPLEMENTED・DEBUG）。
- 認証（RFC 4252）: `none`（方法を知るため。server は常に失敗）、`publickey`（ssh-ed25519、query と署名付き）、`password`（Q1 次第。変更の要求は断る）。`keyboard-interactive`・`hostbased`・GSSAPI は無し。
- connection（RFC 4254）: `session` の channel（1 接続に最大 10、ControlMaster のため）、`pty-req`・`env`（server は `LANG`・`LC_*`・`TERM` だけを受ける）・`shell`・`exec`・`subsystem`（`sftp` だけ）・`window-change`・`signal`（受けて子に送る）・`exit-status`・`exit-signal`・EOF・CLOSE・window の調整、`eow@openssh.com`（受けて無視）、`no-more-sessions@openssh.com`（守る）、`keepalive@openssh.com`（global request はすべて失敗で返す。OpenSSH の ServerAlive・ClientAlive はこれで動く）。
- 範囲の外: port の転送（`direct-tcpip`・`tcpip-forward`、Q2）、X11、agent と agent の転送（Q4）、`tun@openssh.com`、streamlocal、証明書の鍵、`hostkeys-00@openssh.com`（受けたら want_reply が無いので捨てる）、圧縮、GSSAPI、`ssh -J`・ProxyCommand。

### 4.2 program と機能

| program | 置く path | 機能 |
| --- | --- | --- |
| sshd | `/usr/sbin/sshd` | `-D`（前で動く、service が使う）・`-f config`・`-p port`・`-t`（設定の確かめ）・`-d`（1 接続だけ、log を stderr）。root で privsep、root でなければ自分の user だけ（試験用、§6.4）。syslog `LOG_AUTH` に OpenSSH と同じ形の行（`Accepted publickey for user from A port N ssh2: ED25519 SHA256:…`、`Failed password for …`） |
| ssh | `/usr/bin/ssh` | `[user@]host [command]`・`-p`・`-l`・`-i`・`-o`・`-F`・`-t`/`-tt`/`-T`・`-n`・`-q`・`-v`・`-N`（command 無し）・`~.`（切る）。known_hosts（`~/.ssh/known_hosts`・`/etc/ssh/ssh_known_hosts`）、StrictHostKeyChecking（ask・yes・no・accept-new）、BatchMode、ConnectTimeout、ServerAliveInterval。終了の値は相手の exit-status（signal で終われば 255、接続の失敗も 255、OpenSSH と同じ） |
| ssh-keygen | `/usr/bin/ssh-keygen` | `-t ed25519`（他は断る）・`-f`・`-N ''`（空だけ、D11）・`-C`・`-q`・`-l`（指紋）・`-y`（秘密鍵から公開鍵）・`-A`（host key が無ければ ed25519 を作る）・`-R host`（known_hosts から消す、旧 file を `.old` に残す） |
| sftp-server | `/usr/libexec/sftp-server` | SFTP v3（Q3 次第）。stdin・stdout で話す、user の権限 |
| scp（旧の受け手） | `/usr/bin/scp` | Q3 で「入れる」の時だけ。`scp -t`（受ける）・`scp -f`（送る）・`-r`・`-p` の受け手 |

zedBSD の側の scp・sftp の client（zedBSD から外へ送る）は範囲の外（Q3 の補い）。

## 5. module の分割（header が仕様）

依存は一方向。program の file（`*-main.c`・`sshd-*.c`）は library の module を使うだけで、module は program を知らない。

| module（header / .c） | 役割 | 仕様 | 依存 |
| --- | --- | --- | --- |
| ssh-common | 約束（誤りの値・信用できない入力・秘密）、上限の定数、byte 順、name-list、base64 | RFC 4251 §5・§6、RFC 4648 §4 | — |
| ssh-buf | RFC 4251 §5 の型の書き手（伸びる buffer、秘密の時は消しながら伸ばす）と読み手（範囲の確かめ）、sticky な誤り | RFC 4251 §5 | common |
| ssh-crypto | libcrypto-compat の呼び出しの全部（§7） | FIPS 180-4、RFC 8032、RFC 7748、FIPS 203、draft mlkem §2.4、draft chacha §3・§4 | common、libcrypto-compat |
| ssh-cipher | chacha20-poly1305@openssh.com の組み立て | draft chacha §6・§7 | crypto |
| ssh-packet | binary packet、padding、seq、識別の行の読み書き | RFC 4253 §4.2・§6 | buf、cipher |
| ssh-key | ed25519 の blob・署名・公開鍵の行・秘密鍵の file・指紋 | RFC 8709 §4・§6、PROTOCOL.key、agent draft §3.2.3、sshd.8 | buf、crypto |
| ssh-hostfile | authorized_keys・known_hosts・host の pattern | sshd.8 の 2 節、ssh_config.5 PATTERNS | key |
| ssh-kex | KEXINIT・交渉・strict の印・hybrid の message・交換の hash H・鍵の導出 | RFC 4253 §7・§7.2、draft mlkem §2、draft strict §3.1 | buf、crypto、cipher（名前） |
| ssh-transport | transport の状態機械（§6.1）、re-key、export・import | RFC 4253、draft strict §3 | packet、kex、key |
| ssh-auth | service request と userauth の message、署名される data | RFC 4252、RFC 4253 §10 | key |
| ssh-ttymodes | 端末の mode の符号 | RFC 4254 §8、RFC 8160 | buf |
| ssh-channel | channel の表・window・message | RFC 4254、PROTOCOL §2.1・§2.2 | buf |
| ssh-config | sshd_config・ssh_config・`-o` | sshd_config.5、ssh_config.5 | common（pattern は hostfile） |
| ssh-privsep | monitor との frame と同期の呼び出し | （自前の約束、header に全部） | buf、packet（上限） |
| ssh-sftp | SFTP v3 の request の読みと reply の書き | filexfer-02 §3〜§7 | buf |
| sshd.h（sshd-monitor・-preauth・-session・-login・-main） | sshd の process | §6 | 上の全部 |
| ssh-main・ssh-keygen-main・sftp-server-main | 各 program | §4.2 | 上の全部 |

header は 16 本（`userland/base/ssh/*.h`）、関数は 182 個（library の module 171、sshd の process 7、program の main 4）。全 .c は骨格（`return ENOSYS;`）で、cc と clang の `-std=c89 -Wall -Wextra -Werror -Wpedantic` で通る（`check-skeleton.sh` PASS、2026-11-01）。

**Makefile はまだ置かない**（§4.4 の「build が通る形」の読み替え）: `userland/base/ssh/Makefile` を置くと toplevel の wildcard が読み（Makefile:497〜503）、選べる base の package として installer の image にまで入る（Makefile:538〜551）。未完成の ssh が image に入るのを避け、骨格の build の確かめは `check-skeleton.sh`（host の 2 つの compiler）で行う。Makefile・link の規則・image への登録は p021 で入れる。

## 6. process と状態遷移

### 6.1 transport の状態

```
VERSION ──相手の識別の行──▶ INITIAL_KEX ──自分の NEWKEYS 送り・相手の NEWKEYS 受け──▶ RUNNING ⇄（re-key: kex_step≠0）
   │                          │                                                   │
   └────── 誤り・DISCONNECT ──┴────────────────────────────────────────────────────┴──▶ CLOSED（出力の残りだけ）
```

1 回の交換の手順（ssh-transport.h の冒頭）:
- client: KEXINIT 送 → KEXINIT 受（交渉、strict を決める）→ `ssh_hybrid_client_start` → HYBRID_INIT 送 → HYBRID_REPLY 受（長さの確かめ → `check_host_key` → K → H → 署名の確かめ）→ NEWKEYS 送（送りの鍵 'C' に替え、strict なら send seq=0）→ NEWKEYS 受（受けの鍵 'D'、strict なら receive seq=0）。
- server: KEXINIT 送 → KEXINIT 受 → HYBRID_INIT 受 → `ssh_hybrid_server`（長さ・FIPS 203 §7.2・X25519 の 0）→ H → `sign` → HYBRID_REPLY 送 → NEWKEYS 送 → NEWKEYS 受。
- 最初の H が session id。自分が始めた交換の間の上の layer の送りは `held` に溜め、自分の NEWKEYS の後に順に送る。
- strict の初回の交換の間: 最初の受けは KEXINIT、許すのは 20・21・30・31 を各 1 回だけ、seq の wrap は EPROTO（draft strict §3.2）。

### 6.2 sshd の process（D8）

```
listener（root、sshd-main.c）─accept─▶ fork ─▶ monitor（root、sshd-monitor.c）
                                                 ├─socketpair─▶ pre-auth（chroot /var/empty、setgroups・setresgid・setresuid 22）
                                                 │     transport・userauth。SIGN・HOST_KEY・PUBKEY_QUERY・PUBKEY_LOGIN・PASSWORD_LOGIN を monitor に問う
                                                 │     許可 → AUTH_DONE（transport の export）→ exit
                                                 └─▶ session（initgroups・setresgid・setresuid user、chdir home、transport の import）
                                                       channel・pty（自分で openpty、持ち主は user）・shell/exec/sftp-server の fork+exec
                                                       re-key の SIGN と LOGIN_RECORD・LOGOUT_RECORD を monitor に頼む
```

子の始め方と descriptor（sshd.h の冒頭）: monitor は socketpair を作り、fork の後に接続と socket を fd 3・4 へ dup2 して sshd 自身を `-Zpreauth`（`-Zsession`）で execve する。sshd の開く descriptor は全部 close-on-exec、子は最初に `closefrom(5)`。pre-auth は CONFIG（sshd_config の text・host key の公開の blob・相手・privileged）を受け、crypto を一度ずつ使ってから chroot と setresuid。USERAUTH_SUCCESS は pre-auth が送り、出力を書き切ってから AUTH_DONE。session は SESSION_START（user と state）を受け、ids を取ってから import。shell・command の子には pty か pipe だけが渡る。MaxStartups は monitor ごとの pipe（login を許したら 1 byte）で listener が数える。authorized_keys は monitor が fork した user の ids の子が `O_NONBLOCK|O_NOFOLLOW` で開き fstat で確かめて pipe で渡す（root が user の選んだ path を開かない）。

monitor の守り（ssh-privsep.h の冒頭に全部）:
- monitor が自分で持つ状態: 段（pre-auth・session）、session id（最初に署名した H、以後 H の長さは 32 だけ）、認証中の user、残りの試行回数（MaxAuthTries 6）、LoginGraceTime（120 秒、過ぎたら pre-auth を kill）、pre-auth の署名の数（`SSHD_PREAUTH_SIGN_MAX` 4）、session の段の署名の間隔（`SSHD_SIGN_INTERVAL_MS` 1 秒、早い要求は待たせる。1 GiB ごとの re-key を数で切らないため。design-review の 4）。
- 受け入れた危険: session id は pre-auth が最初に頼む H なので、乗っ取られた pre-auth と経路の上の攻撃者が組めば、別の接続の H に署名させて利用者の login を中継できる。monitor は自分の加わっていない交換を見られない（OpenSSH の monitor も同じ。ssh-privsep.h の冒頭に書いた）。
- PUBKEY_LOGIN: 署名された data が `string session_id || 50 || user || "ssh-connection" || "publickey" || TRUE || "ssh-ed25519" || blob` の形で、session id・user・blob が monitor の値と一致することを monitor が自分で読んで確かめ、authorized_keys（StrictModes）と署名を確かめる。
- 返事は理由を言わず、要求から 1 秒以上後（それより長くかかった確かめはその時に）。存在しない・locked の user にも dummy の hash で crypt を走らせ、仕事の量を揃える（ENOENT を EACCES と同じに扱う）。
- AUTH_DONE の state は信用しない（import が全部確かめる）。session の process は user の権限なので、そこが乗っ取られても user が自分でできる以上の事はできない。monitor が session に与えるのは SIGN（H の長さだけ、数の上限）と自分の tty の記録だけ。

### 6.3 ssh（client）

1 process。connect（`getaddrinfo`、ConnectTimeout）→ transport（`check_host_key`: known_hosts の答えで MATCH は通す、CHANGED・REVOKED は断る（BatchMode でも）、ABSENT・OTHER_TYPES は StrictHostKeyChecking と BatchMode で ask・accept-new・no・yes を決め、ask は `/dev/tty` で yes/no を聞き、受けたら `~/.ssh/known_hosts` に足す）→ service request → userauth（none → identity ごとに query → 署名付き → password を最大 3 回、BatchMode なら聞かない）→ channel を 1 つ開き、pty-req（RequestTTY と isatty）→ shell か exec → poll の loop（stdin → DATA、DATA → stdout、EXTENDED_DATA → stderr、SIGWINCH → window-change、`~.`、ServerAlive）→ exit-status を終了の値に。

### 6.4 root でない sshd（試験のための mode）

`geteuid() != 0` の時: monitor・pre-auth・session の 3 つの process と socketpair と message は同じで、chroot と ids の変更だけをしない。自分の user だけを受け入れ、password を断る。これで monitor の protocol・AUTH_DONE・export と import を host で試せる（design-review の 10）。host の上で OpenSSH の ssh・scp・sftp と自前の sshd を相互に試すため（§11.3）。image では root で動くので使わない。log に「not running as root: serving only <user>」を出す。

## 7. WS335 の口（libcrypto-compat、P6 との合意）

2026-11-01 Q1 の中継で P6 と合意した事（WS335 の header は未凍結。P6 の見込みでは 1〜2 時間で凍結）:

- header は `include/libc/compat/crypto/{types,err,x509,evp,rand,crypto}.h`。ssh は `<compat/crypto/evp.h>`・`<compat/crypto/rand.h>`・`<compat/crypto/crypto.h>` を include し、zedBSD では `libcrypto-compat.so` を link する。名前・引数・定数・戻り値の約束は OpenSSL 3.5 と同じ。
- 鍵の交換は mlkem768x25519-sha256（P6 同意）。P6 は SHA-3・SHAKE（FIPS 202）と ML-KEM-768（FIPS 203）を足す（実装の Phase は約 3 つ）。
- ML-KEM: 鍵の生成は `EVP_PKEY_CTX_new_from_name(NULL, "ML-KEM-768", NULL)`＋`EVP_PKEY_keygen_init`＋`EVP_PKEY_keygen` だけ（`EVP_PKEY_Q_keygen` は無し）。ek の取り出しは `EVP_PKEY_get_raw_public_key`（`OSSL_PARAM`・`core_names` は無し）。取り込みは `EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, ek, 1184)` で FIPS 203 §7.2 の検査をする。`EVP_PKEY_encapsulate_init`・`EVP_PKEY_decapsulate_init` の params は NULL だけ。
- `EVP_chacha20`: IV の 16 bytes が state の word 12〜15（little-endian）。counter は word 12〜13 の 64 bit で、word 13 へ繰り上がる（OpenSSL は 32 bit で繰り上がらない。違いは 2^32 block の後だけで、ssh の使い方（counter 0 か 1、packet は 256 KiB 以下）には現れない）。
- Ed25519 は全部実装（署名・確かめ・raw の鍵）。`aes256-gcm@openssh.com` 用の AES-GCM の EVP は、ユーザーが選ばない限り足さない。
- ssh-crypto.c が使う関数の一覧は ssh-crypto.h の冒頭（RAND_bytes、OPENSSL_cleanse、CRYPTO_memcmp、EVP_MD_CTX_new/free、EVP_DigestInit_ex/Update/Final_ex、EVP_sha256、EVP_PKEY_new_raw_private_key、EVP_PKEY_new_raw_public_key、EVP_PKEY_new_raw_public_key_ex、EVP_PKEY_get_raw_public_key、EVP_PKEY_free、EVP_DigestSignInit、EVP_DigestSign、EVP_DigestVerifyInit、EVP_DigestVerify、EVP_PKEY_CTX_new、EVP_PKEY_CTX_new_from_name、EVP_PKEY_CTX_new_from_pkey、EVP_PKEY_CTX_free、EVP_PKEY_keygen_init、EVP_PKEY_keygen、EVP_PKEY_derive_init、EVP_PKEY_derive_set_peer、EVP_PKEY_derive、EVP_PKEY_encapsulate_init、EVP_PKEY_encapsulate、EVP_PKEY_decapsulate_init、EVP_PKEY_decapsulate、EVP_CIPHER_CTX_new/free、EVP_CipherInit_ex、EVP_CipherUpdate、EVP_chacha20、EVP_MAC_fetch、EVP_MAC_free、EVP_MAC_CTX_new/free、EVP_MAC_init、EVP_MAC_update、EVP_MAC_final）。`crypto-contract.c` は加えて `EVP_Digest`（1 回で）を確かめる。
  **確かめ済み（2026-11-01、P6 の答えを Q1 が中継）**: 上の一覧は全部 WS335 の `compat/crypto/evp.h`・`crypto.h` に OpenSSL 3.5 の signature で宣言され、意味（POLY1305 の鍵の入れ直し、X25519 の全 0 の拒否、Ed25519 の DigestSign・DigestVerify、1 回の EVP_Digest）は evp.h の comment に書かれている。`crypto-contract.c` は P6 の compat の header と骨格に対して `-std=c89 -Wall -Wextra -Werror` で警告 0 で compile・link できた（実行は骨格が ENOSYS を返すので未だ FAIL、想定どおり）。P6 は `crypto-contract.sh`（`SSH_CRYPTO_INCLUDE`・`SSH_CRYPTO_LIB`）を WS335 の実装の Phase p023・p024 の受け入れに入れる。
- 照合の方法: WS335 の header が main に入ったら `check-skeleton.sh` の 4 番目（ssh-crypto.c を libcrypto-compat の header だけで compile）が自動で働く。libcrypto-compat の実装が入ったら `SSH_CRYPTO_INCLUDE=… SSH_CRYPTO_LIB=… sh plan/ws336/tests/crypto-contract.sh` が libcrypto-compat の受け入れ（ssh が呼ぶ分）になる。

## 8. 安全

- **定数時間**: 秘密の比較は `ssh_ct_equal`（CRYPTO_memcmp）だけ。tag の確かめは復号の前（draft chacha §7）。秘密の長さは公開（packet の長さ）。hybrid の K は固定長（draft mlkem §2.4・§6、K を string で H に入れる）。
- **秘密の消去**: 秘密を持つ構造体は _clear・_free で消す。`struct ssh_buf` の `secret` は伸びる時に新しい memory へ写して古い方を消す（realloc を使わない）。monitor の host key・password・鍵の file の decode した bytes を消す。
- **乱数**: `RAND_bytes`（libcrypto-compat は getrandom・getentropy を使う。sandbox の基本の集合に getentropy がある src/kern/sandbox.c:69）。KEXINIT の cookie・padding・X25519・ML-KEM・checkint。失敗は EIO で接続を切る（代わりの弱い源は無い）。
- **権限の分離**: §6.2。pre-auth は chroot と uid sshd、session は user。monitor の checks は §6.2。
- **総当り**: MaxAuthTries（既定 6）、LoginGraceTime（120 秒）、MaxStartups（認証前の接続 10。超えたら新しい接続を即閉じる）、応答は要求から 1 秒以上後。
- **時間の差**: 応答の時刻を要求から `SSHD_FAIL_DELAY_MS` 以上後に揃え、存在しない・locked の user も crypt を走らせる（sshd.h）。p020 で差 50 ms 未満を測る。login の失敗は syslog の LOG_AUTH に user・address・port を残す（OpenSSH と同じ形で、後で数えられる）。
- **信用できない入力**: 全 module の header に約束（ssh-common.h）。上限: packet 256 KiB、識別の行 255、name-list 8 KiB、KEXINIT 16 KiB、鍵の file 16 KiB、hostfile 1 MiB、設定 256 KiB、monitor の frame 64 KiB（state だけ大きい）、sftp の packet 256 KiB。
- **端末**: USERAUTH_BANNER と known_hosts の問いに出す相手の文字は制御文字を `?` に置き換える（ssh-auth.h）。
- **環境変数**: server は `LANG`・`LC_*`・`TERM` だけを受ける（`LD_*` などで子を操られないため）。

## 9. 失敗と回復

| 失敗 | 振る舞い |
| --- | --- |
| 相手が交渉できない（KEX・cipher・host key・strict が無い） | DISCONNECT 3（KEY_EXCHANGE_FAILED）に種類を書いて閉じる。client は「no matching key exchange method found」と OpenSSH に似た文を stderr |
| tag の誤り | DISCONNECT 5（MAC_ERROR）、閉じる（RFC 4253 §6.4、飛ばさない） |
| host key の不一致（CHANGED・REVOKED） | DISCONNECT 9、client は大きな警告を出し 255 で終わる。BatchMode でも同じ |
| 認証の失敗の尽き | DISCONNECT 14 |
| 乱数・crypto の失敗（EIO） | DISCONNECT 2、閉じる |
| pre-auth の process の異常終了 | monitor は接続を閉じて終わる（session を作らない） |
| session の process の異常終了 | monitor が LOGOUT_RECORD を自分で書き、終わる |
| 子（shell）の終了 | exit-status か exit-signal、EOF、CLOSE。pty の出力を全部送ってから |
| peer の DISCONNECT | ECONNABORTED、理由を log・stderr |
| 書けない（相手が読まない） | 出力 4 MiB を上限に止め、channel の window で上の layer を止める |
| sshd の設定の誤り | 起動しない、`line N:` を stderr と syslog（service の restart=on-failure は繰り返すだけなので、`sshd -t` を sshd-start で先に流す） |

## 10. 置き場所・build・OpenSSH との入れ替え

- source: `userland/base/ssh/`（header 16 本・.c 23 本）。試験の道具: `plan/ws336/tests/`。
- package（p021 で置く）: `userland/base/ssh/Makefile` に 4 つ（ssh・sshd・ssh-keygen・sftp-server、Q3 で scp を足すなら 5 つ）。**移行の間は menu を `tests`**（installer の image に入らない、既定 n）にして OpenSSH と並べない。link の規則は `platform/common/userland-dynamic.mk` に passkey-fido2（:569〜588）と同じ形で `libcrypto-compat.so` と `libc.so`（sshd は加えて login/verify.c の source と crypt）。base の program は arch で有無を分けない（Guardrail の 2026-10-19）ので、libcrypto-compat が全 arch で build できることが前提（WS335 の担当）。
- 同じ path を OpenSSH と取り合う（`/usr/bin/ssh`・`/usr/sbin/sshd`・`/etc/service.d/sshd`・`/usr/libexec/sftp-server`）ので、1 つの image にはどちらか一方。p022 の試験の config（`plan/ws336/tests/config-amd64-ssh.mk`）は openssh を外して自前を入れる。
- 自前の package の file: `files/sshd_config`（HostKey は ed25519 だけ、`PermitRootLogin yes`・`PasswordAuthentication yes` は今の image と同じ、`Subsystem sftp /usr/libexec/sftp-server`）、`files/sshd-start`（ed25519 だけ `ssh-keygen -A`、`sshd -t` で確かめてから exec）、`files/sshd.service`（今と同じ）、`ssh_config`（今と同じ 1 行。`VerifyHostKeyDNS` は「受けて無視」の一覧に入れる）。
- 入れ替え（p024、時期は Q6）: 既定の config（amd64・rpi4・rpi5・odroid-c5・zybo・visionfive2-keiland。amd64-server はユーザーの物なので Q1 が扱う）の `openssh` を自前の 4 つに替え、`openssl` が他に要らなければ外す。自前の package を menu `base`・既定 y に移す。OpenSSH の package は tree に残し選べるままにする（消すのは別の判断）。guest.py の `HOST_KEY_KINDS` は ed25519 だけで足りる（rsa・ecdsa を入れても害は無い）。`scp -O` の 4 本は Q3 の答えで直す。

## 11. 試験の計画

### 11.1 host の単体（各 Phase、`sh plan/ws336/tests/run-unit.sh <name>|all`）

- `plan/ws336/tests/unit/test-<module>.c`（実装の担当が書く）。全 module と host の OpenSSL（shim）を link、ASan・UBSan、最後の行 `run-unit: PASS`。
- 補助: `unit/check.h`（CHECK・CHECK_EQ・CHECK_BYTES）、`unit/wire.h`（ssh-buf と独立の RFC 4251 の書き手と hex の読み）。期待の値は仕様の vector・手の計算・`wire.h` で組んだ bytes。他の実装の出力を期待の値にしない。
- 「逆向きの独立の実装」: client と server の両方の側を持つ module（packet・kex・transport・auth・channel）は 2 つの instance をつないで往復させ、加えて `wire.h` で組んだ壊れた入力を与える。
- 乱数の入力（各 parser に 10 万回以上の random と変異の入力を与え、ASan・UBSan で落ちず、必ず終わる）。

### 11.2 crypto の契約（`sh plan/ws336/tests/crypto-contract.sh`）

host の OpenSSL 3.5.6 で 108 checks PASS（2026-11-01）。libcrypto-compat ができたら環境変数で同じ試験を流す（p021、WS335 の受け入れの補い）。

### 11.3 host の相互運用（p015・p018・p019 が書いて流す。monitor の 3 process は p017 の `sshd-pair-host.sh`。1 本の script `plan/ws336/tests/interop-host.sh`）

- 自前の ssh・sshd・ssh-keygen・sftp-server を host の OpenSSL で host 向けに build（`build-host.sh`）。
- 相手 A: host の OpenSSH sshd を root でなく高い port で（一時の設定・ed25519 の host key・一時の authorized_keys、`UsePAM no`）。自前の ssh から: exec と exit status、8 MiB の stdin の往復の cmp、`-tt` で `stty size`、known_hosts の accept-new と CHANGED の拒否、`RekeyLimit 1M` の相手で re-key を跨ぐ 64 MiB の転送。
- 相手 B: 自前の sshd（root でない mode）。host の OpenSSH の ssh・scp・sftp から: 上と同じに加え、ControlMaster で 3 session、scp（既定の SFTP mode）8 MiB の往復の cmp、`sftp -b` の ls・put・get・mkdir・rename、**ssh の stderr に post-quantum の警告が出ない**こと、`-o KexAlgorithms=curve25519-sha256` と `-o Ciphers=aes128-ctr` で接続が失敗し、sshd が生きていること。
- 自前どうし（ssh → sshd）。
- 判定は各手順の終了の値と cmp で、最後の行 `interop-host: PASS`。

### 11.4 QEMU（p022、1 本の script `plan/ws336/tests/qemu-interop.sh`）

- image は `plan/ws336/tests/config-amd64-ssh.mk`（`plan/tools/guest/config-amd64-ssh.mk` を元に openssh を自前に替える）と、guest.py の `extra-files` の鍵（AGENTS.md §9 の image の作り方）。
- root の sshd（privsep の本番の形）に host の OpenSSH から: 鍵の root login、password（`user`、sshpass）、scp、ControlMaster、`-tt`、re-key。guest の ssh から host の OpenSSH sshd（root でない、QEMU の user net の 10.0.2.2 の port）と guest 自身の sshd へ。
- privsep の確かめ: guest で pre-auth の process の uid が 22 で root が `/var/empty` であること（`ps` と `/proc` 相当の読み）、session の process が user の uid であること。
- 判定は SSH の結果と script の要約だけ（console・serial の log は読まない、AGENTS.md §9）。起動の確認は `plan/tools/boot-test.sh`。
- 他の arch（aarch64・riscv64・armv7）は build（警告 0）まで（p023）。

### 11.5 実機（p024 の後、ユーザーが拾える試験の一覧へ）

- 5330 の zedBSD（NVMe）・VF2・ZYBO の既存の道具（sshpass・scp・ssh-check.sh）がそのまま通ること。Q1 が割り当てた機械で担当が 1 本の script で流す。

## 12. 実装の Phase の表

「WS335 header」= WS335 の libcrypto-compat の header の凍結（main に merge）を待つ。「WS335 実装」= libcrypto-compat の該当の部分（ML-KEM・SHA-3・Ed25519・X25519・ChaCha20・Poly1305・SHA-256・RAND）が実装され、zedBSD の build に入ってから。他の Phase は module の依存だけを待つ（host の OpenSSL で試すので WS335 の実装は待たない）。

| Phase | module・内容 | 担当（推し） | 依存 | WS335 の待ち |
| --- | --- | --- | --- | --- |
| [p002](phase002/phase.md) | ssh-common・ssh-buf | Sonnet | p001 | — |
| [p003](phase003/phase.md) | ssh-crypto（libcrypto-compat の呼び出し） | Sonnet | p002 | **WS335 header** |
| [p004](phase004/phase.md) | ssh-cipher・ssh-packet（draft の Appendix A の既知の答え） | Sonnet | p003 | （p003 経由） |
| [p005](phase005/phase.md) | ssh-key | Sonnet | p003 | （p003 経由） |
| [p006](phase006/phase.md) | ssh-hostfile | Sonnet | p005 | （p003 経由） |
| [p007](phase007/phase.md) | ssh-kex | Sonnet | p004 | （p003 経由） |
| [p008](phase008/phase.md) | ssh-transport | Opus（状態機械と安全の要） | p005・p007 | （p003 経由） |
| [p009](phase009/phase.md) | ssh-auth | Sonnet | p005 | （p003 経由） |
| [p010](phase010/phase.md) | ssh-ttymodes・ssh-channel | Sonnet | p002 | — |
| [p011](phase011/phase.md) | ssh-config | Sonnet | p006 | （p003 経由） |
| [p012](phase012/phase.md) | ssh-privsep（frame） | Sonnet | p002 | — |
| [p013](phase013/phase.md) | ssh-sftp（Q3 で sftp を入れる時だけ） | Sonnet | p002 | — |
| [p014](phase014/phase.md) | ssh-keygen の program | Sonnet | p006 | （p003 経由） |
| [p015](phase015/phase.md) | ssh の program、`build-host.sh`、`interop-host.sh`（相手 A） | Opus | p006・p008・p009・p010・p011 | （p003 経由） |
| [p016](phase016/phase.md) | sshd の monitor と login の確かめ、`test-monitor.c` | Opus | p009・p011・p012・p014 | （p003 経由） |
| [p017](phase017/phase.md) | sshd の pre-auth と session の process、`sshd-pair-host.sh` | Opus | p008・p010・p016 | （p003 経由） |
| [p018](phase018/phase.md) | sshd の listener（MaxStartups）、`interop-host.sh`（相手 B と自前どうし） | Opus | p015・p017 | （p003 経由） |
| [p019](phase019/phase.md) | sftp-server の program（と Q3 次第で旧の scp の受け手） | Sonnet | p013・p018 | — |
| [p020](phase020/phase.md) | 敵対的な入力と privsep の見直し（fuzz の harness、時間の差、descriptor の漏れ） | Opus | p018・p019（Q3 が C なら p018） | — |
| [p021](phase021/phase.md) | package と amd64 の build、libcrypto-compat での crypto-contract・単体・相互運用 | Opus | p020 | **WS335 実装** |
| [p022](phase022/phase.md) | QEMU の相互運用（root の sshd と privsep） | Opus | p021 | **WS335 実装** |
| [p023](phase023/phase.md) | 他の arch の build | Sonnet | p021 | **WS335 実装** |
| [p024](phase024/phase.md) | OpenSSH からの入れ替え（既定の config・道具の `scp -O`）、実機の試験の一覧 | Opus | p022・p023、Q6 の答え | **WS335 実装** |
| [p025](phase025/phase.md) | 規約の全文の見直し（header の持ち越しの直しも） | conformance-reviewer | p024 | — |

並べて起動できる組（p001 の merge の後）: p002 → {p003（WS335 header の後）、p010、p012、p013} → {p004、p005} → {p006、p007、p009} → {p008、p011、p014} → {p015、p016} → p017 → p018 → p019 → p020 → p021 → {p022、p023} → p024 → p025。

## 13. 標準廉価実装手順 §4 の照合

| 項目 | 状態 |
| --- | --- |
| §4.1 仕様の置き場所・SHA-256 | §1、`build/sources/specs/ssh/SHA256SUMS`。text なので頁の換算は無い（FIPS 203 は WS335 が読む） |
| §4.2 module の分割（一方向、1 Phase = header 1〜3） | §5、§12 |
| §4.3 header の comment（定数・構造体・関数の 7 項目・共通の約束） | `userland/base/ssh/*.h`。共通の約束は ssh-common.h |
| §4.4 .c の骨格（ENOSYS） | 23 本。Makefile は §5 の理由で p021 |
| §4.5 試験の道具 | run-unit.sh（program の内部の試験は ` * run-unit-sources:` で file を名指す）・check-skeleton.sh（BASE の照合は library の header だけ、2 つの compiler、crypto を呼ぶ file の限定、開発の管理情報の検出、libcrypto-compat の header での compile、zedBSD の clang と sysroot での compile）・check.h・wire.h・test-tools.c（道具の自己試験）・host-crypto の shim・crypto-contract（適合の試験の代わりに仕様の vector）。相互運用の script は program の Phase が書く（§11.3） |
| §4.6 Phase の表と phase.md | §12、`phase002/`〜`phase025/` |
| §4.7 design-reviewer | [design-review.md](design-review.md) |
| §4.8 Q1 に返す物 | 報告 |

## 14. HAL

HAL・kernel・driver の変更は無い。sshd が使う kernel の口（chroot・setresuid・pty・SCM_RIGHTS・getentropy）は全部ある（§2.3）。

## 15. ユーザーへの問い

[user-questions.md](user-questions.md)（Q1〜Q9。password 認証、port の転送、scp・sftp、agent、権限の分離の深さ、OpenSSH からの入れ替えの時期、鍵の交換の 1 つの確かめ、cipher、鍵の passphrase）。

## 16. design-reviewer（2026-11-01）

[design-review.md](design-review.md) に全指摘と扱い。32 件（高 3・中 14・低 15）を全部反映した（見送り 0）。主な変更: 子の process を execve で始める（seed を受け継がない）、相手の re-key の間も上の layer の送りを溜める、拒む request の data を見ない、署名の数でなく間隔の制限、応答の時刻を揃える、CLOSING の扱い、cipher の既知の答え（Appendix A）、root なしでも 3 process、zedBSD の header での compile の確かめ、p016 と p019 の分割（Phase は p002〜p025）。
