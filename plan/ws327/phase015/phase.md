<!-- awesome-plan project=zedbsd record=ws327-p015 -->

# ws327-p015 解説: 8 章の例を libkeiland の thread に、network の節と例 `08-network.c` を足す

- 親: [WS327](../ws.md)。設計: [design.md](../design.md) §3・§4・§7・§13。担当: 実装の担当（Sonnet 5.5 high）。依存: p007（API は凍結済みなので
  app の移行の Phase と並べてよい）。
- Status: planned
- Disposition: normal

## 1. 読む物

- `AGENTS.md` §4・§10、`plan/coding-style.md` の全文（例の C にも適用）。
- design.md §3.2（thread の約束）・§4.3（network の約束、特に poll の ready・waker の約束「poll が返った後に queue を見る」）・§7（解説の方針）・§13。
- header（仕様、変えない）: `keiland_thread.h`・`keiland_socket.h`・`keiland_app.h`（`kl_app_post`）。
- 解説: `docs/howto/keiland-programming.md` の冒頭の「Examples」の表と 82 行あたりの C11 threads の注意、8 章（4400 行あたり〜9 章の前）全て。
- 例: `docs/howto/keiland-examples/08-worker.c`・`08-language.c`。

## 2. 触ってよい file

| file | 何を |
| --- | --- |
| `docs/howto/keiland-examples/08-worker.c` | §3.1 |
| `docs/howto/keiland-examples/08-language.c` | §3.1 |
| `docs/howto/keiland-examples/08-network.c`（新） | §3.2 |
| `docs/howto/keiland-programming.md` | §3.3 |
| `plan/ws327/phase015/phase.md` の「結果」 | 記録（`plan/ws327/ws.md` は触らない: 並べた Phase の merge が衝突するので Q1 が直す） |

`docs/` から `plan/` へ link しない（docs は目標の設計の文書）。`docs/howto/keiland-examples/*.png` は作らない（08-network の写真は
p016 で P2 が guest で撮る。それまで本文に画像の行を置かない）。`plan/ws314/tests/doc-examples.sh` は閉じた WS の物で変えない。

## 3. 何を

### 3.1 08-worker.c・08-language.c の thread を libkeiland の層に

- `<threads.h>` を外し、`thrd_t` → `struct kl_thread *`、`thrd_create`（`int (*)(void *)`）→ `kl_thread_start`（`void (*)(void *)`、
  `struct kl_thread_options` に名前: `"primes"`・`"language"`）、`thrd_join(t, NULL)` → `kl_thread_join(t)`、`mtx_t` → `struct kl_mutex`
  （`mtx_init(&m, mtx_plain)` → `kl_mutex_init(&m)`）、`cnd_t` → `struct kl_cond`、`thrd_success` との比較は 0 と、`thrd_error` などの
  返しは errno の値に。`<stdatomic.h>` と atomic の flag はそのまま（design §3.6）。
- design §13.2 の表に従う。振る舞い（数える範囲、post の code、最新の変更が勝つ形）は変えない。
- `<keiland/keiland.h>` が `keiland_thread.h` を含むので include を足さなくてよい。

### 3.2 08-network.c（新、`08-worker.c` から育てる）

- 窓は 08-worker と同じ形（Start・Stop の button と、数と時間の行）。
- Start: network の thread を 1 本（`kl_thread_start`、名前 `"network"`）。thread は loopback（`kl_address_loopback(KL_ADDRESS_IPV4, 0, ...)`）に
  bind・listen する socket と、それに非 blocking で connect する socket を開き、`kl_socket_poll` で listen の READABLE（accept）と connect の
  WRITABLE（`kl_socket_connect_result`）を同時に待つ。繋がった後、「`ping N\n`」を 100 回往復させる（受けた側は同じ行を返す）。
  往復ごとに `kl_socket_poll` で、相手の socket と waker（Stop）を待つ。10 往復ごとに進みを `kl_app_post` で窓に、終わりに回数と
  平均の時間（μs、単調の時計）を post。
- Stop: 窓が止めの flag（atomic）を立て `kl_socket_waker_ring`。thread は poll から返った後に flag を見て終わる（design §4.3 の約束）。
- 終わり: thread の最後の post（FINISHED）を受けた窓が `kl_thread_join` してから、waker を `kl_socket_waker_close`（close は join の後）。
- 外の network が無い guest でも動く（loopback だけ）。`kl_socket_resolve` は使わない（blocking の名前の解決は thread の中で、と本文に書く）。
- 規約の全文に従う（例も source）。copyright の頭は他の例と同じ。

