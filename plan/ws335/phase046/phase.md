<!-- awesome-plan project=zedbsd record=ws335-p046 -->

# ws335-p046: 統合 — compat を前提にした TLS の zedBSD の image での確かめ、残りの掃除、design.md の直し

Status: 実装済み・試験 PASS、Q1 の判定待ち（2026-11-14 06:02 P3 q2069-i01、branch `agent/p3-ws335p046`、基点 main 4e8a9321bf）
Parent: [WS335](../ws.md)。設計: [p042](../phase042/phase.md)。定義: phase-runner（Opus）。依存: p043・p044・p045 の merge、makefile.diff の全部（A〜G）を Q1 が main に当てた事、`net/tls-library.h`・`tls-static.c`・`tls-static-compat.c` を Q1 が消した事（p042 §4 の最後）。

## 0. 何をするか

素の main（command line の上書き無し）で、zedBSD の browser と Mail が libssl-compat・libcrypto-compat を link して https・IMAP の TLS ができる事を QEMU で確かめ、tree に macro・名前の表・OpenSSL への落ちが残っていない事を確かめ、試験の道具と design.md を今の形に合わせる。

## 1. 読む物

- `AGENTS.md`、`plan/agents/protocol.md`、`plan/tools.md`（QEMU の解析、boot-test.sh）。
- p042・p043・p044・p045 の phase.md（結果の節を含む）。p036・p041 の phase.md（guest の試験の流し方）。
- `plan/ws335/tests/run-browser-guest.sh`・`run-mailer-guest.sh`・`config-p036-guest.mk`。
- `plan/ws335/design.md` の §2.1・§4.2・§4.4・§9 の p036 の行。

## 2. 変える物

1. `plan/ws335/tests/run-browser-guest.sh`: 5 の「libbrowser.so が開く名前」（strings）を「`/lib/libbrowser.so` の NEEDED（guest の `readelf` が無ければ host で image の写しを `readelf -d`）に `libssl-compat.so`・`libcrypto-compat.so` があり、`libssl.so`・`libcrypto.so` が無い」に。先頭の comment の `-DWB_TLS_COMPAT` と「opens」を直す。local・untrusted・site の 3 つの case は変えない。
2. `plan/ws335/tests/run-mailer-guest.sh`: 1 の make の command line から `STANDALONE_CFLAGS_mailer` を外す。2 の `.flags` の `-DML_TLS_COMPAT` の確かめを「`build/x86_64/` の mailer の NEEDED に 2 つ」に、guest の program の link に `-l:libssl-compat.so -l:libcrypto-compat.so` を足す（`-l:libkeiland.so` の前）。4 の「/bin/mailer names libssl-compat.so」も NEEDED で。先頭の comment を直す。7 つの case は変えない。
3. `plan/ws335/design.md`: §2.1 の net/tls.c の記述、§4.2（「本家と compat の切り替え」→「compat を直に呼ぶ」。U2 の A と D28 は p042 で置き換えた事を 1 行で残す）、§4.4 の 4・6、§9 の p036 の行の注、§3 に D29（「TLS を使う program（libbrowser・mailer）は compat の header を include し、zedBSD は .so を link、Windows は .a を static link。名前で開く表・macro・OpenSSL への落ちは無い（2026-11-13 ユーザーの決定、p042）」）。
4. 他の WS の記録の写しの注（問い Q5 が A の時）: `plan/ws074/design.md:270-275`（D2: OpenSSL を dlopen）と `plan/ws334/design.md:317・726`（`wb_os_trust_roots`・S-A）に「2026-11-13 WS335 p042 で compat を直に link する形に置き換えた」の注を 1 行ずつ。
5. 消す候補の一覧を結果に（担当は消さない）: `plan/ws335/tests/config-p036.mk`（今の config.x86_64.mk と同じ）、`tls-wine-compat.sh`（差分 B の前の写しの mode）、`plan/ws335/phase036/libbrowser-Makefile.diff`・`browser-win32-Makefile.diff`・`defaults.diff`・`mailer-Makefile.diff`（当てた・置き換えた差分）、p036 の記録の旧い試験（`run-image.sh`・`run-guest.sh`・`config.mk`・`config-guest.mk`・`build-smoke.sh`・`crypto-smoke.c`、Q1 が既に決めていれば省く）。`plan/ws074/tests/guest-build.sh`（旧い build の形で engine を link、書き換えの後は compat も要る）、`build/ws335-nettls-host-compat`。WS335 の完了の後も使う試験（`run-nettls.sh`・`run-mailer-tls.sh`・`run-browser-guest.sh`・`run-mailer-guest.sh`・`run-unit.sh` ほか）を `plan/tools/` へ移すかの案（AGENTS.md §5）。

