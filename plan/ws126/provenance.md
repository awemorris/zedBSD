# WS126: CPython の provenance と license の監査

## 取得した版（2026-10-09 P1、ws126-p001）

- 3.15.0 の final はまだ公開されていない（python.org の `ftp/python/3.15.0/` には rc3 まで、2026-10-09 に確かめた）。WS の計画どおり、3.14 系の最新の bugfix 版 **3.14.8** にした。
- archive: `Python-3.14.8.tar.xz`、URL `https://www.python.org/ftp/python/3.14.8/Python-3.14.8.tar.xz`
  - size 24130644
  - SHA-256 `c2215904f02b175596dc49351585104f4bc20341e1c47378b26a2c274360ce73`
  - ROOT `Python-3.14.8`
  - `sh userland/packages/tools/archive.sh verify` で ok（rc 0）。
- 署名（Sigstore。3.14 から PGP の署名は無い、PEP 761）: `.sig`（ECDSA の署名）と `.crt`（Fulcio の証明書）を openssl で確かめた。
  - `openssl dgst -sha256 -verify <証明書の公開鍵> -signature <.sig>` は `Verified OK`。
  - 証明書の SAN は `email:hugo@python.org`（3.14 の release manager）、OIDC の issuer（1.3.6.1.4.1.57264.1.1）は `https://github.com/login/oauth`、発行は `O=sigstore.dev, CN=sigstore-intermediate`、有効期間は 2026-09-30 21:41〜21:51 GMT。
  - `.sigstore` の bundle の digest は上の SHA-256 と同じ。Rekor の log index は 3025656141。
  - 限り: Fulcio の root までの鎖と Rekor の inclusion proof は確かめていない（`sigstore` の client が host に無い）。
- SBOM: `Python-3.14.8.tar.xz.spdx.json`（python.org の公開物、取得のみ）。

## license の監査

- 本体: PSF License Version 2（`LICENSE`、`Doc/license.rst`）。文書の中の例の code は 0BSD。
- 同梱の第三者の code（`Doc/license.rst` の「Licenses and Acknowledgements for Incorporated Software」）は、どれも permissive。
  - Mersenne Twister（BSD-3）、socket の getaddrinfo（WIDE、BSD-3）、asyncore・asynchat、http.cookies、trace、uu、xmlrpc、kqueue の notice
  - SipHash24（MIT）、dtoa.c（David M. Gay、permissive）、OpenSSL（Apache-2.0、link するだけ）
  - expat（MIT、`Modules/expat`）、libffi の notice（MIT、system の libffi を使う）、zlib（zlib、system の zlib を使う）
  - cfuhash（BSD-3、tracemalloc）、libmpdec（BSD-2、`Modules/_decimal/libmpdec`）、W3C の C14N の試験（試験だけ）
  - asyncio の一部（Apache-2.0 から）、qsbr.c（FreeBSD の GUS、BSD-2）、zstd の binding（BSD-3）
  - Unicode Character Database（Unicode の license）、HACL*（MIT・Apache-2.0、`Modules/_hacl`）、mimalloc（MIT）
- 機械監査（`plan/tools/packages/audit-licenses.sh`、Python の archive だけ）: GPL の文言を含む file は 7。
  - 3 つは既知の build の補助（config.guess など）。
  - 4 つ（`LICENSE`・`Doc/license.rst`・`README.rst`・`Mac/BuildScript/resources/License.rtf`）は、「GPL と両立する」「GPL の code を含まない」と述べる license の文で、GPL の code ではない。
  - → 既知の表に足した（ws126-p001 の判定）。
- 範囲外にする物（image に入れない）: `readline`（GNU readline は GPL。zedBSD の libedit は一部の互換だけ）、`tkinter`、`_gdbm`（GPL の gdbm）、`_curses`（未調査）。
