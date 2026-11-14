<!-- awesome-plan project=zedbsd record=ws343-p003 -->

# ws343-p003 fetch の https（libssl-compat・libcrypto-compat）と、mk の差分の案（libcurl.so の link、fetch の link）

- 親: [ws.md](../ws.md)。Queue: q2096-i01（P8、worktree `/home/awe/zedBSD-worktrees/p8-ws343`、branch `agent/p8-ws343`、基点 main c231c505e8）。
- Status: cleared（2026-11-14 Q1、merge fdc6f069e1）

## 目的と範囲

- base の fetch が https の URL を取れるようにする。TLS は libssl-compat の OpenSSL 3.5 互換の API。証明書の鎖は `/etc/ssl/cert.pem`（ca-certificates）で、名前は URL の host で確かめる。redirect は http と https の間も辿る。
- Makefile・`mk/` は Claude が変えない。要る変更は差分の案として下に書く（承認まで当てない。build の確かめは作業の copy に当てて行い、commit しない）。
- 他の実装（curl・FreeBSD の fetch など）の code は見ていない。RFC 9110・9112・3986・6066・8446 と、libssl-compat の header（`include/libc/compat/ssl/ssl.h`）と mailer の `tls.c`（同じ library を呼ぶ自前の code）から書いた。

## 作った物

| 所 | 中身 |
| --- | --- |
| `userland/base/fetch/main.c` | 書き直し（coding-style の形に合わせた）。https の URL（既定の port 443）、TLS 1.2 以上、`SSL_VERIFY_PEER`、`SSL_CTX_set_default_verify_paths`（`/etc/ssl/cert.pem`）、`-C cafile` で足す CA、SNI（address literal には送らない、RFC 6066 3 節）、`SSL_set1_host` で名前か address を確かめる。確かめに落ちた server には request を送らない。長さ・chunked の body は終わりを待たず、終わりまでの body は TLS の close_notify を要る（無ければ切れた body かもしれないので失敗）。Location は絶対の http・https、`//host/path`、絶対の path、相対の path（今の path の directory に足す）を辿る。fragment は送らない。user 名の入った URL は断る。SIGPIPE は無視（server が先に閉じた後の close_notify の write で死なない）。失敗は file を残さない |
| `plan/ws343/tests/run-fetch-https.sh`・`https-server.py` | host の試験。fetch を host 向けに ASan・UBSan で build し、`plan/tools/tls/host-compat-libs.sh` の compat の static の archive に link。loopback の server（host の Python の ssl）と openssl で作った CA・leaf で 18 の場合 |
| `plan/ws343/tests/run-fetch-qemu.sh`・`config-amd64-fetch.mk` | QEMU の試験。既定の x86_64 の image を `plan/tools/guest/test-image.sh` で作り、`guest.sh` で起動し、公開の https の site から取って host の curl の取得と SHA-256 で比べる。自己署名の site を断る事も |

失敗の文言（例、host 試験の出力から）:

- `fetch: localhost: TLS handshake failed: certificate verify failed: unable to get local issuer certificate`（ほか self-signed certificate・certificate has expired・hostname mismatch）
- `fetch: /etc/ssl/cert.pem is missing: install the ca-certificates package or give -C cafile`（system の root が無く、-C も無い時に足す）
- `fetch: localhost: TLS read failed: error:0A000126:SSL routines::stream ended without a close_notify`
- `fetch: cannot read the CA file <path>`、`fetch: too many redirects`、`fetch: HTTP status 404`

## mk・Makefile の差分の案（未適用、ユーザーの承認待ち）

1. `userland/base/fetch/Makefile`: fetch を libssl-compat・libcrypto-compat に link する（require の欄）。これが無いと fetch は SSL_* が未定義で link に失敗する。

```diff
@@ -5,7 +5,9 @@
 
 include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk
 
+# fetch's https is libssl-compat and libcrypto-compat, zedBSD's own TLS libraries, which the program links.
+
 FETCH_SOURCES := \
 	userland/base/fetch/main.c
 
-$(eval $(call USERLAND_PACKAGE,fetch,fetch,*,y,network,$(FETCH_SOURCES),y,base,))
+$(eval $(call USERLAND_PACKAGE,fetch,fetch,*,y,network,$(FETCH_SOURCES),y,base,base/fetch,base/libssl-compat base/libcrypto-compat,,program,,,bin))
```

2. `mk/userland.mk`: 並列の make で fetch の package が libssl-compat・libcrypto-compat を同時に作りに行かないよう、mailer・ssh と同じ順の依存を足す。