## 3. 確かめの項目

| # | 確かめ | 期待 |
| --- | --- | --- |
| 1 | `git grep -n 'WB_TLS_COMPAT\|WB_TLS_STATIC\|ML_TLS_COMPAT\|tls-static\|tls-library\|tls_static_library' -- userland apps mk Makefile include` | 0 件 |
| 2 | `git grep -n '"libssl\|"libcrypto\|libssl\.so\.3\|libcrypto\.so\.3' -- userland apps \| grep -v '^userland/packages/'` | 0 件（OpenSSL の package の外に OpenSSL の名前を開く所が無い） |
| 3 | `make CONFIGMK=config/config.x86_64.mk BUILD_JOBS=4 build/x86_64/packages/libbrowser/.stamp build/x86_64/packages/mailer/.stamp build/x86_64/packages/browser/.stamp`（素） | warning 0、libbrowser.so・mailer の NEEDED に 2 つ |
| 4 | 同じを aarch64（`config/config.rpi4.mk`）・riscv64（`config/config.vf2.mk`） | warning 0、NEEDED に 2 つ（重要な platform、AGENTS.md §9） |
| 5 | `sh plan/ws335/tests/run-mailer-guest.sh`（image を作る） | `run-mailer-guest: PASS (7 cases)` |
| 6 | 続けて同じ image で `sh plan/ws335/tests/run-browser-guest.sh`（run-browser-guest.sh は image を作らない。image の mtime が 5 の build の後である事を確かめて結果に書く） | `run-browser-guest: PASS`（local・untrusted・site、PNG を目で確かめる、libbrowser.so の NEEDED） |
| 7 | `sh plan/ws335/tests/run-nettls.sh`・`run-mailer-tls.sh`・`check-exports.sh <2 つの .so>` | PASS |
| 8 | `plan/tools/boot-test.sh` で x86_64 の image（graphical）が login prompt まで | 起動の確かめ（browser・mailer の .so の依存で desktop が落ちない事の粗い確かめ） |

image の build は `flock /tmp/zedbsd-image-build.lock`。firmware の許諾には答えない（p036 のとおり一覧から `*-firmware` を外す）。QEMU の名前は `ws335-p046`、log で判定しない（guest の program の最後の行と serial の対話、PNG）。

## 4. 注意

- この Phase は source を変えない。code の不具合を見つけたら直さずに Q1 に返す（直しの Phase を足す）。
- 実機は使わない（Q1 が割り当てた時だけ）。

## 5. 終わりの条件

- §3 が全部期待どおり。commit: `git commit -m WIP -- plan/ws335/tests/run-browser-guest.sh plan/ws335/tests/run-mailer-guest.sh plan/ws335/design.md plan/ws335/phase046/phase.md plan/ws335/ws.md`（4 を直したら `plan/ws074/design.md plan/ws334/design.md` も）。push しない。
- 結果の節: コマンドと最後の行、PNG の path、消す候補の一覧と理由、未実施（i386・pc9821 は参考の platform で確かめない、実機）。

## 結果

2026-11-14 P3（q2069-i01）。toolchain は自分の `build/` に `make CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached`（release の cache は s-ws335-p044 の `build/releases` の写し）、sysroot は各 platform で `make CONFIGMK=<cfg> sysroot`。source は変えていない。

### 変えた物（commit）

