<!-- awesome-plan project=zedbsd record=ws335-p038 -->

# ws335-p038: x509-limbo の全件の照合（path validation の適合）

Status: cleared（2026-11-02 11:50 Q1）。前: 実装済み・試験 PASS（2026-11-02 P7。cleared は Q1）。前: in-progress（2026-11-02 10:10 P7）。前: planned（2026-11-01 p001 が design-review の指摘 23・27 を受けて p020 から分けた。依存: p020）
Parent: [WS335](../ws.md)。担当: Opus（違いの理由の判断が要る）。

- `plan/ws335/tests/unit/test-limbo.c`: `WS335_VECTORS/rec/limbo.vec` の SERVER の 9,792 件を cryc_chain_verify（store に trusted、peer に leaf と untrusted、params に time・peer の名前・max_depth）で流し、期待（SUCCESS・FAILURE）との一致の数を数える。
- 結果の節: 一致の数と、違った件を id の前置き（rfc5280・webpki・bettertls・pathlen・pathological・cve・rfc9881・crl・online・invalid）ごとに数え、理由で分類した表。理由は design.md の決定（D11 の policy の処理なし、D12 の CN なし、trust anchor の basicConstraints と pathLen を見ない、CRL を見ない）か、cryc の欠陥か。
- 欠陥は直す（cryc-chain.c・cryc-name.c・cryc-x509.c。header の変更が要れば Q1 に返す）。trust anchor の制約（limbo の `rfc5280::root-non-critical-basic-constraints` ほか）を見るかは、件数を見てから Q1 に案を返す。
- 終わりの条件: 理由の無い違いが 0。`run-unit.sh limbo all` と `run-unit.sh --win32 limbo`（Windows の build でも同じ件数）が PASS（test-limbo は理由付きの違いを数えて PASS にする: 違いの id の表を試験の中の data として持つ）。

## 結果

2026-11-02（host の `date`、照合が揃ったのは 10:59 JST）P7（Opus、q1929-i01）。worktree p7-ws335e（main a6a0be590 から）。

作った物: `plan/ws335/tests/unit/test-limbo.c`（新規）。直した物: `plan/ws335/tests/unit/test-chain.c` の limbo の表の理由の語と、その説明の comment だけ（下の分類に揃えた。id・件数・check の数は変えていない）。library（cryc-chain.c・cryc-name.c・cryc-x509.c）・header・試験の道具は変えていない。**cryc の欠陥は見つからなかった**。

test-limbo の形: `limbo.vec` の SERVER の全 9,792 件を、trusted を store（parse できない物は入らない）、peer と untrusted を server の list、case の時刻・名前（`cryc_name_host_normalize` を通す）か address・深さ（limbo は中間の数、header は全部の数なので +2、上限 10。features の max-chain-depth の 0 は 2）で `cryc_chain_verify` に渡し、OK か否かを期待と比べる。leaf が parse できない件は検証の前の拒否として FAILURE に数える。違った件は試験の中の表（59 件、id の順、理由の語つき）に無ければ FAIL、表の件が違わなくなっても FAIL、理由の語が一覧に無くても・表が id の順でなくても FAIL。`WS335_LIMBO_TRACE` を付けると全件の code と depth、store に入らない・parse できない certificate を印字する（解析用）。

