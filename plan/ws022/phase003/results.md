# WS022-p003 q128 results

Date: 2026-09-09
Status: completed

## 実装

- x86 static sysrootにZEDBSD_STATIC_TLSを定義。errno/fenv、account/crypt/utmpx、ftw/l64aの既存thread-local領域を静的実行ファイルでもTLSにする。kernel libcと他archの静的契約は変えない。
- amd64/pcat/PC98 Noct user linkerにPT_TLSと.tdata/.tbssを追加。empty PT_TLSはno-TLSとして受け入れる。
- static-tls allocatorは現TCBの読み取り専用templateから独立したRW mappingを作る。親のlive値をコピーせず、zero-fillとTP alignmentを保持。join/reaper/failed-createは既存ownerからmapping全体を解放する。
- raise()のprocess-directed killをthread-directed pthread_killへ修正。呼出し元のsignal/TLSテストで再現・検証した。
- 動的回帰で、既存libc.so TLS=0x1a48に対するrtldの1-page上限と、pure-BSS LOADの二重mapを発見・修正。q126の変更前libc.soも同じサイズだった。dynamic TLS上限は共有の1MiB、alignmentは4096のまま。BSSは既に全域mapしたケースを追加map対象から除く。

## 検証

- production static-tls host fixture、通常/ASan+UBSan PASS (`temp/q128-static-host`, `/tmp/zedbsd-q128-static-host.log`)。100回clone/free、初期template不変、zero-fill、alignment、failed mmap/SET_TLS、実行中selfをfreeしないことを確認。
- 最終compiler corpus: project clang + ld.lld、amd64/i386で各正常4例・不正10例。初期値+zero+64byte alignment、empty、zero-only、nonzero first-byte offset、および実ファイル切詰めを含む。`temp/q128-fixtures/manifest.tsv`。
- amd64最終QEMU PASS: `temp/q128-runtime-amd64-final3/`。8同時thread、100回create/join、各threadのsignal/errno、forkの値継承と分離、RLIMIT_ASでcreate失敗→復帰、不正10例のexec rollback、正常4例のexec。volatile経由でalignmentを観測し最適化で消えないようにした。
- 同じamd64 campaignでdyntest DL:01〜DL:06 PASS。startup/relocation、GD TLS、pthread TLS、plugin close/reopen/recycle、handle OOM recovery、RPATH、symbol versions、corrupt DSO拒否、libc thread safety、stdio。
- i386最終QEMU PASS: `temp/q128-runtime-i386-final/`。amd64と同じstatic/dynamic全campaignをPC/PIC/IDEで確認。

初期の失敗campaignは消去せず保存。runtime-amd64-b/cではraiseが別threadへ配信、finalでは旧rtldのTLS size上限、final2ではpure-BSS double-mapで停止していた。いずれも最後のPASSに置き換えて隠さない。

## 通常成果物とPC-98

- PC-98 `make -j16` PASS (`/tmp/zedbsd-q128-pc98-final.log`)。
- PC-98 486 QEMU `temp/q128-pc98-session/results.json` PASS。2 bootsでlogin、各12回exec、コピー内容と再起動後のchecksum一致、haltを確認。元イメージSHA256不変。
- 通常amd64/PCAT成果物を直列 `make -j16` で復元PASS (`/tmp/zedbsd-q128-amd64-restored.log`, `/tmp/zedbsd-q128-pcat-restored.log`)。config.mkはユーザーのPC98設定を維持。
- `git diff --check` PASS。実機試験はWS022完了条件ではなく、今回実施していない。

WS022 p001〜p003を完了。dynamic IE/LE startup DSOやpost-dlopen static reservationを新たに対応済みとは主張しない。
