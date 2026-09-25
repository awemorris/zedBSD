<!-- awesome-plan project=zedbsd record=ws034p017 -->

# ws034-p017: curl（libcurl を含む）

Phase ID: `ws034-p017`
Parent: [WS034](../ws.md)
Status: **cleared**（q336-i01、2026-09-23。q334-i01 は uncleared）
Phase disposition: normal
Queue: q334（q334-i01、uncleared）→ q336（q336-i01、cleared）
実行: メインセッション

## 入れたもの

| 場所 | 内容 |
| --- | --- |
| `userland/packages/network/curl/` | curl 8.22.0（署名確認済み）。CMake、OpenSSL（WS032）と zlib（p021）の stage を header・library の path で直接渡す（toolchain file が検索を sysroot に閉じるため）。libpsl・libidn2・libssh2・nghttp2・brotli・zstd・LDAP・GSSAPI は off。CA bundle は `/etc/ssl/cert.pem`（p019）。SONAME `libcurl.so.4`、symbol versioning なし。image に `/usr/bin/curl`、`/usr/lib/libcurl.so.4`（＋`libcurl.so` の link）、header、`.pc`、licence。package の依存に openssl・zlib・ca-certificates |
| `include/libc/sys/time.h` | **libc の是正**: `<sys/select.h>` を含める。POSIX はこの header に `fd_set` を求めており、curl の `multi.h` がそれに頼る |
| `include/libc/netinet/in.h` | **libc の是正**: POSIX の `IN6_IS_ADDR_*` の12個（UNSPECIFIED・LOOPBACK・MULTICAST・LINKLOCAL・SITELOCAL・V4MAPPED・V4COMPAT・MC_*）。byte だけを見るので byte order に依らない |
| `include/uapi/ioctl.h`、`src/kern/syscall.c` | **kernel の是正**: `FIONBIO`（`_IOW('f', 126, int)`）。kernel の ioctl の入口で、どの種類の file でも `O_NONBLOCK` を立てる・下ろす（`fcntl(F_SETFL)` と同じ）。OpenSSL は `FIONBIO` が無いと socket を non-blocking にする code が「必ず失敗する」分岐になり、`openssl s_server` が起動しなかった |
| `plan/ws034/tests/` | `config-amd64-curl.mk`、`in6-macros-test.py`（host の glibc の macro と19個の address で突き合わせ、PASS） |

## 検証

| 検証 | 結果 |
| --- | --- |
| `make curl`・`world`・`disk-image` | PASS。`check-dynamic-elf.py`: `libcurl.so.4` は libssl・libcrypto・libz.so.1・libc、`curl` は libcurl.so.4 と同じものを直接も link する（curl の CMake の作り方） |
| ゲストで `curl -V` | `curl 8.22.0 (zedBSD) libcurl/8.22.0 OpenSSL/3.5.8 zlib/1.3.2`、AsynchDNS・HSTS・IPv6・SSL・libz ほか |
| ゲストで `file://` | 読める |
| ゲストで HTTPS（`openssl s_server` を loopback で、host で作った試験用 CA で署名した証明書） | **TLS 1.3 の handshake が終わり、証明書が CA で検証される**（`SSL certificate verified via OpenSSL`、SAN の IP が一致）。**しかし HTTP の応答が届かず timeout** |

## なぜ uncleared か: kernel の TCP

loopback の TCP で、**1回の write が約 1 KiB（`TCP_MSS` = 1024）を超えると、相手に届かない**ように見える。

- 最初の試み: curl の ClientHello（1,534 byte。ML-KEM を含む key share が大きい）が server に届かず、
  server は `read R BLOCK` を繰り返した。
- `--curves X25519` で ClientHello を 512 byte にすると handshake は通った（server の証明書は 810 byte）。
- その後の HTTP の応答（`s_server -www` の頁は数 KiB）が届かなかった。

**原因は確かめていない**（送る側の分割、受け側の再組立て、window、loopback の経路のどれか）。
TCP を直す Phase を新たに作り（ws034-p042）、その後にこの Phase をもう一度 Queue に入れる。
curl の package 自体（build・link・TLS・CA の検証）は上のとおり動いている。

## 他に見つけたこと（範囲外、記録のみ）

- `FD_SETSIZE` が **32**（`include/uapi/select.h`、`fd_set` は 32 bit 1つ）。`select()` を使う program は
  fd 32 以上を扱えない。curl は `poll()` を使うので影響しない。

## 再実行（q336-i01、2026-09-23）: cleared

ws034-p042 で TCP を直した kernel で、同じ試験をやり直した（`evidence/rerun-q336.txt`）。

| 検証 | 結果 |
| --- | --- |
| `curl --cacert ca.pem https://127.0.0.1:4433/`（既定の ClientHello、ML-KEM を含む） | **exit 0、5,360 byte の頁**（`openssl s_server -www`） |
| CA を指定しない（既定の `/etc/ssl/cert.pem`） | exit 60（`unable to get local issuer certificate`）。試験用の CA は既定の bundle に無いので正しい。既定の bundle を読んでいることも示す |

外への HTTPS（実在の site）は、ゲストに外への network が無いので試していない。
