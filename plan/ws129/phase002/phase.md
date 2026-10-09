<!-- awesome-plan project=zedbsd record=ws129-p002 -->
# ws129-p002: image の license の一覧

Status: in-progress（q668・q669、P2、2026-10-04。一覧・script・host 試験・audit・G1〜G4・D1・D2 は済み、open 0。CI の config の完全な rootfs の確かめと QEMU の remacs の起動は T 待ち）
Disposition: normal
Parent: [WS129](../ws.md)
Focused goal: fg019（ベータ1）
Queue: q668（Q1 の dispatch、2026-10-04）
目安: 3〜4h

## 範囲

1. release の image（当面は `config/ci/config-amd64.mk` とデモの config の和）に入る全ての component を列挙する: 自作の source（Zlib 等、SPDX の行から）、`userland/packages/` の外部 package（openssh・openssl・curl・zlib・expat・libcxx・clang・ca-certificates・font など）、
   firmware（i915・AX211・RTL8822B）、取り込んだ表（RTL8822B の `.inc`、browser の Unicode/WHATWG）、LLVM の runtime。
2. 各 component の license の本文の場所（image の中の path と source の中の path）を確かめ、足りないものを洗い出す。
3. 一覧を生成する script（`tools/release/license-inventory.py` など、置き場所は p001 と合わせる）: rootfs の staging と package の metadata から一覧（component・版・license・本文の path）を作り、image の `/usr/share/licenses/` に一覧と本文が揃うかを検査する。
4. `plan/tools/packages/audit-licenses.sh` を走らせ、結果を記録する。GPL 系が image に入っていないことを確かめる（範囲外の例外は guardrail の記録だけ）。

## 受け入れ

一覧（`plan/ws129/licenses.md` と生成物）、script の host の試験、audit の結果、足りない本文の一覧（直すのが他の WS・package なら main に依頼）。image に本文を入れる変更は package の Makefile ごとになるので、その差分は main と調整する。

## 所有 path

`plan/ws129/`、新しい script（`tools/release/` の下）。

## 依存

なし。release の config が p004 で決まったら p006 の前に再生成する。

## 未決の判断

なし。


2026-10-02 Q1: `plan/tools/packages/audit-licenses.sh` は openssl・openssh の tarball だけを見ている（ws115-p005 で判明）。この Phase で全外部 package（glib・pcre2・libffi 以降の GTK の依存、Emacs・vim・Python を含む）に広げる。

## 実施（2026-10-04、q668、P2）

- 一覧: [licenses.md](../licenses.md)（要約・足りない本文・判断・audit）、生成物 [licenses-generated.md](../licenses-generated.md)・[licenses-index.txt](../licenses-index.txt)。
- script: `tools/release/license-inventory.py` と `tools/release/license-components.json`（44 の component、image の分は 25）。make に image の中身を聞く
  （`--eval` の断片で、選んだ package・kernel の option・`/usr/share/licenses/` の file・外部の版と archive・source）。`--rootfs` で disk の上も確かめる。
- host 試験 [license-inventory-test.sh](../tests/license-inventory-test.sh) PASS（8 件）。T1 の CI の image の rootfs（`t1-full`）に `--rootfs` で当て、入っている本文は全て在る。
- audit: `plan/tools/packages/audit-licenses.sh` を全 archive に広げた（Q1 の 2026-10-02 の記録の指示）。main の distfiles（29 archive）で all known: yes。
- 残り: G1 zedBSD の LICENSE、G2 libc の regex（TRE の BSD-2・musl の MIT）、G3 i915 の Intel の MIT、G4 libvulkan の LICENSE-PROTOCOL を image に入れる
  （各 package の Makefile、main と持ち主に依頼）。D1 remacs（GPL）はユーザーの判断待ち（Q1 が上げた）。release の config が p004 で決まったら再生成。

2026-10-04（続き）: Q1 の委任で G1〜G4 を直した（licenses.md の「足りない本文」の節）。CI の config から clang・libcxx を除いた rootfs で `--rootfs` の確かめ:
残りは D1 だけ。host 試験の期待を「既知の残り 1 件（remacs）」に直して PASS。

## D1・D2 の判断（2026-10-04 ユーザー）

「i915-oldはもう使っていないので削除です。remacsはuserland/base/emacsとしてコピーを取り込み、作者としてzlibライセンスにします。特別扱いは不要です。」→ q669（P2）で実施。

2026-10-04 q669: ユーザーの判断で D1（remacs を userland/base/emacs に取り込み zlib に）・D2（i915-old を削除）を実施（licenses.md）。kernel の build と
check-kernel-includes（`amd64 vmunix check: PASS`）、rootfs の build rc=0・新しい warning 0、`license-inventory.py --rootfs` の open 0、host 試験 PASS
（期待を open 0 に）、menuconfig の host 試験 PASS。

## ベータ2 の再生成（2026-10-09、q920、P1）

Q1 の割り当て:「p002 license の一覧を今の release の image（config）に合わせて再生成し audit（新しく入った package・firmware・intelbt 等）」。
詳細は [licenses.md](../licenses.md) の「ベータ2 の再生成」。

- `license-inventory.py --config config/release/config-amd64-beta2.mk --distfiles <main>/build/distfiles` → `26 components, 0 open items`。
  生成物を plan/ws129 に置き換えた。差: FFmpeg（LGPL、decided、build の config.h で GPL・nonfree・version3 が 0 を確かめた）・Hershey・SKK の辞書が増え、
  Inter → Mahora、Venus の宣言（libvulkan が Kei GPU の protocol になった）と expat（デモの config だけ）が減った。
- `intelbt-firmware` は release の config に無い（UAT だけ）。WS143 を出すなら足す要がある → **Q1・ユーザーの判断**。足しても `27 components, 0 open items`。
- `audit-licenses.sh`: FFmpeg を package の判定に足し、展開先を fresh-out の `build/tmp/audit-licenses` にして script の rm をやめた。
  未知は main の distfiles に残る使われない `remacs-1a724393053e.tar.gz` の README だけ（Q1 が外せば all known: yes）。
- host 試験 `sh plan/ws129/tests/license-inventory-test.sh` → PASS。
- 未実施: release の image の rootfs への `--rootfs` の当て（image の build は T1・Q1）。release の rootfs ができたら
  `license-inventory.py --config config/release/config-amd64-beta2.mk --rootfs <rootfs>` で本文の有無を確かめる。

状態: ベータ2 の分は cleared 候補（intelbt の判断と release の rootfs への `--rootfs` は残り）。
