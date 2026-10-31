<!-- awesome-plan project=zedbsd record=ws334-p024 -->

# ws334-p024 統合: 永続の HTTP の cache（第 2 段）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §3.5.1（CACHE の space）、§3.5.4、§3.7。
- 状態: planned（2026-11-01 p002 で作成）。ユーザーの決定 21（U5）で第 2 段: WS334 の第 1 段の受け入れ（p032）には入れない。Q1 が起動を決める。
- 依存: p023。
- 担当の想定: Opus（phase-runner）を 1 回。使う約束は `storage/records.h` の `wb_storage_cache_*` と `storage/queue.h`。

## 中身

1. **索引**（`storage/instance.c` に足す）: URL の key → {record の bytes の数、`stored`、最後の使用の ms}。instance の作成では CACHE の space の**索引の record だけ**を読む（body を読まない。design §3.5.4）: 索引の record は key `0`（`wb_storage_key_valid` を満たし、16 進の key は偶数の長さ・hash の key は `-` を含むので data の key と重ならない）、中身は `KEILAND CACHE INDEX 1\n` と各 entry（u16 key の長さ・key・u64 bytes・i64 stored・i64 used、little-endian）。records.h は変えない（索引の encode・decode は instance.c の中の static の関数、壊れた索引は空の索引として捨て、次の書きで作り直す）。索引の record は entry を足す・除く・使う時に write-behind で書き直す（使用の時刻は 60 秒に 1 回まで）。索引に無い record（索引の書きの前に落ちた）は上限の掃除の時に list で見つけて remove。
   - lock: 索引は instance の `cache_lock`（新）の下。lock の順（外 → 内）は `loader->lock` → `cache_lock` → jar の lock → storage の queue の lock（design §3.5.2 の順に `cache_lock` を足した。`cache_lock` の中で queue の待つ関数を呼ばない）。
2. **書き**（`net/loader.c` の `loader_cache_store` の後）: memory の cache に入れた entry を `wb_storage_cache_encode`（`stored` は `wb_os_wall_ns()/1000000`。memory の cache の `stored_at` は monotonic なので別に持つ）→ key（URL の bytes の `wb_storage_key_text`、衝突は p023 の 4 の規則）→ `wb_storage_queue_write(CACHE)`、索引に足す。network の thread から呼ぶので待たない関数だけ（queue.h の lock の約束）。
3. **読み**（`loader_cache_find` が memory で見つけない時）: 索引に URL があれば、`loader->lock` も `cache_lock` も持たずに `wb_storage_queue_read` で record を読み（network の thread が待つ、design §3.5.2）、decode して URL が同じなら memory の cache に入れ、今の `loader_cache_fresh`・`loader_cache_answer` の道へ（鮮度は `stored` を wall の時計で比べる）。ETag の再検証で 304 が来たら `stored` を今にして書き直す。
4. **上限**: 索引の bytes の合計が `cache_limit`（既定 256 MiB）を超えたら、最後の使用の古い順に remove して下回らせる。backend の write が ENOSPC なら古い物を 1 割消して 1 回やり直す（design §3.7）。
5. **消し**: 将来の「閲覧の data の消去」の口は作らない（範囲の外）。

## 試験（受け入れ）

- `plan/ws334/tests/storage-test.c` に cache の項を足す: 200 と max-age の response を host の小さな server から取り、instance を作り直した後に server を止めても同じ URL が cache から答える。no-store・POST・304・ETag の再検証・上限での LRU の削除・壊れた record の remove。
- `run-loader-tests.py`（WS074）の件数が前と同じ。`run-unit.sh all`・`cross-build.sh` が PASS。zedBSD の libbrowser の build が warning 0。

## 変えてはいけない物と終わり

- module の header と .c、試験の道具。HAL・toolchain・共有の build/。rm をしない。push しない。
- 全部 PASS の後、file の群ごとの commit、SHA と結果を Q1 に、結果の節と ws.md の行。