server の list の扱い（p020 の test-chain と違う点）: header は server の list を 16 枚（CRYC_CHAIN_MAX_PEER）までとするので、untrusted が 15 枚を越える 24 件は、まず leaf から名前（`cryc_name_equal`）で上へ辿れる certificate だけに減らし（順は保つ。名前で辿れない certificate は候補にならないので結果を変えない）、それでも越える分を切る。p020 と同じく先頭の 15 枚で切ると、bettertls の pathbuilding の tc61〜tc80（untrusted 35 枚）は必要な中間が 16 枚目より後にあり、SUCCESS の 8 件が UNABLE_TO_GET_ISSUER になり、FAILURE の 12 件も path の組み立てを試さずに一致していた（名前の graph で確かめた: 先頭の 15 枚では trust anchor に届かず、全部なら中間 4 枚で届く）。減らした後は 20 件とも path の組み立てを経て期待と一致する。切ったのは pathological の 100 枚の 4 件だけ（どれも FAILURE で一致）。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`、各 command に `ulimit -f 65536` と `timeout`）は下の「試験」の表。

件数: 9,792 件（期待 SUCCESS 930 件）。**期待と一致 9,733 件、理由つきの違い 59 件、理由の無い違い 0 件**。SUCCESS の期待の 924 件が有効。parse できない leaf 7 件（全部 FAILURE の期待）。posix と win32 で同じ件数。

| id の前置き | 件 | 一致 | 理由つきの違い |
| --- | --- | --- | --- |
| bettertls | 9,572 | 9,572 | 0 |
| rfc5280 | 99 | 80 | 19 |
| webpki | 56 | 31 | 25 |
| crl | 17 | 7 | 10 |
| online | 14 | 14 | 0 |
| pathlen | 13 | 11 | 2 |
| pathological | 11 | 11 | 0 |
| rfc9881 | 6 | 3 | 3 |
| cve | 3 | 3 | 0 |
| invalid | 1 | 1 | 0 |

違いの理由（どれも design.md・header の決定で、cryc の欠陥ではない）:

| 理由 | 件 | id と根拠 |
| --- | --- | --- |
| issuance（RFC 5280 が発行する CA に課す「conforming CAs MUST」の規則。6.1 の path validation は見ない） | 11 | `rfc5280::aki::{critical-aki, leaf-missing-aki, intermediate-missing-aki, cross-signed-root-missing-aki}`・`rfc5280::ski::{root-missing-ski, intermediate-missing-ski}`・`rfc5280::root-non-critical-basic-constraints`（中間の BC の critical も見ないので anchor 固有でない）・`rfc5280::pc::ica-noncritical-pc`（policyConstraints の critical の規則。policy を処理しても path の失敗にはならないので p020 の no-policy から改めた）・`rfc5280::san::noncritical-with-empty-subject`・`rfc5280::leaf-ku-keycertsign`（cA FALSE の leaf は中間に使えば INVALID_CA）・`rfc5280::serial::zero` |
| cabf（CA/Browser Forum の Baseline Requirements・webpki の profile。RFC 5280 には無い） | 23 | `webpki::aki::root-with-aki-*` の 5 件（root の AKI の中身）、`webpki::cn::*` の 9 件（CN の中身。D12 で CN は host 名に使わない）、`webpki::eku::{ee-anyeku, ee-critical-eku, ee-without-eku, root-has-eku}`、`webpki::ee-basicconstraints-ca`、`webpki::forbidden-rsa-key-not-divisible-by-8-in-leaf`・`webpki::forbidden-rsa-not-divisible-by-8-in-root`（2052 bit。中間でも 8 の倍数は見ないので anchor 固有でない）、`webpki::malformed-aia`（AIA は読まない）、`webpki::san::san-critical-with-nonempty-subject` |
| no-crl（失効を見ない、D13） | 10 | `crl::*` の期待 FAILURE の 10 件（期待 SUCCESS の 7 件は一致） |
| name（cryc-name.h の定め） | 4 | `rfc5280::nc::invalid-dnsname-leading-period`（header は先頭の dot を「その下の名前」と定める）、`rfc5280::san::underscore-dns`（header は `_` を許す）、`webpki::san::public-suffix-multi-label-wildcard-san`・`...-private-namespace-wildcard-san`（public suffix の表を見ない） |
| no-mldsa（ML-DSA は D9 の範囲の外） | 3 | `rfc9881::ml-dsa-{44,65,87}`（期待 SUCCESS、こちらは UNSUPPORTED_ALGORITHM。期待 FAILURE の 3 件も同じ code で一致） |
| anchor（trust anchor の中身はそのまま信頼する、6.1.1 (d)・cryc-chain.h。同じ欠点の中間なら拒む物だけ） | 2 | `rfc5280::unknown-critical-extension-root`・`rfc5280::root-missing-basic-constraints` |
| key-usage（leaf の KU に digitalSignature が無いと拒む、header） | 2 | `rfc5280::ca-as-leaf`・`pathlen::validation-ignores-pathlen-in-leaf`（どちらも leaf が keyCertSign だけの CA。期待 SUCCESS） |
| nc-leaf（leaf 自身の nameConstraints は何も制約しない、6.1.4 (g) は CA の物だけ） | 2 | `rfc5280::nc::not-allowed-in-ee-{noncritical,critical}` |
| depth（case は self-issued の中間を深さに数えない、header は全部） | 1 | `pathlen::max-chain-depth-1-self-issued` |
| nc-noncritical（critical でない nameConstraints も処理する、p017 の Q1 の判断） | 1 | `rfc5280::nc::permitted-dns-match-noncritical` |

p020 の 46 件はそのまま違いとして残り、理由の語だけを改めた（profile 26 と anchor の 2 件（root の BC の critical、root の RSA 2052 bit）を issuance 11・cabf 23 に分け直し、no-policy の 1 件を issuance に、decided の 3 件を nc-noncritical 1・nc-leaf 2 に）。新しい 13 件は crl 10・rfc9881 3。

一致した件の code も読んだ（期待 FAILURE の件が、別の理由で偶然 FAILURE になっていないか）:

- bettertls: NAME_CONSTRAINT_VIOLATION 5,322、HOSTNAME_MISMATCH 1,496、IP_ADDRESS_MISMATCH 1,343、MALFORMED 516（leaf の SAN の符号化が壊れた件で、名前の check の EINVAL）、pathbuilding の 30 件（INVALID_CA 12・EXPIRED 6・INVALID_PURPOSE 6・UNSUPPORTED_ALGORITHM 6）。
- `rfc5280::ica-ku-keycertsign`・`rfc5280::intermediate-ca-missing-basic-constraints` は case に中間が入っていない（limbo.json の untrusted_intermediates が空）ので UNABLE_TO_GET_ISSUER で一致する（狙いの KU・BC の検査には届かない。同じ検査は test-chain が自前の証明書で確かめている）。`rfc5280::ski::critical-ski` は leaf の AKI が root の SKI と違い候補にならず UNABLE_TO_GET_ISSUER。`rfc5280::serial::negative` は負の serial ではなく知らない critical の拡張で拒む。`rfc5280::root-inconsistent-ca-extensions`（KU の bit が全部 0 の root）・nc の空の subtree の 4 件・`invalid::invalid-issuer-key` は parse で拒まれ（EINVAL）store・list に入らない。どれも欠陥ではない。

Q1 に返す案（header の変更か製品の判断が要る。この Phase では変えていない）:

1. trust anchor の制約を見るか（phase.md の求め）。anchor 固有の違いは 2 件（知らない critical の拡張、basicConstraints が無い）だけ。案 A: 今のまま（RFC 5280 6.1.1 (d)、header の定め）。案 B: anchor の知らない critical の拡張だけ拒む（cryc-chain.h の anchor の拡張を見ない旨の文を直す。Mozilla の bundle `cacert-2026-08-13.pem` の 121 枚は全部 BC が critical で知らない critical の拡張は無いので影響は無い）。anchor に BC を求めるのは Windows の ROOT の古い version 1 の root を落とし得るので勧めない。P7 の推しは B（安全側で、実の bundle に影響が無い）。header の変更なので Q1 の判断。
2. CA/Browser Forum の profile の規則（cabf 23 件）を強制するか。推しは強制しない: design D11 は RFC 5280 6.1 の web PKI の client で、BR は root program が CA に課す発行の規則。強制すると私設の CA が落ちる（例: `plan/tools/mail/run-host-mail-backend.sh` の試験の server の証明書は EKU を持たず、`webpki::eku::ee-without-eku` の規則で拒まれる）。製品の判断なので Q1（要ればユーザー）。
3. dNSName の制約の先頭の dot（`rfc5280::nc::invalid-dnsname-leading-period`）。header の今の定め（その下の名前だけ）は広く使われる読み方で、limbo は RFC 5280 の文面で不正とする。変えるなら header の変更。推しは今のまま。

試験:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh limbo`（ASan・UBSan、build を含め 372 秒、64 core で load 約 10） | `test-limbo: PASS (95464 checks)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh --win32 chain limbo`（x86_64、Wine、limbo は約 2 分 20 秒） | `test-chain: PASS (22305 checks) (win32)` / `test-limbo: PASS (95464 checks) (win32)` / `run-unit: PASS`（limbo の件数は posix と同じ） |
| `sh plan/ws335/tests/run-unit.sh all`（1471 秒） | `run-unit: PASS`（test と ct の 52 本が全部 PASS、`test-chain: PASS (22305 checks)`・`test-limbo: PASS (95464 checks)` を含む） |
| `BASE=4eae22020 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS` |
| `git diff --check` | 出力なし |

注意: posix の test-limbo は ASan の下で約 6 分かかり、run-unit.sh の 1 本ごとの `timeout 600` に近い（余裕は約 1.6 倍）。時間の内訳は測っていない（gdb で 1 回止めた所は bettertls の case の ECDSA の確かめの中だった）。重い host で切れたら、run-unit.sh の timeout を上げるか（試験の道具の変更なので Q1）、limbo だけ長くするのが要る。

わざと壊す確かめ（cryc-chain.c と test-limbo.c の写しを取って変え、流し、写しから戻した。戻った事は cmp で確かめた）: (m1) name constraints の check を外す → 理由の無い違いで FAIL、(m3) host 名の照合を常に一致にする → FAIL、(m4) 表の 1 件を消す → FAIL、(m5) 違わない id を表に足す → FAIL。(m2) 知らない critical の拡張の判定を外す形は warning で build できず数えない。

未実施: 実機・QEMU（範囲外）、aarch64 Windows の実行（build だけの道具）、limbo の CLIENT の case（converter が SERVER だけを出す）、CRL を case に渡す試験（D13 で範囲外）。
