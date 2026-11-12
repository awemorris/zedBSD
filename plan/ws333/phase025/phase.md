<!-- awesome-plan project=zedbsd record=ws333-p025 -->

# ws333-p025: p014 の食い違いの解析と直し（HWP の読み・E-core の domain・package の C-state）

Status: in-progress（2026-11-12 Q1、q2022-i01 P1）
Parent: [WS333](../ws.md)

## 目的

[p014](../phase014/phase.md)（5330 の CPU の受け入れ）の測定で出た食い違いを、code を読んで原因まで突き止め、直せる物は直す。考える Phase。実機の測定は原因と直しが揃った後に 1 回だけ流す。

## 解析する事（p014 の §3 と持ち越し）

1. **HWP の読みが古い**: `thermal cpu` を 5 秒ごとに 4 回読むと、最高の MHz が毎回同じ値。`delivered_mhz` は GET の時にその CPU で読む設計（design.md）。読みの道（ioctl → cpuperf → provider → MSR の APERF・MPERF か IA32_HWP_STATUS 等）を辿り、写しを返していないか、差分の基準（前回の値）を更新しているかを確かめる。
2. **E-core の MHz が要求の上限を超える**: powersave（E の max 12）で 3280 MHz、manual 0（E の max 8）で 2191 MHz。E-core の `highest_mhz` が 0。level と MHz の換算（CPUID 0x16・HWP_CAPABILITIES・bus の MHz・P と E の scale の違い、Intel SDM の hybrid の記述）を確かめる。
3. **manual 100 50 50 で spin が 1192 MHz**（要求 1-38 なら 3800 まで）。strategy の値から HWP_REQUEST の min・max・EPP・desired への写像と、package の HWP_REQUEST_PKG・HWP の enable の順を確かめる。1 と混ざっていないかを分ける。
4. **E-core が 4 つ組の domain に出ない**: `thermal cpu domains` で E-core が 1 CPU ずつ別の domain。topology の組み立て（CPUID 0x1A・0x1F の module の level、L2 の共有）を確かめる。
5. **package の C-state が 0%**（p012 から）と **全 CPU の負荷で P-core が 1.4 GHz に留まる**（p011 の PLATFORM_LIMIT 0x8）: kernel の側で妨げている物（MWAIT の hint、C-state の auto-demotion・undemotion、0xE2 の package の limit、tick・IPI の起こし）があるかを code と MSR の値で確かめる。kernel の外（ASPM・PSR・電力の上限の設定）なら、その根拠を記録して他の WS へ渡す材料にする。

## 進め方

- まず code と仕様（Intel SDM Vol. 3・4）だけで原因の仮説を立て、根拠（file:line、SDM の節）を書く。直せる物は直し、host の試験（plan/ws333/tests/ の cpu-x86-host、test-hwp 等）を足して確かめる。
- 実機が要る確かめは 1 本の script にまとめ、最後に Q1 の割当で 5330 で 1 回流す（p014 の script の直し: levels は strategy の後に読む、spin を CPU ごとに置く道具が無ければ全 CPU に 1 本ずつ等）。
- 解けない物は、試した事・残る仮説・要る情報を書いて Q1 に返す。

## 受け入れ

- 1〜5 の各々に、原因（根拠付き）か、kernel の外と判断した根拠が書かれている。
- 直した物は build warning 0、host の試験 PASS。5330 の測定で 3 の MHz が要求に従う（要求の上限を超えない、manual の上限の近くまで出る）事と、読みが 4 回で変わる事。

## 結果

（未記入）
