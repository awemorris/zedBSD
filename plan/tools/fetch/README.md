# fetch の https の回帰（WS343 から移した）

base の `fetch`（`userland/base/fetch/main.c`）の http・https を、libssl-compat・libcrypto-compat と `/etc/ssl/cert.pem` の上で確かめる。fetch・libssl-compat・libcrypto-compat・ca-certificates を変えた時に流す。

| file | 役割 |
| --- | --- |
| `run-fetch-https.sh` | host の試験。fetch を host 向けに ASan・UBSan で build し、`plan/tools/tls/host-compat-libs.sh` の compat の static の archive に link する。openssl で作った CA・leaf と loopback の server（`https-server.py`）に対して 18 の場合（長さ・chunked・終わりまでの body、close_notify の有無、絶対・相対・http→https の redirect、redirect の loop、CA file 無し・自己署名・期限切れ・別の名前の拒否、TLS 1.2、address literal、plain の http、404、読めない CA file、標準出力）。body は SHA-256 で照合し、失敗は file を残さない。最後の行 `run-fetch-https: PASS (18 cases)` |
| `https-server.py` | 上の loopback の HTTP・HTTPS の server（host の Python の ssl） |
| `run-fetch-qemu.sh [BUILD]` | QEMU の試験（x86_64、KVM、harness の USB の network と QEMU の NAT）。`config-amd64-fetch.mk` の image を `plan/tools/guest/test-image.sh` で作り、`plan/tools/guest/guest.sh` で起動し、公開の https の site（example.com、rfc-editor.org、http→https の redirect）を guest で取って host の curl の取得と SHA-256 で比べ、自己署名の site（self-signed.badssl.com）を断る事を確かめる。最後の行 `run-fetch-qemu: PASS (4 cases)`、host に外の network が無ければ `run-fetch-qemu: SKIP` |
| `config-amd64-fetch.mk` | 既定の x86_64 の program から firmware の package を外した config（QEMU には要らず、license の同意を求めるため） |

流し方: repository の root で `sh plan/tools/fetch/run-fetch-https.sh`（出力は `build/tmp/fetch-https`、compat の .a は `build/tmp/fetch-compat` に残して次の run で使う）。QEMU は `GUEST_RUNTIME=build/<担当>-fetch-guest sh plan/tools/fetch/run-fetch-qemu.sh [BUILD]`（既定 `build/x86_64`、出力は `build/tmp/fetch-qemu`）。新しい worktree では先に `make CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached` と `sysroot`。