- 88515375f3: `plan/ws335/design.md`（§2.1 に今の形の注、§3 に D29、D28 に置き換えの注、§4.2 を「compat を直に呼ぶ」に書き直し（U2 の A と D28 の印を p042 で置き換えた事を 1 行）、§4.4 の 4・6、§8 の p033 の行の注、§9 の p036 の行の注）、`plan/ws074/design.md`（TLS の dlopen の項に注 1 行）、`plan/ws334/design.md`（`wb_os_trust_roots` の行と S-A に注 1 行ずつ）。
- 37a0b614b0: `plan/ws335/tests/run-mailer-guest.sh`: make の `STANDALONE_CFLAGS_mailer`（`-DML_TLS_COMPAT`）を外した。`.flags` の macro の確かめを `build/x86_64/bin/mailer` の NEEDED（readelf -d）に、guest の program の link に `-l:libssl-compat.so -l:libcrypto-compat.so`、guest の strings を「`/bin/mailer` の cksum が build の file と同じ」に（guest に readelf が無いので、同じ file である事で host の readelf を guest の file の NEEDED とする）。名前の case を 2 つ足した（下）。QEMU の名前を `ws335-mailer-guest` に。
- 03e7cfe847: `plan/ws335/tests/run-browser-guest.sh`: 5 を「guest の `/lib/libbrowser.so` の cksum が `build/x86_64/dynamic/libbrowser.so` と同じ、その NEEDED に 2 つがあり OpenSSL が無い」に。名前の case を 2 つ足した（下）。先頭の comment（`-DWB_TLS_COMPAT`・opens）を直した。QEMU の名前を `ws335-browser-guest` に。

### 名前（DNS）の照合と SNI の試験（Q1 の追記の 2）

- browser（guest）: 2 つ目の `s_server`（PORT+1）は既定の証明書が `ws335-default.test`、`-servername ws335-name.test -cert2` で SNI が `ws335-name.test` の時だけ `ws335-name.test` の証明書（両方とも試験の CA）。guest の `/etc/hosts` に `10.0.2.2 ws335-name.test ws335-other.test` を足し（disk は snapshot）、名前は libc の resolver の `/etc/hosts` で引く。
  `name`（`https://ws335-name.test:<port2>/`）: 描けた = SNI が出て名前が合った（SNI が無ければ default の証明書で hostname mismatch になる）。`othername`（`https://ws335-other.test:<port2>/`）: `certificate verify failed: hostname mismatch` で拒んだ = 名前を照合している（同じ CA なので名前だけが理由）。
- mailer（guest）: `CA-signed, the certificate SNI picks`（既定の証明書は CA の `mail.invalid`、SNI `localhost` の時だけ `localhost` の証明書）が ok、`CA-signed, another name`（CA の `mail.invalid` だけ、CA file あり）が `ML_ERROR_UNTRUSTED` で理由 `hostname mismatch`、指紋が一致。
- host の `run-nettls.sh` は今も 127.0.0.1 だけ（address の道）。名前の道は上の guest の 2 本と、公開の site（`site`）で通る。

### §3 の確かめ

| # | コマンド | 結果 |
| --- | --- | --- |
| 1 | `git grep -n 'WB_TLS_COMPAT\|WB_TLS_STATIC\|ML_TLS_COMPAT\|tls-static\|tls-library\|tls_static_library' -- userland apps mk Makefile include` | 0 件 |
| 2 | `git grep -n '"libssl\|"libcrypto\|libssl\.so\.3\|libcrypto\.so\.3' -- userland apps \| grep -v '^userland/packages/'` | 0 件 |
| 3 | `make CONFIGMK=config/config.x86_64.mk BUILD_JOBS=4 -j4 build/x86_64/packages/{libbrowser,mailer,browser}/.stamp` | rc=0、compiler の warning 0（`build/p046/x86_64.log`）。`build/x86_64/bin/mailer`・`dynamic/libbrowser.so` の NEEDED に `libssl-compat.so`・`libcrypto-compat.so`、`libssl.so`・`libcrypto.so` 無し |
| 4 | 同じを `config/config.rpi4.mk`（build/rpi4、AArch64）・`config/config.vf2.mk`（build/vf2、RISC-V） | 両方 rc=0、warning 0（各 528 の CC・LD）、mailer・libbrowser.so の NEEDED に 2 つ |
| 5 | `BUILD_JOBS=4 sh plan/ws335/tests/run-mailer-guest.sh`（image を作る） | `run-mailer-guest: PASS (9 cases)`（7 つの元の case と名前の 2 case）。image の build の warning は noct の外 0（noct 3）。image は `build/x86_64/hdd-image.img` 2026-11-14 05:37:37 |
| 6 | 続けて `sh plan/ws335/tests/run-browser-guest.sh`（同じ image、05:37:37 の build の後に起動） | `run-browser-guest: PASS`。local・name・site の PNG を目で確かめた（local・name は試験の page、site は example.com の page。site の一部の言語の文字が豆腐なのは font の範囲で TLS ではない）。untrusted は `unable to get local issuer certificate`、othername は `hostname mismatch` |
| 7 | `sh plan/ws335/tests/run-nettls.sh`、`run-mailer-tls.sh`、`check-exports.sh build/x86_64/dynamic/libcrypto-compat.so build/x86_64/dynamic/libssl-compat.so` | `run-nettls: PASS (http 15 cases, loader 11 cases, ...)`、`run-mailer-tls: PASS (8 cases)`、`check-exports: PASS`（libcrypto 公開 79・cryc_ 190、libssl 公開 23） |
| 8 | `make CONFIGMK=config/config.x86_64.mk BUILD_JOBS=4 CONFIG_USERLAND_PROGRAMS=<既定から firmware を除く 241> disk`（`login=graphical`）→ `OUTPUT=build/p046/boot-test BOOT_TIMEOUT=300 plan/tools/boot-test.sh build/x86_64/hdd-image.img` | `boot-test: PASS`（login prompt）。ただし QEMU の std VGA では greeter が「no greeter or no display」で終わり console の login に替わった。desktop（compositor・greeter）の描画は確かめていない |

