<!-- awesome-plan project=zedbsd record=ws129-p008 -->
# ws129-p008: ベータ1 の公開（ユーザーの指示でだけ）

Status: planning（手順は 2026-10-09 夜に記載。p007 とユーザーの公開の指示を待つ）
Disposition: normal
Parent: [WS129](../ws.md)
Focused goal: fg019（ベータ1）
Queue: none
目安: 1h

## 範囲

ユーザーの明示の指示の後でだけ: tag の作成と push（またはユーザーの操作）、CI の release の job の実行の確認、公開された配布物の download と SHA-256 の照合、release notes の表示の確認。
AGENTS.md の「push はしない」はユーザーの指示で上書きされる範囲に限る（指示の文言と範囲を記録する）。

## 受け入れ

公開の URL、配布物の SHA-256 の一致、release の本文の確認。

## 所有 path

`plan/ws129/`。

## 依存

p007、ユーザーの公開の指示。

## 未決の判断

公開の指示そのもの。

## 公開の手順（2026-10-09 夜 P1、ベータ2。実行はしない、push と tag はユーザーの指示で）

版は `VERSION` の `1.0.0-beta2`、release の config は `config/release/config-amd64-beta2.mk`（`ZEDBSD_RELEASE_ZIP := n` なので Windows の zip は載らない）、本文は `docs/release/zedbsd-1.0.0-beta2.md`。workflow は `.github/workflows/release.yml`、tag の検査は `tools/release/release-tag.sh`。

### 事前の確かめ（RC の commit で）

1. `docs/release/` の 3 つの file の `review:` の comment を全て決めて消す（p005）。既知の問題の表は RC の時点の Bug Board と合わせる。
2. tag の検査と release の情報を local で: `sh tools/release/release-tag.sh classify zedbsd-1.0.0-beta2-rc1` → `kind=build rc=1 version=1.0.0-beta2`、`make -s ZEDBSD_CONFIG=config/release/config-amd64-beta2.mk release-info` → `version=1.0.0-beta2`・`name=1.0.0 Beta 2`・`zip=n`、`sh plan/ws129/tests/host-release.sh build/<自分>/ws129-host` → `18 passed, 0 failed`（2026-10-09 夜 P1 の tree a77107e68 で 3 つとも確かめた）。
3. license: `python3 tools/release/license-inventory.py --config config/release/config-amd64-beta2.mk` が `27 components, 0 open items`（p002）。CI の build の step も同じ検査と `--rootfs` を行い、open があると止まる。
4. p006 の最終回帰（release の config の image、QEMU）が通っていること。
5. push の前の検査（AGENTS.md の git の手順）: `git fetch origin && git log --format=%B origin/main..main | grep -ci 'co-authored-by'` が 0、`git log --format=%B origin/main..main | grep -v '^$' | grep -vx WIP | wc -l` が 0。

### RC（ユーザーの指示の後、Q1）

1. `git tag zedbsd-1.0.0-beta2-rc1 <RC の commit>` → `git push origin main`（ユーザーの指示の範囲で）→ `git push origin zedbsd-1.0.0-beta2-rc1`。tag は動かさない（直しが要れば rc2 を新しく作る）。
2. 実行の確認: `gh run list --workflow release.yml --limit 3`、`gh run watch <id>`。job は classify → build（toolchain の build、license、image、gzip、SHA256SUMS）→ publish。
3. Prerelease の確認: `gh release view zedbsd-1.0.0-beta2-rc1 --json name,isPrerelease,isLatest,assets` で、名前 `Kei/zedBSD 1.0.0 Beta 2 RC 1`、isPrerelease true、latest でない、asset が `zedbsd-1.0.0-beta2-amd64.img.gz`・`SHA256SUMS`・`LICENSES.md` の 3 つ（2 GiB 未満）。
4. 配布物の確かめ: 新しい directory に `gh release download zedbsd-1.0.0-beta2-rc1` → `sha256sum -c SHA256SUMS` が OK、`gzip -t` が通る。展開した image を T1 が `plan/tools/boot-test.sh` で起動（login prompt の PNG）、ユーザーが 5330 で UAT（p007）。
5. 本文の表示: GitHub の release の頁で、HTML の comment が見えないこと、link（known issues・guide・Makefile）は最終の tag ができるまで切れていることを確かめる（予定どおり）。

### 最終（ユーザーの指示の後、Q1）

1. RC の後の変更は `docs/release/` だけにする（`release-tag.sh promotable` が他の差を拒む）。それ以外を直したら新しい rc から。
2. `git tag zedbsd-1.0.0-beta2 <commit>`（RC の commit か、docs/release/ だけ進めた commit）→ push。promote の job が最も大きい rc（または workflow_dispatch の from_rc）の asset を download して `sha256sum -c` し、同じ bytes を Prerelease `Kei/zedBSD 1.0.0 Beta 2` として、最終の commit の本文で作る。
3. 確かめ: `gh release view zedbsd-1.0.0-beta2 --json name,isPrerelease,assets`、download して `sha256sum -c SHA256SUMS`、RC の SHA256SUMS と同じ。本文の link が開けること。
4. Latest Release への昇格はユーザーが手で行う（エージェントは行わない、2026-10-02 ユーザー）。
5. 記録: 公開の URL、asset の SHA-256、tag の commit をこの phase.md に。

### 失敗した時

- classify が落ちる: tag の名前と VERSION の不一致。tag を消すのはユーザーの判断（GitHub の tag の削除）で、普通は正しい名前で作り直す。
- build が落ちる: log を読み、直して rc を一つ進める。publish が落ちた時は workflow_dispatch（tag を入力）で同じ tag を作り直せる（release が無ければ）。
- promote が落ちる: `promotable` の拒否なら docs/release/ 以外の差を確かめる。asset の sum の不一致は rc の release を調べる。
