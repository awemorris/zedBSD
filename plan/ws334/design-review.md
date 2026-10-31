<!-- awesome-plan project=zedbsd record=ws334-design-review -->

# WS334 p001 design-reviewer の敵対的レビューと反映

- 日時: 2026-11-01（レビューの起動 00:50 頃、結果 01:05 頃、反映 01:09〜）。レビューの担当: design-reviewer（P7 が起動、read-only）。
- 対象: [design.md](design.md) の a80138ca4 の版（決定 14 の前）。レビューの間に P7 が決定 14・15 で §0・§4 と Phase の番号を書き直したので、レビューの担当は
  HEAD（9cfc879db）でも #2・#7・#11・#16・#18 が残る事を確かめた。下の「Phase」の番号はレビューの時の物で、反映は今の番号（p002〜p018）で書いた。
- レビューの担当が code で確かめた design.md の file:line の参照: 約 70 か所、ほぼ正しい（§2.2・§2.5・§2.6 の抜き取り）。user-questions の番号の一致、決定を要点で
  記録している事、HAL の変更が無い事、toolchain（llvm-mingw）が許可の問いになっている事も確かめた。
- 「[V]」はレビューの担当が code で確かめた物。P7 も #1（`vm/heap.c:555`・`:745`）・#5 の memmem（`page/svg.c:1968`）・#4（`loader.c:1122-1127`）・#7 の 32 bit
  （`base/base.h:140`）・#11 の `gcc-mingw-w64-ucrt64` が apt の候補にある事を確かめた。

## 指摘と扱い

