# q086 FS/USB acceptance — 50 short stories

Date: 2026-09-06. Status: defined; execution pending.
Each row is a short sequence with a recovery/postcondition, not an assertion count.
Production-source host fixtures inject near-normal errors deterministically.
Native integration uses disposable images and grouped boots; only S49 requires
a persistence reboot. Record runner, individual result, and coverage limitations
in ws018-p019/results.md. A scenario is not passed merely because its model passes
if the row explicitly requires native execution.

| ID | Layer | Sequence | Required result |
| --- | --- | --- | --- |
| S01 | USB | 通常READ→WRITE→FLUSH→再READ | 内容一致、全URB回収 |
| S02 | USB | CSWだけSTALL→clear-halt→CSW再読 | CBW再送なしで成功 |
| S03 | USB | CSW再読もSTALL→reset→再発行 | 回数有限、成功後I/O継続 |
| S04 | USB | CBW短転送→reset→READ再発行 | 不完全CBWを成功扱いしない |
| S05 | USB | data timeout→cancel再試行成功 | 期限内に返り次のBIO成功 |
| S06 | USB | cancel連続失敗→呼出元buffer解放→遅延完了 | 隔離bufferのみアクセス、二重完了なし |
| S07 | USB | cancel失敗後同URBを再setup | EBUSY、保持中buffer上書きなし |
| S08 | USB | 隔離中に別endpoint転送 | 別転送は進行 |
| S09 | USB | reset成功直後06/29/00→再発行 | 一回のみ許可 |
| S10 | USB | resetなし06/29/00 | 自動再発行しない |
| S11 | USB | reset後06/28媒体変更 | 書込再開しない |
| S12 | USB | reset後02/3A媒体なし | 媒体なしを成功扱いしない |
| S13 | USB | reset後06/29が連続 | 有限エラー、無限retryなし |
| S14 | USB | FLUSH transport失敗→reset→成功 | sticky latch前に回復 |
| S15 | USB | FLUSH再試行も失敗→WRITE→READ | 書込拒否、読込の可否を独立判定 |
| S16 | USB | CSW tag不一致→回復 | 異なるcommandの成功を受理しない |
| S17 | USB | CSW residue超過→回復 | 長さ不整合を拒否 |
| S18 | USB | disconnect中のretry | 新世代への再発行なし |
| S19 | FS | 連続extent loop書込→通常FAT読込 | alias一致 |
| S20 | FS | 断片extent境界を跨ぐloop書込 | 論理順と内容一致 |
| S21 | FS | loop末尾ちょうど→一block超過 | 境界内成功、越境拒否 |
| S22 | FS | extent論理holeでattach | 不完全map拒否、claim回収 |
| S23 | FS | extent親partition範囲外 | attach拒否、書込なし |
| S24 | FS | loopと隣接fileが同cache line共有 | 隣接bytesを保持 |
| S25 | FS | FAT slotをwarm→loopで同sector更新 | 古いslotを返さない |
| S26 | FS | loop attach中通常backing書込/切詰め | claimが拒否 |
| S27 | FS | loop detach→backing更新→reattach | 古いmapを再利用しない |
| S28 | FS | loop FLUSH失敗→次のREAD/FLUSH | エラー伝播、lock解放 |
| S29 | FS | UFS既存block全体overwrite | 不要なdata readなし、内容一致 |
| S30 | FS | UFS部分overwrite→前後READ | 周囲bytes保持 |
| S31 | FS | 新規UFS blockの部分write | 未書込範囲zero |
| S32 | FS | FAT chain要求範囲より先を破損→WRITE | 従来の全chain検証を維持 |
| S33 | FS | UFS metadata rollback失敗→後続write | readonly維持、flush成功だけで復帰しない |
| S34 | FS | 複数block directoryを生成→image検査 | 更新制限を明示検出 |
| S35 | FS | 通常image→directory検査 | 現行imageの結果を記録 |
| S36 | FS | overlay copy-up→小変更→再open | 下層保持、上層内容一致 |
| S37 | IO | regular 64KiB write/read | 4096byte単位、内容一致 |
| S38 | IO | unaligned regular pread/pwrite | offset保持、前後不変 |
| S39 | IO | readv/writev空要素と境界跨ぎ | 合計長とoffset正しい |
| S40 | IO | pipe PIPE_BUF writev | 原子性の既存契約維持 |
| S41 | IO | 途中user-copy fault | 部分転送とoffsetを正しく返す |
| S42 | IO | buffer確保失敗→次のI/O | ENOMEMまたは小buffer fallback、lock漏れなし |
| S43 | IO | 既存mmap→通常write/read | 既存VM coherence維持 |
| S44 | IO | 4セル chunk512/4096×IMOD4000/0 | 呼出数と測定可能な値を比較、未測定IRQ値を捏造しない |
| S45 | 統合 | wifi-store初回保存→読戻し | temp fsync→rename→dir fsync成功 |
| S46 | 統合 | 保存途中失敗→再保存 | 旧値か新値の完全形、回復後保存可能 |
| S47 | 統合 | 設定置換反復→daemon再読込 | 不完全profileなし |
| S48 | 統合 | native FAT/loop/UFS/overlay一連操作 | 実kernel上で永続化経路が動く |
| S49 | 統合 | 一括sync→一度再起動→読戻し | 永続性確認、毎シナリオ再起動不要 |
| S50 | 統合 | Wi-Fi30既存stories再実行 | 今回変更でcommand/daemon契約を壊さない |

