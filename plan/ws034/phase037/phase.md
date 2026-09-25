<!-- awesome-plan project=zedbsd record=ws034p037 -->

# ws034-p037: packages の置き場所を `/usr` へ揃える

Phase ID: `ws034-p037`
Parent: [WS034](../ws.md)
Status: **cleared**（q348-i01、2026-09-24）
Phase disposition: normal
Queue: q348（q348-i01）
実行: メインセッション

## 規則（`Makefile` の `ZEDBSD_PACKAGE_BINDIR` の注記に書いた）

package が持ち込むものはすべて `/usr` の下: command は `/usr/bin`、daemon は `/usr/sbin` か `/usr/libexec`、
library は `/usr/lib`、header は `/usr/include`、data と license は `/usr/share`。設定だけが `/etc`。
`/bin`・`/sbin`・`/lib` は base system のもの。runtime loader は `/lib`、次に `/usr/lib` を探す。

## 調べた結果と直したもの

`userland/packages/*/*/Makefile` の `--file` の行き先を全部見た。`/usr` と `/etc` 以外は 2 つだけだった。

| package | 以前 | 以後 |
| --- | --- | --- |
| openssl | `/lib/libcrypto.so`、`/lib/libssl.so`（「loader は /lib しか探さない」という古い注記つき） | `/usr/lib/` |
| remacs | 辞書 `/home/skkjisyo.dic`（`/home` は利用者の場所） | `/usr/share/remacs/skkjisyo.dic` |

remacs は bundled の辞書を環境変数 `REMACS_SKK_DICT` だけで探す（remacs は別 repository の upstream で、ここでは変えない）。
kernel が init に渡す既定の環境（`src/kern/exec.c` の `process_spawn_init`）の値を新しい場所へ変えた。

## 検証（`plan/ws034/tests/config-amd64-usr.mk`: SSH ハーネスの image ＋ zlib・expat・ca-certificates・curl・remacs）

| 検証 | 結果 |
| --- | --- |
| rootfs の `/lib` に ssl・crypto が無い、`/usr/lib/libssl.so`・`libcrypto.so`・`/usr/share/remacs/skkjisyo.dic` がある | OK |
| sshd（`libcrypto.so` を `/usr/lib` から読む）へ SSH で入れる | OK（ハーネスの `wait` が通る） |
| `openssl version` | OpenSSL 3.5.8 |
| curl の HTTP（1 MiB、cksum 一致）と HTTPS（`https://example.com/` が 200、CA の検証つき） | OK |
| amd64・pcat・pc98 の CI kernel | warning 0 |

## 残り

- `REMACS_SKK_DICT` は init の環境にだけあるので、**SSH の session では空**（以前から同じ）。remacs が既定の場所を持つか、
  login の環境で渡すのがよい。remacs の側の変更は upstream の判断なので記録だけにした。