| # | 重さ | 節 | 指摘（要点） | 扱い | 直した所 |
| --- | --- | --- | --- | --- | --- |
| 1 | 高 [V] | §2.3・§9.1・p003 | `vm/heap.c` の `posix_memalign`（:555・:745）と `free` は mingw・UCRT に無い。vm は「ISO C だけ」は誤り | 反映 | §2.2 O30、§2.3、§3.4 の memory の群、§9.1 に `vm/heap.c`、p003 |
| 2 | 高 [V] | §6.2・§4.4 | libbrowser・libkeiland・UI が `vk*` を直に呼ぶので、`vulkan-1.dll` の import で loader の無い機械で .exe が起動しない（CPU の道も使えない） | 反映 | §6.2 遅延の import（`dlltool --output-delaylib`）と loader の有無の確かめ、予備の thunk の file、§7.3 M2、§11 A17 |
| 3 | 高 [V] | §3.5.4 | memory の cache を instance で共有すると view ごとの network の thread が解放と参照で競う。lock の順が無い。disk の IO を lock の下で待つ | 反映 | §3.5.2 storage の thread の write-behind と lock の順、§3.5.4 memory の cache は loader ごと・永続の索引だけ instance、§11 A13・A14、p006 を分けた |
| 4 | 高 [V] | §5 M1 | ring の起こしは `rang` を立てず page に届かない。engine の thread は open で始まるので後からの set は最初の起こしを落とす。`media.h` の約束が変わる | 反映 | §5 M1（open の引数、`net_loader_ring`、`media.h` の書き直し） |
| 5 | 中 [V] | §2.2 | memmem・stdout の text の mode・SSL_ERROR_SYSCALL の errno・POLLIN の wants・strtod の locale・libmedia の close の漏れ。header の名前の grep の限界 | 反映 | §2.2 O29・O31〜O34、§5 M9、§2.3 と `cross-build.sh` の `nm -u` の許す表、§12 R13 |
| 6 | 中 [V] | §5 M7 | `__aarch64__` は CPU の分岐でなく Pi 4 の firmware の decoder の回避 | 反映 | §5 M7（device で分ける）、§9.2 に `vkvideo.c` |
| 7 | 中 [V] | §3.5.3 | hash は 32 bit、区切りの無い連結の衝突、書きの時の衝突で上書き、`.ls` に origin が無い、長い origin の今の file の移行が無い | 反映 | §3.5.3（長さの前置き、FNV-1a 64、書く前の照合と衝突の番号、v1 の読み・v2 の書き・移行） |
| 8 | 中 [V] | §3.6 | 既定の instance と v3 の挙動の矛盾、ABI の互換は無い、once、destroy の戻り、違う thread の view が Web Storage を lock 無しで共有 | 反映 | §3.6（何も保存しない既定、once、int を返す、instance と view は 1 つの thread、作成の失敗でも close） |
| 9 | 中 | §3.5.5・§3.7 | lock の file の残り、fsync が無い、cookie の平文 | 反映（平文は問い） | §3.5.5（OS の lock、fsync・FlushFileBuffers、やり直し）、§11 A15、問い U14 |
| 10 | 中 | §6.6 | ROOT の store は写しで足りない根がある・Disallowed を見ない。mingw の OpenSSL の OPENSSLDIR・設定の自動の読み | 反映 | §6.6 S-A（Mozilla の根を合わせる、Disallowed を拒む、既定の path を呼ばない、OPENSSLDIR と NO_LOAD_CONFIG）、既知の限り、p013 |
| 11 | 中 | §6.1・§2.11 | msvcrt の gcc に `-mcrtdll=ucrt` は混ざる。UCRT の gcc がある。OpenSSL も同じ CRT。heap の記述。libgcc。winpthreads の 2 重 | 反映 | §6.1 T-B を `gcc-mingw-w64-ucrt64` に、§2.10、§2.11、`-static-libgcc`、`libwinpthread-1.dll` を 1 つ同梱、§11 A16 |
| 12 | 中 [V] | §6.1・§6.4 | 1809 では UTF-8 の active code page が効かず narrow の path の open が日本語の folder で失敗 | 反映 | §6.1 対象を Windows 10 1903 以降に、§6.3 |
| 13 | 中 [V] | O19・§3.4 | Windows の時刻帯の名前は略号でないので今の表は使えない | 反映 | §3.4 の時刻の群（CLDR windowsZones の表をデータとして引用） |
| 14 | 中 | §4.1・§4.7 | (a) 共通の shell への移動は browser-component の規則 2 の変更、(b) apps/ は checker の外、(c) host-build.sh・host-shell.c が壊れる、(d) png-write.c の漏れ、(e)(f) 共通の shell は ISO C だけでない・GTK4 に合わない | (a)(e)(f) は決定 15（案 A）で共通の shell を作らなくなったので予備の案 B の記録に。(b)(c)(d) は反映 | §12 R14、§4.8 と p009 に host-build.sh・host-shell.c、§6.9 に png-write.c、§4.2 の予備の案 B |
| 15 | 中 [V] | §6.9 | 一番上の Makefile は Windows で壊れやすい、compat の header の置き場、compat の `-Wno-error`、clean の守り、共有の `build/sources` への取得 | 反映 | §6.9（単独の入口を本来の物に、compat の header の写し、clean の守り、取得は自分の出力へ）、§6.3 の warning の扱い、問い U7 |
| 16 | 中 [V] | §9 | file の一覧の漏れ（`vm/heap.c`・`page/svg.c`・`page/link.c`・`net/http.c` の p005・`page/script.c:575`・`vkvideo.c`・`picture.c`・`host-shell.c`）、shell の重なりの列、開発の管理情報の comment | 反映 | §9.1・§9.2 を書き直し（重なりの列、comment は p018） |
| 17 | 中 | §8 | p010 の libmedia への依存、p003 の受け入れ、R4 の確かめの時期、p005・p010 の見積もり、規約の Phase の独立、実機の合格の組、T4 は標準でない、手順の前提物が無い | 反映 | §8 を書き直し（p006 を分ける、p011 は p008 に依存、規約は p018、R4 は p015、§7.3 の M1〜M8、T4 を外す、T1〜T3 の前の小さな設計） |
| 18 | 中 | 全体 | §1 の libkeiland の範囲の食い違い、棚卸しの Phase の誤り、「Q1」の 2 つの意味、sink の例外、決定 9 の読み、決定 3 の WS234 への移しは決定 14 で再確認 | 反映 | §1、§2.12・R9、問いを U1〜U14 に、§3.5.2、§3.1 と問い U15、§4.9（決定 14・15 の形） |
| L1 | 低 | §3.4 | loopback の waker の accept の後に相手を確かめる | 反映 | §3.4 の起こしの群 |
| L2 | 低 | §3.4 | 依存の DLL の探索も狭める | 反映 | §3.4 の library の群（`SetDefaultDllDirectories`） |
| L3 | 低 | §7 | `objdump -p` の許す system の DLL の表 | 反映 | p014 の受け入れ |
| L4 | 低 | §3.6 | instance の作成の失敗の時の files の context の解放 | 反映 | §3.6（作成の失敗でも close） |
| L5 | 低 | §3.5.5 | antivirus が掴む間の `MoveFileEx` の失敗 | 反映 | §3.5.5 の files の行 |
| L6 | 低 | §6.6・§7.4 | OpenSSL の `mingwarm64` と arm64 の DLL の名前、llvm-mingw の make は未確かめ | 未確かめとして記録 | §6.6、§7.4（make は Git for Windows か MSYS2） |

## 見送った物

なし（全部を反映するか、決定 15 で予備の案 B の記録に移した）。
