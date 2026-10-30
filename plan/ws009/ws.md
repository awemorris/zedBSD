<!-- awesome-plan project=zedbsd record=ws009 -->

# WS009: documentation

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-10-30（q1725、P7 の完了の形の案。WS の受け入れの確認は下の表）
Primary Milestone: MG001
Related Milestones: MG006, MG008, MG009
Objectives: O2, O4, O5
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい文書の要求は新しい WS として立てる。この WS は再開しない）
Target: **ベータ2**（2026-10-13 ユーザー「WS196、WS172、WS009はベータ2に入れます。」。旧: ベータ3（2026-10-08 ユーザー））
<!-- awesome-plan-current:end -->

## 目標

zedBSD の architecture、独自の仕様、HAL の境界、公開の UAPI、build、boot の流れ、init と service、対応する device を説明する
product の文書（`docs/`）を作り、今の振る舞い・互換・制限・予定を混ぜずに書き、clean な build と QEMU の boot と管理を文書から再現できるようにする。

完了の条件（原文）: WS009 is complete when every listed documentation work item is published, cross-linked, and traceable to
current headers/source/tests; build and boot instructions are reproduced in a clean environment; compatibility claims agree
with WS001; and the documentation link/structure checks pass.

## ユーザーの決定

- 2026-10-27 ユーザーの回答（クリック）「今書く」: DOC-54（/dev/gpu の UAPI の文書）を保留から外し、今書く。
- 2026-10-29 ユーザーの回答（クリック、Q1 の中継）Q-D54-1「capset の形も書く」。解釈: 範囲は `/dev/gpuN` の ABI 1、sysctl の GPU の項、
  capset id 4・version 0 の payload の形。`/dev/amdrdna0` と設計中の UAPI は書かない。
- 2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」:
  p009 を p010〜p024 に分けて実行した。

## 結果

work item の全部（DOC-00〜DOC-55）を `docs/` に公開した。

| ID | 文書 | 完了 |
| --- | --- | --- |
| DOC-00 | 情報の構成、style（`docs/style.md`）、Status の行、link の検査 | p001 |
| DOC-10・11・12 | 独自の仕様の概要、HAL と kernel の構造、POSIX/SUS と `_XOPEN_SOURCE` の profile（`docs/reference/compatibility-profile.md`） | q150（p007）、p028 で header と照合 |
| DOC-20 | `docs/howto/build-from-source.md` | p002、p026・p028 で今の main に合わせた |
| DOC-30〜34 | boot の流れ、kernel の parameter、boot の filesystem と loopback root、native UFS root、USB の試用と NVMe への install | p005・q190（p008） |
| DOC-40 | init・rc.conf・service.d・fd 3 の readiness・shutdown | p003 |
| DOC-50〜53 | `/dev/console`・`/dev/graphics`・`/dev/system`・evdev と `/dev/input` | q150・p004 |
| DOC-54 | `/dev/gpu` の UAPI（`docs/reference/gpu.md` と `gpu-*.md` の 14 file） | p010〜p024 |
| DOC-55 | networkd・`net`・dhcpc・WPA・rc.conf の network（`docs/reference/managed-wlan.md`・`network-console.md`） | q150、p025 で今の `net wifi`・`net lan`・`net startup` に書き直した |

## WS の受け入れの確認（2026-10-30 P7、q1725-i01、main 2596260f4 + 5e17e926e）

| 条件 | 確かめ | 結果 |
| --- | --- | --- |
| 全ての work item の公開 | 上の表 | 全部公開 |
| 相互の link（索引から辿れる）と構造（Status の行） | `python3 -I plan/tools/docs-check/check-docs-current.py .` | PASS（49 indexed-documents、49 status-documents、56 items。`docs/agent/` と release の本文 `docs/release/zedbsd-*.md` は対象外） |
| header・source・試験への辿り | `python3 -I plan/tools/docs-check/check-gpu-uapi-doc.py --base 4dfcce008 --examples --final`、上の check-docs-current（`net wifi` の usage、evdev、Noct・LLVM の版と cache の asset 名、variant と menu の label） | 両方 PASS（GPU: 14 files、46 index rows、98 constant rows、47 structures、20 capset rows、0 placeholders。例の compile は sysroot のある amd64 の 4。p023 では 3 arch の 12） |
| clean な環境での build と boot の再現（DOC-T20・T30・T72） | p028: main 2596260f4 の git archive から menuconfig の既定 → sysroot → native・hybrid・uefi・bios の disk-image、`plan/tools/boot-test.sh` | 4 つの image が exit 0、5 つの起動（native・hybrid・uefi の OVMF+NVMe、hybrid・bios の SeaBIOS+IDE）が画面の `login:` まで PASS |
| 互換の主張と WS001 の一致（DOC-T60） | p028: compatibility-profile.md を `include/libc/features.h`・`include/uapi/types.h`・WS001 の台帳と照合 | 11 行のうち 10 一致、1 件（`uapi_ptr_t` の macro 名）を直した。文書は完全な準拠を主張しない |
| link の検査 | `noct plan/tools/docs-check/check-relative-links.noct docs/reference docs/architecture docs/howto docs/release` | PASS（1051 links） |

