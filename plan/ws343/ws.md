<!-- awesome-plan project=zedbsd record=ws343 -->

# WS343 base の TLS と ssh を自前に: CI の config から OpenSSH・OpenSSL・curl・libcurl を外し、base に自前の ssh・sshd・libssl.so・libcrypto.so、fetch の https

- Master: [plan/master.md](../master.md)（2026-11-02 10:52 作成）。Primary Milestone: MG005。
- Status: incomplete（2026-11-02 20:46 p002 を先に始めた。p001 は WS335 の completed と WS336 の後）
- 目標:
  - 各 CI の config（config/ci/*.mk。ユーザーの物の config-amd64-server.mk は除き、ユーザーに確かめる）から OpenSSH・OpenSSL・curl・libcurl の package を外す。
  - 代わりに base に、自前の ssh・sshd（[WS336](../ws336/ws.md)）と、libssl.so・libcrypto.so（[WS335](../ws335/ws.md) の libssl-compat・libcrypto-compat）を入れる。
  - base の fetch が、互換の libssl・libcrypto で https を使えるようにする。
  - ca-certificates は packages に残し、自前の実装（libssl・ssh・fetch・ブラウザ）もそれを参照する。

## ユーザーの決定

- （2026-11-02 10:52）ユーザーから、上の目標の趣旨の指示を受けた（WS335 の完了の後、互換の ssh・sshd の完成と合わせて）。

- （2026-11-02 20:46）ユーザーから、passkey-fido2・fidoctl（libpasskey の crypto-openssl.c）が使う OpenSSL の関数を libcrypto-compat に足し、link を差し替えるだけで OpenSSL から外せるようにする、という趣旨の指示を受けた（Q1 が出した「libpasskey に libcrypto-compat の内部を直接呼ぶ版を足す」案ではなく）。

## 関係する WS

- [WS335](../ws335/ws.md)（libssl-compat・libcrypto-compat）、[WS336](../ws336/ws.md)（自前の ssh・sshd）、[WS334](../ws334/ws.md)（Windows 版のブラウザの切り替え）、WS074（ブラウザの net/tls.c、WS335 p036）。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws343-p001 | 調べと設計: CI の config ごとの package の依存（curl・libcurl・OpenSSL を使う program の洗い出し）、base に入れる名前（libssl.so・libcrypto.so の soname と互換の範囲）、fetch の https、ca-certificates の場所、menuconfig と MAC-T001 の試験 | planning | WS335 completed、WS336 |
| [ws343-p002](phase002/phase.md) | libcrypto-compat に passkey が使う OpenSSL の関数 16 個を足す（HMAC、EVP_PKEY_verify・_verify_init・_public_check・_fromdata・_fromdata_init・_get_octet_string_param・_Q_keygen・_CTX_set_signature_md、EVP_aes_256_cbc・EVP_CipherFinal_ex・EVP_CIPHER_CTX_set_padding、EVP_KDF_fetch・_CTX_new・_derive、OSSL_PARAM_construct_octet_string と crypto-openssl.c が使う残りの OSSL_PARAM の口）。crypto-openssl.c は変えずに link だけを libcrypto-compat に替え、passkey-fido2・fidoctl を OpenSSL 無しで build、host 試験で OpenSSL 版と同じ答え | cleared（2026-11-02 22:20 Q1、b8c4adaf6 を main に merge 11846c70f、main で test-api-passkey PASS、passkey-fido2・fidoctl の amd64 の build が OpenSSL 無しで warning 0） | WS335 p039（libcrypto-compat.so） |

## Resume point

- 2026-11-02 10:52 Q1: 作成。WS335 の completed と WS336 の ssh・sshd の完成の後に p001 を割り当てる。

## Q1 の判断（2026-11-02 22:20、p002 の merge）

- WS335 の check-skeleton の BASE を b8c4adaf6c（p002 の終わり）に移す。
- WS335 design.md §5 の公開の API の表と evp.h の範囲の note に passkey の部分集合を入れるのは WS335 p035（P1）で行う。
- libpasskey の crypto-openssl.c・crypto.h の comment（OpenSSL's libcrypto）は、OpenSSL 互換の API を呼ぶという意味で正しいので、そのままにする。
- passkey-fido2・fidoctl の実機（または WS172 の loopback の security key の image）での動作は、WS343 の後の Phase で確かめる。