```diff
@@ -105,6 +105,7 @@
 $(BUILD)/packages/fidoctl/.stamp $(BUILD)/packages/ssh/.stamp $(BUILD)/packages/libssl-compat/.stamp: $(BUILD)/packages/libcrypto-compat/.stamp
 $(BUILD)/packages/passkey-fido2/.stamp: $(BUILD)/packages/passkey/.stamp $(BUILD)/packages/libcrypto-compat/.stamp
+$(BUILD)/packages/fetch/.stamp: $(BUILD)/packages/libssl-compat/.stamp $(BUILD)/packages/libcrypto-compat/.stamp
 $(BUILD)/packages/noct/.stamp: $(BUILD)/packages/libegl/.stamp $(BUILD)/packages/libglesv2/.stamp
```

3. `mk/rootfs.mk`: libcurl.so の link は curl を選んだ時だけ作る（推し）。消すだけの案（`/usr/lib/libcurl.so=libcurl.so.4` の行を除き、前の行の `\` を取る）も同じ効果だが、curl を選ぶ config で link が無くなる。

```diff
@@ -76,7 +76,7 @@ ROOTFS_DYNAMIC_FILES := \
 ROOTFS_PACKAGE_LINKS ?= \
 	/usr/bin/noct=/bin/noct \
 	/usr/lib/libz.so=libz.so.1 \
-	/usr/lib/libcurl.so=libcurl.so.4
+	$(if $(filter curl,$(CONFIG_USERLAND_PROGRAMS)),/usr/lib/libcurl.so=libcurl.so.4)
```

- 気づいた事（この Queue の外）: `/usr/lib/libz.so=libz.so.1` も zlib を選ばない既定の image では行き先が無い（同じ形の条件にできる）。curl・zlib の package の `PACKAGE_LINKS` は今の rootfs.mk が読まない。

## 確かめ

| 確かめ | コマンド | 結果 |
| --- | --- | --- |
| host 試験 | `sh plan/ws343/tests/run-fetch-https.sh` | PASS（18 cases）: 長さ・chunked・close_notify のある終わりまでの body、close_notify の無い終わりの失敗、絶対・相対の path の redirect、http→https の redirect、redirect の loop、CA file 無し・自己署名・期限切れ・別の名前の失敗、TLS 1.2、address literal、plain の http、404、読めない CA file、標準出力 |
| 3 arch の build（案 1・2・3 を作業の copy に当てて） | `make -j4 CONFIGMK=config/config.<p>.mk build/<p>/packages/fetch/.stamp`（p = x86_64・rpi5・vf2） | 3 つとも exit 0、log の warning 0。`build/<p>/bin/fetch` の NEEDED は libssl-compat.so・libcrypto-compat.so・libm.so・libc.so、Machine は X86-64・AArch64・RISC-V |
| QEMU（x86_64、既定の image、公開の site） | `sh plan/ws343/tests/run-fetch-qemu.sh` | PASS（4 cases、下の「QEMU の結果」） |

## QEMU の結果

- 2026-11-14 P8、`sh plan/ws343/tests/run-fetch-qemu.sh`（案 1〜3 を当てた作業の copy、KVM、harness の USB CDC-ECM と QEMU の user の NAT、外の network あり）: `run-fetch-qemu: PASS (4 cases)`。
  - `https://example.com/`（577 byte）、`https://www.rfc-editor.org/rfc/rfc9110.txt`（502941 byte、chunked でない長さの body）、`http://www.rfc-editor.org/rfc/rfc8446.txt`（http→https の redirect、337736 byte）: guest の fetch の取得を host に写し、host の curl の取得と SHA-256 が一致。
  - `https://self-signed.badssl.com/`: `fetch: self-signed.badssl.com: TLS handshake failed: certificate verify failed: self-signed certificate`、exit 1。
  - guest の `/etc/ssl/cert.pem`（188900 byte）・`/lib/libssl-compat.so`・`/lib/libcrypto-compat.so` がある事、`/usr/lib/libcurl.so` が無い事（案 3）を `ls` で見た。
- image は既定の x86_64 の config から firmware の package（*-firmware）だけを外した物（`config-amd64-fetch.mk`）。既定の config のままでは `plan/tools/guest/test-image.sh` が amd-navi-firmware の license の問いに答えられず止まった（`mk/platform-x86_64/firmware.mk` の `firmware-licenses` が空。この Queue の外、Q1 に報告）。image の build の log の `warning:` は 4 行で、全部 fetch・compat の外（noct の `-Wunused-but-set-global` 3 つ、`hir_opt_simd.c`・`elback.c`・`scmback.c`、と gmake の jobserver の 1 つ）。fetch・libssl-compat・libcrypto-compat の warning は 0。
- 実機は使っていない（未実施）。aarch64・riscv64 の QEMU は未実施（build だけ）。

## 残り

- mk の差分の案 1〜3 のユーザーの承認と適用（Q1）。適用までは main の fetch は link できない（main.c が SSL_* を呼ぶため）。merge は案の適用と同時にする必要がある。
- 規約の全文の見直しの Phase（p004）。