## 制限・移管

- **toolchain の 2 手は再現していない**: p028 は共有の LLVM・host Noct（`build/llvm`・`build/NoctLang`）を読み取り専用で使った
  （2026-10-30 Q1 の判断 (a)）。`make toolchain-cache`・`make -j16 toolchain` を含む完全な再現は将来の CI か release の UAT の扱い。
- 再現していない否定の側: `uefi`・`native` が SeaBIOS で、`bios` が OVMF で起動しないこと。`make run` そのもの（boot-test.sh は同じ firmware と bus の組）。
  i386 PC/AT・PC-98 は過去の WS010 の証拠のまま。
- `docs` 全体の link の検査は、固定版の `docs/agent/awesome-plan/README.md` の `img/1.png` ほか 6 件の欠けで止まる（触らない、2026-10-29 Q1）。
- managed-wlan.md（p025）は usage と source の照合だけで、guest での実行は確かめていない。
- amd64 の `zedbsd.cfg`・`zedbsd-native-uefi.cfg` の `video=640x480` は kernel の token にもなり（UEFI の loader は読むが消さない、BIOS の loader は読まない）、
  kernel の parameter の表（`src/kern/boot.c`）に `video` が無いので、どの起動でも `boot: ignored 1 unknown parameter; first=video` が出る
  （試験の script の物ではない）。文書（kernel-boot-parameters.md 9 節）にそう書いた。code を直す（kernel が `video` を知る、か loader が消す）かは別の WS の判断。
- 新しい公開の UAPI の文書は、その producer の WS の Phase として書いてよい（`plan/tools/docs-check/` の照合を流す）。
- WS012 の service の console の文書（`docs/reference/init-services.md`）は WS012 の証拠と照らす（YAML v1 の rc.conf と service.d の割当の形の区別、ZSV1、
  起動・停止と永続の enable の区別、永続の後の reload の失敗の扱い）。
- 回帰の道具: `plan/tools/docs-check/`（`check-relative-links.noct`・`check-docs-current.py`・`check-gpu-uapi-doc.py`・`gpu-doc-rules.md`）、
  [plan/tools.md](../tools.md) に登録した。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws009-p001 | 文書の階層と規則、Noct の相対 link の検査 | Complete |
| ws009-p002 | toolchain・並列の image の build・amd64 QEMU の印・診断・link | Complete |
| ws009-p003 | init と service の設定・readiness・監視・shutdown | Complete |
| ws009-p004 | 入力（複数の source・resync・console の購読・detach） | Complete（q046） |
| ws009-p005 | kernel の parameter と x86 の 4 つの loader の経路 | Complete（q046） |
| ws009-p006 | WLAN・USB HID・Intel Mac・Noct・toolchain | Complete（q072） |
| ws009-p007 | architecture・互換・console/graphics/system・複数の無線 | Complete（q150） |
| ws009-p008 | amd64/PC98 の installer と native root | Complete（q190） |
| ws009-p009 | DOC-54 の第 2 版の設計 | canceled（p010〜p024 に分けて置き換えた） |
| ws009-p010〜p021 | gpu.md・gpu-capabilities・memory・sharing・submission・fences・jobs・display・display-control・drivers・command-stream と sysctl・examples | cleared（2026-10-30 Q1） |
| ws009-p022 | gpu-requests.md（索引と errno の表） | cleared（2026-10-30 Q1、main 4374dde38） |
| ws009-p023 | DOC-54 の統合 | cleared（2026-10-30 Q1、main 4a3b4e0e4） |
| ws009-p024 | DOC-54 の正確さの敵対的レビュー（BUG-365・BUG-366） | cleared（2026-10-30 Q1、main d2c8a5969） |
| ws009-p025 | managed-wlan.md の書き直し（DOC-55） | cleared（2026-10-30 Q1、main 581085a8c） |
| ws009-p026 | build-from-source.md の Noct と variant、索引、Status の行 | cleared（2026-10-30 Q1、main 2dd8d03b0） |
| ws009-p027 | 照合を `check-docs-current.py` に作り直し | cleared（2026-10-30 Q1、main 13e2ab4b2） |
| ws009-p028 | clean な tree からの build と boot の再現、互換の照合 | cleared（2026-10-30 Q1、main 5e17e926e） |

## 記録の所在

各 Phase の計画・結果・試験・設計の review（`gpu-doc-review.md`）は WS の完了で削除する。削除の前の `plan/ws009/` は git の履歴にある
（この ws.md を完了の形にした commit の親）。
