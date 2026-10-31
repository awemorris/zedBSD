<!-- awesome-plan project=zedbsd record=ws337 -->

# WS337 RISC-V 64 で x86_64・arm64 の Linux の binary を動かす（事前の変換と cache、interpreter の予備）

- Master: [plan/master.md](../master.md)（2026-11-01 作成）。Primary Milestone: MG002。
- Status: planning（2026-11-01 05:45 新設。ユーザーとの質疑の途中、設計の Phase はまだ始めていない）
- 目標: RISC-V 64 の zedBSD の上で、x86_64 と arm64 の Linux の user mode の program を動かす。SoC は揃ったが app の無い RISC-V で、
  x86_64・arm64 の資産を使えるようにする。QEMU の user mode（TCG、その場で少しずつ変換する方式）より速くするため、
  実行 file と動的 library ごとに事前に RISC-V の命令へ変換して cache に置き、変換しきれなかった所だけを interpreter で動かす。
- 進め方: 未定（設計の Phase で決める。標準廉価実装手順を使うかもユーザーが決める）。

## ユーザーの決定

- 2026-11-01 ユーザーから、RISC-V 64 で x86_64 と arm64 の binary を user mode で動かす物を作る、事前の binary の変換と、変換しきれなかった所の
  interpreter の予備で動かす、目的は RISC-V で x86_64・arm64 の資産を動かすこと、QEMU の TCG は速くないので事前の変換と OS の支えを組み合わせて
  速くする、という趣旨の指示を受けた。
- 2026-11-01 ユーザーから、変換した結果は実行 file と動的 library（.so・DLL）ごとに cache する、interpreter で動かした所（命令とデータの境目の
  曖昧な所など）は記録して、次の事前の変換の手掛かりにする、という趣旨の指示を受けた。
- 2026-11-01 ユーザーから、kernel の機能として作るのではなく、user land を含めた OS 全体に、他の architecture の binary を変換した物の置き場所が
  あり、それで支えるだけ、kernel に手を入れる所はおそらく無い（入れるならそれでもよい）、この技術は Linux でも使える、という趣旨の指示を受けた。
- 2026-11-01 ユーザーから、program が呼ぶ .so・DLL を link して 1 つの binary に変換することもできる、という趣旨の指示を受けた。
- 2026-11-01 ユーザーから、対象は Linux の binary も含む、という趣旨の答えを受けた。
- 2026-11-01 ユーザーから、library は最初から thunk（変換した program から RISC-V の native の library を直に呼ぶ橋）を支えるが、Linux の
  system call を真似る事もあり得るので両方の口を作る、という趣旨の答えを受けた。
- 2026-11-01 ユーザーから、x86_64 から作る、という趣旨の答えを受けた。
- 2026-11-01 ユーザーから、変換の出力の前提は RV64GC＋Zba・Zbb、vector（V）は有れば使う、という趣旨の答えを受けた。
- 2026-11-01 ユーザーから、Linux の ELF が実行されたら binfmt（他の architecture の ELF を見た exec が、登録した user land の道具へ渡す仕組み）で
  hook して、変換済みの binary を起動する、という趣旨の答えを受けた。
- 2026-11-01 ユーザーから、Linux の binary に link されている .so が既知の物なら、zedBSD で真似るための専用の library に link する、という趣旨の
  答えを受けた。

## まだ決まっていない事（設計の Phase で案を作り、ユーザーに問う）

- 事前の変換をいつするか（初回の実行・package の導入・裏での作り直し）、cache の置き場所と鍵（中身の hash）と無効化。
- x86 の memory の順序（TSO）を Ztso の無い RISC-V でどう守るか（fence の付け方、thread が 1 つの時の扱い、app ごとの設定）。
- 実行中に命令を作る program（JIT：Java・.NET・JavaScript の engine など）の扱い（interpreter だけか、後で小さなその場の変換を足すか）。
- 完了の印にする「動いた」物（CLI の道具・言語の処理系・GUI の app・速さの目標）と、x86_64 の命令の範囲（x86-64-v2 の SSE4.2 まで、v3 の AVX2 まで）。
- binfmt の hook を kernel の exec のどこに置くか（kernel に入れる最小の物）、Linux の system call を真似る層の範囲（/proc・/sys・futex・epoll・clone など）。
- 既知の .so の一覧と、専用の library（thunk）の作り方。
- 標準廉価実装手順を使うか、担当。

## Resume point

- 2026-11-01 05:45 Q1: WS を planning で立てた。上の「まだ決まっていない事」をユーザーに問い終えたら、p001（設計）を立てる。

## Phase

| ID | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| p001 | 設計（範囲・変換の方式・cache・binfmt・Linux の ABI の層・thunk・memory の順序・試験の道具・Phase の分け） | planning | — |