成果物（worktree の `build/`、commit しない）: `build/p046/*.log`・`*.out`、`build/tmp/ws335-browser-guest/{local,name,site,screen}.png`、`build/tmp/ws335-mailer-guest/case-*.clean.txt`、`build/p046/boot-test/login.png`。

### 未実施

- i386・pc9821（参考の platform、AGENTS.md §9）、実機、arm64・riscv64 の QEMU での TLS（build と NEEDED だけ）、Windows（p045 の tls-wine の後に変えていない）、graphical の desktop の上の browser・Mail の起動（QEMU の std VGA では greeter が出ない）。

### 消す候補（担当は消していない。Q1 へ）

- `plan/ws335/tests/config-p036.mk`: 今の `config/config.x86_64.mk` と同じで、comment が消えた `-DWB_TLS_COMPAT` を前提。どこからも使われない。
- `plan/ws335/phase036/libbrowser-Makefile.diff`・`browser-win32-Makefile.diff`・`defaults.diff`・`mailer-Makefile.diff`: 当てた・p042 で置き換えた差分。
- p036 以前の旧い build の形の試験: `plan/ws335/tests/run-image.sh`・`config.mk`・`config-guest.mk`・`build-smoke.sh`・`crypto-smoke.c`（2026-11-06 の build の作り直しの前の形）。`run-guest.sh`（zedBSD の interop、`run-interop.sh --guest` を流す）は旧い `BUILD=` の形で今は動かない見込み（未確かめ）。interop を zedBSD でも回帰に残すなら今の形に直す、残さないなら `run-interop.sh` の comment の「run-guest.sh drives it」と一緒に消す（Q1 の判断）。
- `plan/ws074/tests/guest-build.sh`: 旧い build の形で engine を link し、compat を link しない。
- `build/ws335-nettls-host-compat`（main の checkout ではなく、作った担当の worktree にあれば）。`tls-wine-compat.sh` は既に無い（p045 で Q1 が消した）。
- WS335 の完了の後も使う試験を `plan/tools/` へ移す案（AGENTS.md §5）: `run-nettls.sh`・`run-mailer-tls.sh`・`run-browser-guest.sh`・`run-mailer-guest.sh`（と `config-p036-guest.mk`・`mailer-tls.c`）・`run-unit.sh`・`run-interop.sh`・`run-limbo.sh`・`check-exports.sh`・`tls-client.c`、`plan/tools/tls/`（既にある `host-compat-libs.sh`）の下に `ws335` の名を外して。

### p047 への申し送り

- source は変えていないので p047 の範囲（p043〜p045 の C）はそのまま。p047 の確かめに `run-mailer-guest.sh`・`run-browser-guest.sh` は要らない（source の書き方だけなら host の試験と build で足りる）が、動作に触れたら 2 本を流す（初めの image の build は数十分）。
- BUG-430 の注意: 既定の `build/x86_64` で image を作る。image の build は `flock /tmp/zedbsd-image-build.lock`。

## Q1 の追記（2026-11-13）

- `plan/ws335/tests/run-mailer-guest.sh` はまだ `-DML_TLS_COMPAT` を前提にしている（2・7・63・84〜85 行、`.flags` の確かめ）。p044 の後の mailer に合わせて直してから流す。
- p043 の報告: host の http の試験は全部 127.0.0.1 に繋ぐので、名前（DNS）の照合（`SSL_set1_host`・SNI）の道を通らない。名前で繋ぐ試験を guest か host に足す。
