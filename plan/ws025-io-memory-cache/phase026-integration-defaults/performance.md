# WS025 最終性能観測

2026-09-08 JST。QEMU TCG、OVMF、4 CPU、xHCI USB-root、IMOD=4000。
各 mode は 64 KiB pwrite を4回実行して最後に fsync、101反復。
4 GiB と16 GiBの計404標本で内容とcounter oracleが合格した。
両条件とも使い捨てイメージ内の data.img を4 KiB境界に配置し、layout.jsonに記録。
通常の同期書込み経路であり、writeback opt-in によるfsync省略ではない。

| RAM | 書込み位置 | 中央値 | p95 | p99 |
| --- | --- | ---: | ---: | ---: |
| 4 GiB | 同じ64 KiBを4回 | 10 ms | 40 ms | 40 ms |
| 4 GiB | 異なる連続256 KiB | 10 ms | 20 ms | 20 ms |
| 16 GiB | 同じ64 KiBを4回 | 10 ms | 20 ms | 20 ms |
| 16 GiB | 異なる連続256 KiB | 10 ms | 20 ms | 20 ms |

計時は10 ms刻みで、実USBの速度を表さない。過去のp005/p006にはFAT配置が
2 KiBずれた標本があるため、中央値30–40 msとの違いをそのまま単一変更の
改善率にしない。冒頭の約0.5秒という観測とも完全な同条件比較ではない。

## 転送単位

全標本で上位書込みは4回／262144 bytes、I/O pool新規backing割当て0回、
large borrow/return各4回、small/fallback 0回、completion error 0回。
同一位置modeのUFS data runは4回、連続位置modeは5回だった。
このcounterはUFS内部のdata runであり、全file backend呼出し数ではない。
現在のdata.imgは8 KiB block／12 direct pointersなので、2本目の64 KiB writeが
96 KiBのdirect/indirect境界を跨ぐ。content_run_bytesは一つのmapping windowに
限定するため、そのwriteを二つのrunにする。4 KiBごとのsyscall分割はない。
FS50 S44は現在のsyscall関数をそのまま使い、64 KiB pool経路と枯渇時の返値を検証する。

USB WRITE(10)は同一位置modeで10回、連続位置modeで11回、いずれも311296 bytes。
これにはdata以外のmetadataが含まれる。IO_DRIVER_WRITEはloop層とphysical層を
両方数えるため20/22回・622592 bytesとなり、USBとの二重計数を性能悪化と解釈しない。

冗長なHCD bounce copyは両modeで0 bytes。USB coreの所有stagingへのコピーは残り、
4 GiBではmetadata・BOT制御を含め311824–311912 bytes。周期的TURにより小さく変動する。
copy削減は確認できるが、これだけでCPU短縮やdirect user DMAの必要性を主張しない。

証拠: `../temp/p026-baseline-1/{4096,16384}/` の samples.tsv、summary.json、
layout.json、guest.log、source.sha256、および親results.json。新しい標本のquantileと
呼出し数はsummarize-io-baseline.pyで再計算・照合済み。
