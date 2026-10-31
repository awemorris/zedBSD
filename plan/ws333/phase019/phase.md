<!-- awesome-plan project=zedbsd record=ws333-p019 -->

# ws333-p019: sessiond の THERMAL の要求（strategy・boost、root か wheel）

Status: in-progress（要求の口・権限の規則・答えの行と、その host 試験は済み。sessiond から kernel への呼びは `/dev/thermal` の UAPI（p004、ユーザーの review 待ち）まで stub で、持ち越し）
Parent: [WS333](../ws.md)
Queue: q1777-i01（P4）

## 目的

design.md §9 の p019 と §4.10 の権限: Keiland の app（session）が sessiond の socket で strategy と fan の boost を頼み、root で走る sessiond が、session の user が root か wheel の時だけ代わりに `/dev/thermal` の SET を呼ぶ。GET は誰でも `/dev/thermal` を直に読めるので、sessiond は SET の 2 つ（strategy・boost）だけを持つ。

## 判断

- 2026-10-31 ユーザーから、昼頃まで不在の間は実装と試験を進める、という趣旨の指示を受けた（Q1 経由）。
- Q333-1・Q333-3 は推し（B・A）で進める（Q1 の指示）。この Phase への効き:
  - Q333-1 B（最後に選んだ strategy を保存し起動の時に rc が渡す）: session が選んだ strategy も「最後に選んだ物」なので保存の対象。保存の形と書き手は p013（`/sbin/thermal` と `/etc/thermal.conf`）が決めるので、sessiond が保存するのは p013 の後（持ち越し）。
  - Q333-3 A（root の直の設定は CUSTOM、次の strategy で消える）: sessiond は直の設定を受けない（strategy の SET だけ）。CUSTOM を消すのは kernel の SET の側の振る舞いで、sessiond には何も足さない。
- 要求の形（session の socket の 1 行、POWER・SERVICE と同じ並び）:
  - `THERMAL strategy performance|balanced|powersave`
  - `THERMAL strategy manual SPEED NOISE GATING`（各 0〜100、向きは Q333-4 の推し A の「100 = その物が多い」）
  - `THERMAL boost all|FAN LEVEL`（FAN 0〜255、LEVEL 0〜255。fan の有無と段の数の上限は kernel が答える）
  - 語は空白 1 つで区切り、前後の空白・tab・余りの語は EINVAL。数は 10 進で符号・先の 0 無し、3 桁まで。`firmware` は選べない（kernel の SET(FIRMWARE) が EINVAL なのと同じ）。
- 権限: root か wheel（POWER・SERVICE と同じ `account_in_wheel`）。語の検査が先で、形の悪い要求は誰からでも ERROR（power-rules と同じ順）。
- 答えの行: `OK`（kernel が返した bit を ` partial`・` pending`・` superseded` の語で足す）、`FAIL wheel`（POWER と同じ語）、`ERROR invalid`（EINVAL）・`ERROR unsupported`（EOPNOTSUPP）・`ERROR nofan`（ENOENT）・`ERROR unavailable`（ENODEV、`/dev/thermal` が無い）・`ERROR failed`（他）。
- 記録: 全ての要求を sessiond の認証の log と syslog に（誰が・何を・errno）。
- sessiond から kernel への呼び（`thermal.c` の `thermal_carry()`）は、UAPI の ioctl の番号と struct が決まるまで ENODEV を返す stub（答えは `ERROR unavailable`）。UAPI ができたら、この関数だけを `/dev/thermal` の open と SET_STRATEGY・SET_BOOST の ioctl に差し替え、UAPI の flag を `SESSIOND_THERMAL_*` の bit に写す。

## 成果（branch agent/p4-bugs）

- `userland/desktop/sessiond/thermal-rules.h`・`thermal-rules.c`（新規）: `sessiond_thermal_decide()`（語の解析と権限）、`sessiond_thermal_answer()`（errno と bit から答えの行）。file・socket を触らないので host 試験で単独に走る。
- `userland/desktop/sessiond/thermal.c`（新規）: `sessiond_thermal()`（決めて・log して・carry して・答える）と stub の `thermal_carry()`。
- `userland/desktop/sessiond/session.c`: session の要求の振り分けに `THERMAL ` を足し、file の先頭の protocol の表に 3 行を足した。
- `userland/desktop/sessiond/sessiond.h`: `sessiond_thermal()` の宣言。`Makefile`: 2 つの source を足した。
- 試験: [tests/sessiond-thermal/run.sh](../tests/sessiond-thermal/run.sh)（`host-thermal-rules.c` と `thermal-rules.c` を gnu89 で、素と ASan/UBSan の 2 回。出力は既定 `/tmp/ws333-sessiond-thermal`、`fresh_out`）。

## 確かめ（2026-10-31、P4）

| 確かめ | コマンド | 結果 |
| --- | --- | --- |
| rules の host 試験 | `sh plan/ws333/tests/sessiond-thermal/run.sh` | PASS 113 checks（素と ASan/UBSan の両方）。語 10 の成功・形の悪い語 32・権限 7（wheel の成功 2、他の user の EPERM 3、他の user の形の悪い語は EINVAL 2）、拒みの時に request が 0 に戻る事、答えの行 13・小さすぎる buffer 2 |
| 試験の効き（突然変異、手で 8 つ） | `thermal-rules.c` を sed で変えて同じ試験を compile して走らせる（/tmp の写し、tree は変えない） | 7 つを検出（権限の検査の外し、先の 0、上限の +1、名前の後の余りの語、boost の余りの語、答えの語、ENODEV の写し）。残り 1 つ（空の語の検査の外し）は今の文法では同値（空の語は次の strcmp・数の検査で EINVAL になる） |
| sessiond の build | `make -j16 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=/tmp/p4-ws333-p019/b /tmp/p4-ws333-p019/b/bin/sessiond` | rc 0、`-Werror` の下で `thermal.c`・`thermal-rules.c`・`session.c` を compile し link（`bin/sessiond` 94304 byte）。log の `warning:` 273 件は perl の locale 258 と openssl の package 15 で、sessiond の物は 0。openssl の package は worktree の `build/packages/` に build された |
| QEMU | — | 未実施（kernel への呼びが stub で、p005 の注入の build もまだ無いため。Q1 の指示で未実施でよい） |

## 持ち越し（UAPI の後）

1. `thermal_carry()` を `/dev/thermal` の SET_STRATEGY・SET_BOOST の ioctl に差し替える（p004 の UAPI の後。open の失敗は ENODEV に写す）。
2. session の strategy の保存（Q333-1 B、p013 の `/etc/thermal.conf` の書き手を使う）。
3. QEMU の試験（design.md §6 の「sessiond（p019）」: p005 の注入の build の試験の受け手で、wheel の session から strategy が届き、wheel でない user が拒まれる事を ssh で読む）。
4. Keiland の側の呼び手（設定の画面）は別の WS（design.md の Keiland の行）。
