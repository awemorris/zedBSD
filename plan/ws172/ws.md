<!-- awesome-plan project=zedbsd record=ws172 -->
# WS172: passkey の認証の枠組み（/sbin/passkey と /etc/passkey、sessiond は外部の program で認証）

Target: **ベータ3**（2026-10-08 ユーザー「WS172はベータ3に回します。」。p002 の PIN の実装は main に入ったまま、T1 の結果を受けて判定する）
Status: incomplete（2026-10-05 追加、ベータ2。p001 設計 cleared、p002 PIN の login は cleared（T1-210、2026-10-05 夜）、p003 FIDO2 は段 A（passkey-fido2）を実装、2026-10-06。WS162・WS163 を吸収）
Master: [master](../master.md)
Primary Milestone: MG006
Related: [WS161](../ws161/ws.md)（hidraw・smartcard・libpasskey）、[WS162](../ws162/ws.md)（FIDO2 の login）、[WS163](../ws163/ws.md)（PIN の login）

## 由来（ユーザー、2026-10-05 夕）

「/etc/passwd, /etc/shadowは変更せず、/etc/passkeyを導入してrootだけがアクセス可能に。sessiondは外部プログラムを呼んでログイン認証を行うように変更。/sbin/passkeyを呼び出す。passkeyコマンドは、パスワード認証、PIN認証、FIDO2認証ができる。/etc/passkeyには、アカウントとPIN、アカウントとFIDO2のID、アカウントとセキュリティチップ上のID、みたいな情報が入っている。passkeyコマンドは、将来はセキュリティチップでの認証にも対応する。セキュリティチップはMicrosoft方式もありえるし、独自のプラットフォームの方法も実装できる。これでどうでしょう。プロセス生成はだめでしょうか？」

「passkeyはbaseに起きます。OpenSSLはリリースまでに独自実装に置き換える予定なので、問題ないです。PIN の失敗の回数はsessiondがメモリ上に持てばいいです。試行のたびにファイルアクセスするのは、おそらく何らかのサイドチャネルアタックに使われます。」

## 決まったこと

- `/etc/passwd`・`/etc/shadow` は変えない。新しい `/etc/passkey`（root だけ、0600）に、account ごとの PIN の hash、FIDO2 の credential（ID と公開鍵）、将来のセキュリティチップの上の鍵の ID を置く。
- sessiond は認証を外部の program **`/sbin/passkey`**（base）に任せる（BSD Authentication と同じ考え、process の生成で良い）。passkey は password・PIN・FIDO2 を確かめ、将来はセキュリティチップ（Microsoft の方式・独自の方式）も。
- passkey は base に置く。暗号は当面 OpenSSL の libcrypto を使い、**リリースまでに独自の実装に置き換える**（master-design-policy §2.1 の例外、期限はリリース。Guardrail の例外の表に記録）。
- PIN の失敗の回数は **sessiond が memory に持つ**（試行ごとの file の読み書きをしない。side channel を避ける）。

## 設計で決めること（p001、Q1 の案を土台に）

- 秘密の渡し方: password・PIN は argv や環境変数で渡さず pipe（標準入力）、結果は終了の code か専用の fd。
- FIDO2: 機器（hidraw・smartcard）の data の解析は権限の無い子の process（sandbox）、passkey（root）は challenge の生成と libpasskey の verify.c での署名の検証だけ。
- lock の画面も sessiond の UNLOCK → passkey（compositor は PIN の file を読まない）。WS163 の mock を置き換える。
- 登録・変更（PIN の設定、FIDO2 の登録・削除）は sessiond 経由で passkey を起動して /etc/passkey に書く（passkey を setuid にしない、password の確かめも中で）。
- sessiond の memory の失敗の回数: 再起動で消えることの扱い（遅延の増加、password の後の再有効化）。
- sessiond の口: `AUTH name style`（秘密は続く行か別の fd）と `UNLOCK style`。greeter・lock の UI の方式の選び方。
- `/etc/passkey` の形式（行の形、版）。docs（docs/architecture/security.md と keiland.md の login の節）に先に書く。
- セキュリティチップは後の Phase。ユーザー（2026-10-05）「TPMは /dev/security0  みたいなインタフェースをきちんと考えて設計、実装したいです。ad hocに/dev/tpm0みたいなのはやりたくないです。世の中のセキュリティチップの、入手可能な大まかな仕様を調べて、どんなインタフェースがあればいいかをサーベイするフェーズをやります。これも計画に入れておいてください。」→ p004 の survey と p004b の設計。

## Phase

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計（上の項目）と docs、design-reviewer | planning（第 2 版、判断 P1〜P10 待ち、2026-10-05 P1） | — |
| [p002](phase002/phase.md) | `/sbin/passkey` の password・PIN と `/etc/passkey`、sessiond の外部の認証と memory の失敗の回数、lock・greeter の PIN | in-progress（実装・host 試験済み 16b4fd99・9668d420・dd30409a・b92dc626、T1 待ち） | p001 |
| [p003](phase003/phase.md) | FIDO2（hidraw・smartcard、子の sandbox と root の検証） | uncleared（2026-10-07） | p002、WS161 の p002〜 |
| p004 | **セキュリティチップの調べ（survey）**: 世の中のセキュリティチップの入手できる大まかな仕様を調べ、OS がどんな interface を持てばよいかを比べる。対象の例: TPM 2.0（TCG の仕様、firmware の TPM: Intel PTT・AMD fTPM、TIS・CRB の interface）、Microsoft Pluton、Apple の Secure Enclave（公開の資料の範囲）、Google Titan（Titan M・OpenTitan、公開の資料）、Arm の TrustZone の TEE（GlobalPlatform の TEE Client API、OP-TEE）、Android の Keystore・KeyMint の考え方、スマートカード・secure element（PIV・OpenPGP、ISO 7816）、Linux の /dev/tpmrm0 と FreeBSD の tpm の作りの良し悪し。出力: 共通の操作（鍵の生成・封印・署名・乱数・総当たりの防御・attestation など）の表と、zedBSD の汎用の口 **`/dev/security0` のような** UAPI の案（ad hoc な /dev/tpm0 にしない、TPM 以外の chip も同じ口で）。ユーザーの review | planning | — |
| p004b | `/dev/securityN` の UAPI の設計（p004 の結論から）と、TPM 2.0（5330 の PTT、QEMU の swtpm）の driver の設計、passkey の chip の方式 | planning | p004、ユーザーの review |
| p005 | OpenSSL を独自の暗号に置き換える（リリースの前） | planning | p003 |
| p006 | 全文規約の見直し（WS の終わり） | planning | p005 まで |
| [p007](phase007/phase.md) | greeter で認証の方式を選ぶ UI（PIN・Password・Hardware Key、2026-10-08 ユーザー。lock の分は WS187 p003） | planned（ベータ3） | p002（PIN）cleared、Hardware Key は p003 |