### 3.3 keiland-programming.md

1. 冒頭の「Examples」の表に `08-network.c` の行（Chapter 8、What it shows「Two sockets on the loopback, waited for together on a network thread;
   Stop rings a waker」、Grows out of 08）。
2. 82 行あたりの「The programs of chapter 8 use C11 threads ...（FreeBSD の `-lstdthreads`）」を、libkeiland の thread（`keiland_thread.h`）を使い
   追加の library は要らない、の意味に直す。
3. 8 章: 例の写し（`<!-- example: keiland-examples/08-worker.c -->` の後の ```c の block）を新しい file と byte で同じに。
   「The threads are standard C11 threads ...」を「libkeiland's threads (`keiland_thread.h`) ...」に。本文の `thrd_create`・`thrd_join`・
   `mtx_*`・`cnd_*` の引用と説明を新しい code に。`#L` の行の参照を全て新しい行に直す（§4 の 2）。
4. 8 章に節「### Talking over the network」（「When only the latest answer counts」の後）: `08-network.c` の写し（marker
   `<!-- example: keiland-examples/08-network.c -->` と ```c の block、file と byte で同じ）、読み方（socket・address・poll の ready・
   waker・非 blocking の connect）、規則（socket の待ちは app の thread でしない、結果は post、Stop は waker で、close は join の後、
   名前の解決は blocking なので thread で）、落とし穴（`kl_socket_connect_result` は待ちの後だけ、EAGAIN は EWOULDBLOCK と比べない、
   receive の 0 は stream の終わり、1 つの socket を 2 つの thread で使うのは送りと受けに分けた時だけ）。header への link
   （`../../userland/desktop/include/keiland/keiland_socket.h`）。

## 4. 確かめの command

1. 例の compile と本文との一致: Linux の libkeiland を作り（`make -j16 -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws327-p015-linux build/ws327-p015-linux/lib/libkeiland.so`）、
   `sh plan/ws314/tests/doc-examples.sh build/ws327-p015-linux/lib` → 最後の SUMMARY に FAIL 0（全ての例を clang・gcc で `-Werror`、link、
   本文の写しが file と byte で同じ）。
2. 行の参照: `python3 -I plan/ws327/tests/doc-lines.py 08-worker.c 08-language.c 08-network.c` → `ws327-doc-lines: PASS`。さらに出力の
   各参照の first・last の行が、本文がその参照で説明している code（直後の ``` の block や「(`function`)」）と合う事を自分で全て読んで確かめる。
3. 08-network を host で動かす事はしない（窓が要る）。guest の写真は p016。
4. `sh plan/ws327/tests/check-base.sh p015` → PASS。

## 5. 突然変異の確かめ（§13.6）

1. 本文の 08-worker.c の写しの 1 行を変える → doc-examples.sh が FAIL。
2. `#L` の参照の 1 つを file の行の数より大きくする → doc-lines.py が FAIL。
3. 戻して `cmp`。

## 6. 注意

- 本文は英語。用語は既存の章と同じ（post、record、the application's thread）。
- 本文の行の参照は 08-worker で 19 個、08-language で 14 個ある。例を直すと全部ずれる: doc-lines.py で一覧を出して直す。
- 例の写しの block は byte で同じでなければならない（tab、末尾の改行）。file から写す（手で打たない）。
- `08-network.c` は既存の例の名前・形（`FONT`、`kl_ui`、event の関数）を踏襲する。

## 7. 受け入れ（Q1 が見る）

§4 の 1・2・4 が PASS、§5 の 1・2 が捕まる、規約の見直し、`git commit -m WIP -- docs/howto/keiland-programming.md docs/howto/keiland-examples/08-worker.c
docs/howto/keiland-examples/08-language.c docs/howto/keiland-examples/08-network.c plan/ws327/phase015/phase.md`（ws.md は Q1）（新しい file は先に `git add`）。

## 結果

実行日: 2026-10-30（host の `date`）。

- 08-worker.c: `<threads.h>` を外し、`thrd_t` → `struct kl_thread *`（join の後に NULL）、`thrd_create` → `kl_thread_start`（名前 `"primes"`、NORMAL）、
  `thrd_join` → `kl_thread_join`、thread の関数を `static void primes_count(void *)` に。atomic はそのまま。
