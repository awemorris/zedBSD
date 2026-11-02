<!-- awesome-plan project=zedbsd record=ws336-p021 -->

# ws336-p021: package と amd64 の build、libcrypto-compat での確かめ

Status: in-progress（2026-11-02 20:23 P6 q1989。2026-11-02 21:15 実装済み・試験 PASS、cleared は Q1）。前: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner（Opus）
依存: p020
WS335 の待ち: **WS335 の実装**（libcrypto-compat の SHA-256・SHA-3・ML-KEM-768・X25519・Ed25519・ChaCha20・Poly1305・RAND が実装され main に入り、zedBSD の build に `libcrypto-compat.so` がある事）

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `plan/ws336/design.md` §10・§11.2
- `platform/common/userland-dynamic.mk:569〜588`（外部の library を link する規則の例）
- `Makefile:470〜503`・`:538〜551`（package の登録と installer の image）
- `userland/packages/network/openssh/`（今の package の file）
- WS335 の記録（libcrypto-compat の host の build の作り方）
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/Makefile`（ssh・sshd・ssh-keygen・sftp-server の package。menu は `tests`、既定 n。sshd は login/verify.c の source を共有）
- `platform/common/userland-dynamic.mk` の link の規則（libcrypto-compat.so）
- `userland/base/ssh/files/`（sshd_config・ssh_config・sshd-start・sshd.service）
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- `SSH_CRYPTO_INCLUDE`・`SSH_CRYPTO_LIB` で libcrypto-compat の host の build を指し、`crypto-contract.sh`・`run-unit.sh all`・`interop-host.sh`・`sshd-pair-host.sh` が PASS
- amd64 の `$(BUILD)/bin/ssh`・`sshd`・`ssh-keygen`・`sftp-server` の build（警告 0）と `check-dynamic-elf.py` の needed（libcrypto-compat.so・libc.so）

## 4. 注意

- libcrypto-compat の host の build が無ければ Q1 に返す。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての library の header、HAL・toolchain・共有の `build/`、OpenSSH の package（`userland/packages/network/openssh/`）、既定の config（`config/ci/`、p024 まで変えない）、他の WS の file。header の不備は Q1 に返す。

## 6. 確かめの command

- `sh plan/ws336/tests/crypto-contract.sh`（libcrypto-compat で）
- `sh plan/ws336/tests/run-unit.sh all`
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

2026-11-02 21:15（host の `date`）。**実装済み・試験 PASS**（cleared は Q1 が決める）。P6、worktree `p6-ws336`（branch `agent/p6-ws336`、基点 main 57bc8e37f5）。library の header・HAL・toolchain・OpenSSH の package・`config/ci/` は変えていない。WS335 の libcrypto-compat（p039 の `libcrypto-compat.so` の規則を含む）は main にあった。

### 作った物

- `userland/base/ssh/Makefile`: package 4 つ（`ssh`・`sshd`・`ssh-keygen`・`sftp-server`）。menu `tests`、既定 n、全 arch（`*`）、class basic、require `base/libcrypto-compat`、置き場所 `/usr/bin/ssh`・`/usr/sbin/sshd`・`/usr/bin/ssh-keygen`・`/usr/libexec/sftp-server`（OpenSSH と同じ path、だから既定 n で並べない）。各 program は自分の main と program の file と library の module 全部（`ssh-*.c` から program の物を除いた 14）、sshd は加えて `userland/base/login/verify.c`。sshd を選ぶと `/etc/ssh/sshd_config`（0600）・`/etc/service.d/sshd`・`/usr/libexec/sshd-start`（0755）、ssh を選ぶと `/etc/ssh/ssh_config` が image に入る（OpenSSH の package と同じ `ZEDBSD_PACKAGE_INPUTS`・`ZEDBSD_PACKAGE_FILES` の形。データの `--mode` は 0644 に固定なので）。
- `userland/base/ssh/files/`: `sshd_config`（今の image の物から rsa の HostKey を外し ed25519 だけ。`PermitRootLogin yes`・`PasswordAuthentication yes`・`Subsystem sftp /usr/libexec/sftp-server`）、`ssh_config`（今と同じ `VerifyHostKeyDNS no`）、`sshd.service`（今と同じ）、`sshd-start`（新しく書いた: 設定の有無、`/var/empty`、ed25519 の鍵が無ければ `ssh-keygen -q -A`、`sshd -t -f` で確かめてから exec。`!` の否定は zedBSD の sh で確かめていないので if/else の形）。
- `platform/common/userland-dynamic.mk`: `DYNAMIC_SSH_PROGRAM`（4 つの program の link の規則、passkey-fido2 と同じ形で `-l:libcrypto-compat.so -l:libc.so`、`check-dynamic-elf.py --needed libcrypto-compat.so --needed libc.so`）。4 つの名前を `DYNAMIC_PORTABLE_PROGRAMS`（arm64・riscv64 の generic の規則から外す一覧）に足した。`platform/amd64/vmunix.mk` の generic の規則から外す一覧（:1053）にも足した（足さないと basic の規則が同じ target を作る）。
- **passkey-fido2 の規則の修正**（WS172 の規則、Q1 の OK で p021 に入れた）: passkey-fido2 の 2 つの target-specific の行（OpenSSL の `-I` と `$(ZEDBSD_OPENSSL_STAGED)` の前提）が、他の program と共有の object（`login/verify.o`・`common/account.o`・`passkey/record.o`・`passkey/request.o`）にも掛かっていたため、sshd・login・su・sudo・passwd などを build すると OpenSSL を configure・build していた（main でも同じ）。4 つは OpenSSL の header を include しない（`grep openssl/` で確かめた。OpenSSL を include するのは `libpasskey/crypto-openssl.c` だけ）ので、`$(filter-out)` で 2 行から外した。
- 試験: `plan/ws336/tests/compat-host.sh`（新）: libcrypto-compat を host 向けに `sources.mk` の 28 file から ASan・UBSan で static library にし、`SSH_CRYPTO_INCLUDE`（`compat/` が `include/libc/compat` の directory）・`SSH_CRYPTO_LIB` を向けて crypto-contract・run-unit all・package の file（`ssh-keygen -A` を新しい root の下で、`sshd -t` に sshd_config（HostKey を root の下に移して、ed25519 の 1 行だけの事も）、ssh に ssh_config（閉じた port で「Connection refused」まで読む事）、`sh -n sshd-start` と実行の権限）・sshd-pair-host・interop-host を流す。`CRYPTO_COMPAT_SOURCE_DIR` で library の写しを使える。`build-host.sh`・`interop-host.sh`・`sshd-pair-host.sh`・`keygen-host.sh` が `SSH_CRYPTO_INCLUDE`・`SSH_CRYPTO_LIB` を受ける（既定は今までの host の OpenSSL の shim）。

### 実行した command と最後の行

- `sh plan/ws336/tests/compat-host.sh`（libcrypto-compat で）→ `contract: crypto-contract: PASS`（108 checks）、`unit: run-unit: PASS`（18 本）、`files: PASS`、`pair: sshd-pair-host: PASS`、`interop: interop-host: PASS`（161 checks、0 failed）、最後の行 `compat-host: PASS`
- `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/p021d build/p021d/bin/ssh build/p021d/bin/sshd build/p021d/bin/ssh-keygen build/p021d/bin/sftp-server`（新しい BUILD、passkey の修正の後）→ rc 0、`warning:` 0、OpenSSL の行 0、4 つとも `check-dynamic-elf.py --role application --needed libcrypto-compat.so --needed libc.so` を通る（`llvm-readelf -d` の NEEDED も libcrypto-compat.so と libc.so だけ）。worktree の sysroot は main の `build/amd64/sysroot` の複写（make は作り直さなかった）。
- passkey の修正の確かめ: 空の package の root（`ZEDBSD_EXTERNAL_WORKROOT=build/tmp/p021-emptyroot`）で `make -n` の sshd・login・su・sudo・passwd・passkey・account-admin は OpenSSL の Configure 0、passkey-fido2 だけ 1。新しい `BUILD=build/p021c` で sshd・login・su を build して OpenSSL の行 0、その後 passkey-fido2 を build して rc 0（`crypto-openssl.c` などは OpenSSL の include で compile、`check-dynamic-elf --needed libcrypto.so --needed libc.so` 通過）。`OUT=build/tmp/p021-fido2-host sh plan/ws172/tests/fido2-host-test.sh` → `fido2-host-test: PASS`。修正の前は p021 の最初の build で sshd が OpenSSL を configure・build した（OpenSSL の 15 の warning はそこから）。
- arm64・riscv64（`config-rpi4.mk`・`config-riscv64-virt.mk`）の `make -n build/.../bin/sshd`: rc 0、自前の規則で link（実際の build と警告 0 は p023）。
- host の OpenSSL のままの回帰: `sh plan/ws336/tests/run-unit.sh all` → `run-unit: PASS`、`keygen-host.sh` → `keygen-host: PASS`、`sshd-pair-host.sh` → `sshd-pair-host: PASS`。
- `BASE=0e9b68f948 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`
- `git diff --check` → 出力なし

### わざと壊す確かめ

`build/tmp/p021-mut/mut.py`（worktree の file は書き換えず、libcrypto-compat か package の file を変異ごとの新しい directory に写して 1 か所を変え、compat-host.sh の写しをその写しに向けて流す。RLIMIT_FSIZE 64 MB・RLIMIT_CPU 400 s、150 秒と RSS 6 GB で kill）。

- 10 通り全部 FAIL: libcrypto-compat の X25519 の a24（121665 → 121666）・SHA-256 の最初の K・ChaCha20 の 16 bit の回転・ML-KEM の q（3329 → 3331）は crypto-contract が FAIL（RFC 7748 6.1・FIPS 180-4・RFC 8439 2.4.2・ML-KEM の鍵の生成の失敗）。sshd_config に rsa の HostKey を足す・keyword の綴りの誤り・Subsystem の path を外す、ssh_config の keyword の綴りの誤り、sshd-start の構文の誤り・実行の権限を外す、は files が FAIL。
- 1 回目は 2 並列で流したところ、crypto-contract.sh の出力の directory が 1 つなので並列の 2 つの run の結果が混ざり、2 つが 150 秒の上限に掛かった。library の 4 通りを 1 つずつ流し直して（3 回目）各々の FAIL の行が自分の変異の物である事を確かめた。
- 逆の確かめ: `check-dynamic-elf.py` に `--needed libcrypto.so` や libc.so だけを宣言すると ssh は FAIL（DT_NEEDED の一覧が違う）。

結果は `build/tmp/p021-mut/run1.log`・`run2.log`・`run3.log` と各 `results.json`。

### 自分で決めた点

- package の名前は program の名前（`$(BUILD)/bin/<名前>` が program）、4 つとも package の path は `base/ssh`。
- 全 arch（`*`）にした（libcrypto-compat も `*`）。arm64・riscv64 は共通の規則で link される。32 bit の arch（pcat・armv7 など）の規則は無い（既定 n、p023 で）。
- host 向けの libcrypto-compat は試験の script の中で作る（WS335 の run-unit と同じ include の形）。
- `sshd-start` は rsa・ecdsa の鍵を作らない（自前の sshd は ed25519 だけ。OpenSSH の package の sshd-start とは別に書いた）。

### 未実施・残り

- zedBSD の上（QEMU）で動かす事、image に入れる事は p022（`plan/ws336/tests/config-amd64-ssh.mk`）。
- arm64・riscv64・armv7 の実際の build は p023（ここでは make -n まで）。
- 既定の config の入れ替え（openssh → 自前、menu `base`・既定 y）は p024・WS343。
- interop-host を host の OpenSSL のままで流し直していない（変えたのは crypto の flag を変数にした所だけで、既定の値は前と同じ文字列。libcrypto-compat の run では全部の行を通った）。
- 規約の全文の見直しは p025。C の source は変えていない（Makefile・shell・設定の file と試験の script）。
- 消す物（Q1 に頼む）: `build/p021/`・`build/p021c/`・`build/p021d/`（試験の build）、`build/packages/openssl/`（修正の前に sshd の build が作った OpenSSL の build と stage）、`build/amd64/packages/toolchain/`（その時に make が作った cross toolchain の環境）、`build/amd64/sysroot/`（main の複写、p022 で使うなら残す）、`build/tmp/p021-*`（mut・dry run・build の log・emptyroot・files・host・fido2-host）、`build/tmp/ssh-compat-host.run.*`・`ssh-crypto-contract.run.*` などの古い run。