- 08-language.c: `mtx_t`/`cnd_t`/`thrd_t` → `struct kl_mutex`/`struct kl_cond`/`struct kl_thread *`、`mtx_*`/`cnd_*` → `kl_mutex_*`/`kl_cond_*`、
  thread は名前 `"language"`、`language_reader` は `static void`（`return NULL` 相当は `return;`）。振る舞いは同じ。
- 08-network.c（新）: 08-worker から育てた。window は同じ形。network の thread は loopback の listen socket と非 blocking の connect socket を
  `kl_socket_poll`（listen の READABLE・connect の WRITABLE・waker）で同時に待ち、accept と `kl_socket_connect_result` の後に `ping N` を 100 回往復
  （各往復は `network_wait` で socket と waker を待つ）。10 往復ごとに進みを `kl_app_post`、終わりに回数と平均の μs（`CLOCK_MONOTONIC`）を post。
  Stop は flag を立ててから `kl_socket_waker_ring`、thread は poll が返った後に flag を見る。窓は FINISHED を受けて `kl_thread_join` してから
  `kl_socket_waker_close`。`kl_socket_resolve` は使わず本文で説明。
- keiland-programming.md: 章の一覧・Examples の表（`08-network.c` の行）・導入の C11 threads の注意を libkeiland の thread に・8 章の例の写し 2 つと
  抜粋・本文の `thrd_*`/`mtx_*`/`cnd_*` の説明・`#L` の参照（08-worker 19、08-language 14 の全て）を新しい行に。節「### Talking over the network」
  （例の写し、読み方、規則、落とし穴）を「When only the latest answer counts」の後に足した。画像の行は置いていない（p016）。

確かめ（worktree の root）:

- `make -j16 -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws327-p015-linux build/ws327-p015-linux/lib/libkeiland.so` → exit 0、warning 0。
- `sh plan/ws314/tests/doc-examples.sh build/ws327-p015-linux/lib` → `SUMMARY pass=76 fail=0 not-done=0`（08-worker・08-language・08-network を clang・gcc の
  `-Werror` で compile、link、本文の写しが file と byte で同じ）。
- `python3 -I plan/ws327/tests/doc-lines.py 08-worker.c 08-language.c 08-network.c` → `ws327-doc-lines: PASS`。各参照の first・last を、本文が説明する
  code と照らして読んだ（変えた範囲は元の file との差分で対応を作り、ずれた物は全て目で確かめた）。
- `sh plan/ws327/tests/check-base.sh p015` → `ws327-check-base: PASS`。`git diff --check` は空。
- 08-network の thread 側の動き（窓なし）: 08-network.c を include した使い捨ての試験（scratchpad、git の外）で `network_run` を直に呼び、
  100 往復・error 0・平均 84 μs・約 2.0 秒、別の thread が 300 ms で Stop（flag と ring）すると 15 往復で `ECANCELED`（300 ms）で終わる事を確かめた。
  （窓・post・join と waker の close の経路は host では未実施、p016 の guest で。）
- 突然変異: 本文の 08-worker の写しの 1 行を変える → doc-examples.sh が `FAIL shown 08-worker.c`（fail=1）、`#L34-L36` を `#L34-L9999` にする →
  doc-lines.py が FAIL（範囲が file の外・words と link が不一致）。どちらも戻して `cmp` で一致。

自分で決めた点:

- 往復の間に 20 ms の pause（`PAUSE_MS`）を、waker だけを時間付きで待つ `kl_socket_poll` で入れた（Stop を押す時間を作るのと、時間付きの poll を見せるため）。
  平均の時間には含まれない。
- 進みは worker と同じく kl_app_post の 1 種類（`POST_PROGRESS` / `POST_FINISHED`）。window の post は使わない。
- `network_finish` は終了時に FINISHED を `kl_app_dispatch(app, -1)` で待つ（thread は必ず FINISHED を post する。post が ENOMEM で失敗すると待ち続けるが、
  その時は元の worker と同じく結果が失われる事を例のコメントで述べている）。
- 時刻は `kl_clock_us` でなく `clock_gettime(CLOCK_MONOTONIC)` の小さな関数（別の thread から呼ぶ物の約束が header に無いため）。

未実施: guest での実行と写真（p016）、08-network.png（作らない）、32 bit など他の arch の compile（例は host の clang・gcc のみ）。

Q1 への所見: ws.md の Phase の表は触っていない。
