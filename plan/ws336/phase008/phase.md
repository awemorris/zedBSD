<!-- awesome-plan project=zedbsd record=ws336-p008 -->

# ws336-p008: ssh-transport（transport の状態機械・re-key・export と import）

Status: cleared（2026-11-02 11:37 Q1）。前: in-progress（2026-11-02 10:52 P5。11:36 実装済み・試験 PASS、cleared は Q1）。前: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner（Opus。状態機械と安全の要）
依存: p005・p007
WS335 の待ち: p003 経由

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `userland/base/ssh/ssh-transport.h`（冒頭に交換の順・strict・re-key）
- `plan/ws336/design.md` §6.1・§8・§9
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc4253.txt`（SHA-256 `b471173305154f6c…`、全桁は同じ directory の SHA256SUMS）: §4.2、§7、§7.1（KEXINIT の後に送ってよい message）、§7.3、§9、§11
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/draft-ietf-sshm-strict-kex-02.txt`（SHA-256 `c08126dcaaf63958…`、全桁は同じ directory の SHA256SUMS）: §3.2・§3.3
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/draft-ietf-sshm-mlkem-hybrid-kex-10.txt`（SHA-256 `d7e1ff8723466a80…`、全桁は同じ directory の SHA256SUMS）: §2.1
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc4250.txt`（SHA-256 `eb3a9d7c3e1fae53…`、全桁は同じ directory の SHA256SUMS）: §4.1.2（message 番号の範囲）、§4.2.2（disconnect の理由）
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/ssh-transport.c`
- `plan/ws336/tests/unit/test-transport.c`
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- client と server の 2 つの transport を memory の中でつなぐ（pending → input）。host_key と sign は試験の中の鍵（ssh-key）、check_host_key は受ける・拒む
- 初回の交換の後に session_id が両方で同じ、upper の message の往復（5・50・94・200）
- 相手が始めた re-key の間（自分の KEXINIT を送った後、自分の NEWKEYS の前）の `ssh_transport_send` が held に溜まり、自分の NEWKEYS の後に順に出る。held と出力が満ちると ENOBUFS で何も積まれない
- re-key: tick で 1 時間を越える・byte の上限を越える（上限は試験で到達しにくいので、packet の counter を直に大きくして tick）で KEXINIT、相手からの re-key、re-key の間の upper の message が held に溜まり NEWKEYS の後に順に出る、session_id は変わらない
- strict: 最初の受けが IGNORE で EPROTO、初回の交換の中の DEBUG・IGNORE・SERVICE_REQUEST・2 回目の HYBRID_INIT で EPROTO、相手の印が無いと EPROTO と DISCONNECT 3、seq の 0 への戻り
- 相手の識別の行: 前置きの行（client だけ）、65 行目で EPROTO、`SSH-1.5-` で EPROTO、253 文字ちょうど
- 交換の誤り: 署名の 1 bit、host key の拒否（EACCES と DISCONNECT 9）、S_REPLY の長さ、KEXINIT の一覧の不一致で DISCONNECT 3
- UNIMPLEMENTED: 番号 8・128 の message に seq 付きの UNIMPLEMENTED が返る。EXT_INFO（7）は捨てる。交換の外の 30・31 と、段に合わない 21 は EPROTO
- DISCONNECT の受けで ECONNABORTED と理由
- export → import の往復で通信が続く（鍵・seq・残りの input）、import の壊れた入力（各欄の長さ・role）で EINVAL
- 乱数の入力: 交換の各段の後で random な message を送り込み、落ちずに誤りで終わる

## 4. 注意

- header にない内部の状態（kex_step の値）は .c が決める。header の struct の field は変えない。
- 交換の間の upper の message の受け: 相手の KEXINIT を受ける前の upper の message は届ける（RFC 4253 §7.1）。
- disconnect の後の pending の出力は残す（呼び手が書いて閉じる）。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、この Phase の物でない .c、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/run-unit.sh transport`（この Phase の試験）
- `sh plan/ws336/tests/run-unit.sh all`（それまでの全部）
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`（基点は [ws.md](../ws.md) の「header の基点」の行。cc と clang で警告 0、header が基点から不変）
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

2026-11-02 11:36（host の `date`）。**実装済み・試験 PASS**（cleared は Q1 が決める）。

### 作った物・変えた物

- `userland/base/ssh/ssh-transport.c`（骨格の置き換え）: 識別の行、KEXINIT・hybrid・NEWKEYS の段、strict、generic な message、held、re-key（tick）、disconnect、export・import。
- `plan/ws336/tests/unit/test-transport.c`（128389 checks）: client と server を memory の中でつなぎ、壊れた相手は試験が `wire.h` で組んだ鍵なしの packet（RFC 4253 §4.2・§6・§7.1・§11、strict §3.1、hybrid §2.2）か、相手の packet 層で直に枠付けした鍵ありの packet で演じる。期待の値は仕様の形と `wire.h` で組んだ bytes だけ（他の実装の出力は使っていない）。
- **header の変更（Q1 が承認、2026-11-02）**: `ssh-transport.h` の `struct ssh_transport` に `uint8_t c_init[SSH_HYBRID_INIT_SIZE];` を足し、struct の comment に「client が送った C_INIT を reply が来るまで H のために持つ、交換の他の値と一緒に wipe」を足した。H は C_INIT を含むが、header には置き場が無かった（`struct ssh_hybrid_client` は鍵だけで C_INIT を持たず、ssh-crypto に C_INIT を作り直す口も無い）。struct を使う他の module は無い（grep で確かめた。p015 以降は未作成）。header だけの commit `0e9b68f948` を新しい基点として `BASE=0e9b68f948 sh plan/ws336/tests/check-skeleton.sh` → PASS。`BASE=d4ab182d2` では ssh-transport.h だけが変わったとして FAIL（想定どおり。他の header は d4ab182d2 から不変）。**ws.md の「header の基点」は Q1 が 0e9b68f948 に直す**（この Phase では ws.md のその行を変えていない）。

### 実行した command と最後の行（2026-11-02 11:3x）

- `sh plan/ws336/tests/run-unit.sh transport` → `test-transport: PASS (128389 checks)` / `run-unit: PASS`
- `sh plan/ws336/tests/run-unit.sh all` → 14 試験すべて PASS（buf 323・channel 1603651・cipher 1064・common 2316・crypto 1656・hostfile 736314・kex 402989・key 2771・packet 369168・privsep 947・sftp 1836・tools 13・transport 128389・ttymodes 20682）、最後の行 `run-unit: PASS`
- `BASE=0e9b68f948 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`
- `git diff --check HEAD -- <この Phase の 2 file>` → 出力なし（rc 0）
- cc と clang の `-std=c89 -Wall -Wextra -Werror -Wpedantic` で ssh-transport.c が警告 0（check-skeleton の中と、手で 1 回）。

### 試験の内容

- 初回の交換: 両方 RUNNING、session_id が同じ、client の check_host_key が見た blob が `wire.h` で組んだ RFC 8709 §4 の blob と一致、server の host_key・sign が 1 回ずつ、strict で 4 つの seq が 0、両方向の鍵が対になる、k・h・c_init が wipe 済み、upper の 5・50・94・200 と 6・60・94・255 の往復と順序と seq。
- held: VERSION の間の send が held になり NEWKEYS の後に出る。server が始めた re-key で client が自分の KEXINIT の後・NEWKEYS の前に送った 5 通が出力を増やさず held に溜まり、NEWKEYS の後に順に届く（server の側も同じ）。held が 4 MiB ちょうどまで入り（32772 bytes × 127 通と残りぴったりの 1 通）、1 byte 多いと ENOBUFS で held の大きさが変わらない。4 MiB の held は出力の上限まで送り、残りは written で出る。RUNNING で held が残る間の send は後ろに並ぶ。
- 出力: 上の layer の message は出力の上限（`SSH_PACKET_OUTPUT_MAX` − 予約 64 KiB）で ENOBUFS（境界は payload と最大の枠 37 bytes がちょうど入るか）。予約の中で transport の UNIMPLEMENTED と KEXINIT は出る。予約も尽きると答えを積めずに EMSGSIZE で閉じる。
- re-key: tick で 3599 秒は無し・3600 秒で開始、時計の戻りは無し、交換の間の 2 回目の tick は無し。4 つの counter（送り・受けの bytes と packets）が上限ちょうどで開始し 1 少ないと無し。server の counter でも。session_id 不変、鍵が変わる、kex_time が tick の時刻。自分の交換の間に相手の KEXINIT 前の upper は届く。相手の KEXINIT と NEWKEYS の間の upper は UNIMPLEMENTED で捨て、交換は続く。
- strict: 最初の受けが IGNORE で EPROTO と DISCONNECT 2。KEXINIT の後の DEBUG・IGNORE・SERVICE_REQUEST（server）で EPROTO。2 回目の HYBRID_INIT（1 回目は REPLY・NEWKEYS が出る）、2 回目の KEXINIT、2 回目の REPLY、REPLY の前の NEWKEYS、KEXINIT の前の HYBRID_INIT で EPROTO。印の無い KEXINIT・逆の側の印で EPROTO と DISCONNECT 3。seq の wrap（受け・送り）を初回の交換の中で EPROTO、0xfffffffe では続く、初回の後の wrap は誤りでない。
- 識別の行: client が 64 行の前置きを飛ばして交換できる、65 行目で EPROTO、server の前置きは EPROTO、`SSH-1.5-` は EPROTO と DISCONNECT 8、`SSH-2.0`（dash 無し）・`SSH-2.00-` は EPROTO、`SSH-1.99-` は可、LF だけの終わりは可、253 文字ちょうどは可・254 は EMSGSIZE、NUL は EINVAL、途中の行は EAGAIN で待つ。
- 交換の誤り: 署名の 1 bit（EBADMSG、DISCONNECT 3、session_id 無し）、間違って署名する server、ML-KEM 暗号文の 1 bit（K が違い署名で EBADMSG）、host key の拒否（EACCES と DISCONNECT 9、check は 1 回）、check の他の誤り（EIO と DISCONNECT 2）、S_REPLY の 1 byte 短い・長い（EINVAL、DISCONNECT 3、check_host_key を呼ばない）、別の種類の host key（ENOTSUP、DISCONNECT 3）、X25519 の全 0 の点（EINVAL、DISCONNECT 3、check の後）、C_INIT が 1 byte 短い（server が EINVAL と DISCONNECT 3、sign を呼ばない）、ops が無い client・server（EINVAL）、host_key・sign の失敗（EIO）。KEXINIT の一覧の不一致（cipher・kex）で ENOTSUP と DISCONNECT 3、理由の文に「no matching encryption…」。壊れた KEXINIT は EINVAL と DISCONNECT 2。NEWKEYS の後ろに 1 byte は EINVAL。
- first_kex_packet_follows: 外れの推測の次の packet を捨てる（1 つだけ）、当たりなら捨てない。
- UNIMPLEMENTED: 番号 8・128・0・19・22・29・191 に、受けた packet の seq（strict の reset 後の最初は 0）を持つ UNIMPLEMENTED が返る（`wire.h` で組んだ `3, uint32 seq` と一致）。2・3・4・7（EXT_INFO）は答え無しで捨てる。RUNNING の 30・31・21・32・40・49 は client でも server でも EPROTO。鍵なしの空の packet は EINVAL。
- DISCONNECT: ECONNABORTED、code と理由（制御文字と DEL は `?`）、以後の next・send・input は EPIPE。送る説明は 256 bytes で切る（wire の長さで確かめた）、受けた理由は 255 文字に切る。壊れた DISCONNECT も ECONNABORTED（code 0、理由は空）。2 回目の disconnect は何も積まない。初回の交換の最初の packet の DISCONNECT も ECONNABORTED と理由。tag の 1 bit で EBADMSG と DISCONNECT 5（鍵ありで読んで確かめた）。
- export・import: export の bytes が `wire.h` で組んだ下の layout と byte ごとに一致。free して import し、受けて未処理だった 2 通が出て、往復と re-key が続く（check_host_key は 2 回目）。server も export・import の後に re-key と送り。VERSION・pending あり・交換の間・held ありは EBUSY で out に何も書かない。壊れた入力: format・role・空の行・`SSH-1.5-`・NUL 入り・254 文字・peer の行が `OpenSSH`・session_id 31・33・strict 0・2・鍵 63・65（両方向）・wrapped 2（両方向）・余りの 1 byte、全ての短い prefix、input が SSH_PACKET_INPUT_MAX + 1 で EINVAL（ちょうどは可）。失敗の後は transport が 0 に wipe されている。
- 乱数: 6 つの段（何も無し・行の後・KEXINIT の後・client の half の後・reply の後・RUNNING）× 1000 回、乱数の行・鍵なしの枠・乱数の bytes を両側に入れ、EAGAIN か、閉じる誤りだけ。RUNNING の接続に乱数の message（番号も中身も乱数、相手の鍵で枠付け）を 20000 回。ASan・UBSan で落ちない。

### わざと壊す確かめ

`build/tmp/p008-mut/mut.py` と `build/tmp/p008-mut2/mut.py`（同じ 77 通り、mut2 は書き方の直しの後の source に対して）で、ssh-transport.c の**複写**（`build/tmp/p008-mut*/mNN/ssh-transport.c`）を壊して試験を build・実行した。worktree の file は書き換えていない（実行の前後の SHA-256 が同じことを script が確かめた、`source unchanged: True`）。上限: RLIMIT_FSIZE 64 MB、RLIMIT_CPU 400 秒、RSS 6 GB か 150 秒で kill、並列 4。

- 1 回目: 77 通り中 72 が FAIL。生き残った 4 つに試験を足した（DEL を `?` にしない → 理由に 0x7f、flush が予約を見ない → 4 MiB の held を written で出して出力が上限の内、c_init を消さない → 交換の後に c_init が全 0、枠の 37 bytes を見ない → 境界のちょうどの大きさで ENOBUFS と 0）。
- 2 回目: 77 通り中 75 が FAIL。「枠の 37 bytes を見ない」は 1 byte の message では前の確かめに隠れていたので、境界の試験を足して `build/tmp/p008-mut3/test47` で FAIL を確かめた。
- 残る 1 つ（最初の受けが KEXINIT かの確かめを外す）は同値: KEXINIT の前の NEWKEYS・HYBRID_INIT・HYBRID_REPLY は各段の確かめで、他の番号は初回の交換で許す番号の確かめで、どれも EPROTO になる（DISCONNECT の文だけが違う）。仕様（strict §3.2）をそのまま書いた確かめとして残す。
- 主な変更の例: 前置きの行数の `>=`、server の前置き、版の確かめ、prefix の dash、1.99 の綴り、行の上限 +1、wrap の送り・受け、推測の破棄、初回の交換の番号の集合、kex の番号の範囲、相手の交換の判定、UNIMPLEMENTED の seq と byte 順、予約の範囲 128・191 の境界、upper の 50、理由の切り・制御文字、送る説明の長さ、2 回目の KEXINIT・HYBRID_INIT・REPLY・NEWKEYS、HYBRID_INIT の role、S_REPLY の長さ、DISCONNECT の code（3・5・8・9）、check の誤りと署名の誤りを無視、session_id の上書き（server・client）、受けの鍵の letter、strict の seq の reset（送り・受け）、NEWKEYS の大きさ、hold の 3 つの条件、held の上限、flush の offset、予約 0、自分の message の ENOBUFS、tick の 4 つの counter と時間の境界と時計の戻りと交換中、kex_time、export の held・方向の順、import の role・strict・input の上限・flag・行の長さ・余り・counter、ECONNABORTED、EXT_INFO、written の交換中の flush、send の 49・大きさ、空の packet、H の KEXINIT の取り違え。

### 自分で決めた点（header が黙っている所、細かくした所）

- **kex_step**: 6 つの bit（KEXINIT・hybrid・NEWKEYS のそれぞれ送り・受け）。両方の NEWKEYS が揃うと 0 に戻し、初回なら initial_done 1・state RUNNING、kex_time = now、k・h・c_init を wipe、KEXINIT の buffer を空にする。
- **送る順**: 両側とも相手の識別の行を受けてから KEXINIT（header の通り）。client は相手の KEXINIT を受けたらすぐ HYBRID_INIT。server は HYBRID_INIT を受けたら REPLY と NEWKEYS を続けて送る。送りの鍵は NEWKEYS を積む前に導出する（失敗で鍵の無い NEWKEYS を残さない）。
- **client の確かめの順**（design §6.1）: REPLY の parse → S_REPLY の長さ（EINVAL・3）→ host key の blob が ssh-ed25519（ENOTSUP/EINVAL・3）→ check_host_key（EACCES は 9、他の誤りは 2 で渡す）→ K（EINVAL は 3）→ H → 署名（誤りはそのまま、EBADMSG など、3）→ session_id → NEWKEYS。
- **DISCONNECT の受け**: どの段でも（初回の交換の最初の packet でも）ECONNABORTED と code・理由。strict §3.2 は切ることを求めているので、切った上で理由を残す（相手が「no matching …」で切る時の client の表示のため）。壊れた DISCONNECT も読めた所までで ECONNABORTED。理由は 255 文字までに切り、0x00〜0x1f と 0x7f を `?` にする。返事の DISCONNECT は送らない。
- **CLOSED の後**: input・next・send は EPIPE。disconnect は何もしない。閉じた時に hybrid の half・c_init・k・h・held を wipe して捨て、pending の出力は残す。
- **番号の扱い**: 0 も「どの layer でもない」として UNIMPLEMENTED。相手の交換の間（相手の KEXINIT の後・NEWKEYS の前）の 5・6 も 50 以上と同じく UNIMPLEMENTED。32〜49 はいつでも EPROTO。send は header の通り 5・6・50〜255 を受ける（128〜191 も送れるが、受け側は UNIMPLEMENTED）。空の packet は EINVAL と DISCONNECT 2。
- **first_kex_packet_follows の外れ**: KEXINIT の次の packet を、番号に関わらず 1 つ捨てる（RFC 4253 §7「the next packet」）。
- **hold**: 自分の交換の間（KEXINIT 送り〜NEWKEYS 送り）に加え、VERSION の間（鍵の無いうちに upper を平文で出さない）と、held が残る間（順を保つ）も held にする。held は uint32 の長さと payload。flush は出力の予約の外に入る分だけ送り、残りは `ssh_transport_written` が出す。
- **出力の予約**: 上の layer の send・flush は出力が `SSH_PACKET_OUTPUT_MAX − 64 KiB` を越える分を ENOBUFS で断り、transport 自身の KEXINIT・REPLY・NEWKEYS・UNIMPLEMENTED・DISCONNECT のために 64 KiB を残す。自分の message が出力の上限で積めない時は、接続を続けられないので EMSGSIZE で閉じる（ENOBUFS は back-pressure の意味なので使わない）。
- **tick**: 4 つの counter と時間の上限は tick でだけ見る。now が kex_time より前（時計の戻り）なら何もしない。KEXINIT を送れない誤りは接続を閉じる（DISCONNECT 2 を積む）。
- **send の EIO**（padding の乱数の失敗）は DISCONNECT 2 で閉じる（design §9）。ENOMEM は閉じずに返す。
- **前置きの行**: 1 行も識別の行と同じ 253 文字の上限（`ssh_packet_take_line` の capacity）。そのため 64 行 × 255 bytes < 16384 で、bytes の上限（`SSH_VERSION_PRELUDE_BYTES`）は届かない（確かめは残す）。
- **ops の欠け**: server の host_key・sign、client の check_host_key が NULL なら、使う時に EINVAL と DISCONNECT 2。
- **re-key の host key**: check_host_key は交換ごとに呼ぶ。最初の交換と同じ鍵であることを transport は求めない（struct に置き場が無い）。必要なら呼び手（p015 の ssh）が check_host_key の中で最初の blob と比べる。
- **export の layout**（ssh-transport.c の冒頭に書いた）: byte format 1、byte role、string version_local、string version_peer、string session_id（32）、byte strict（1）、送り・受けの順に string 鍵（64 bytes: K_1・K_2）・uint32 seq・byte wrapped・uint64 bytes・uint64 packets、uint64 kex_time、string 未処理の input。import は全欄を確かめる（行は 1〜253 文字・NUL 無し・`SSH-2.0-` か `SSH-1.99-`、session_id 32、strict 1 だけ、鍵 64、wrapped 0・1、input は SSH_PACKET_INPUT_MAX まで、余り無し）。失敗の時は transport を free（0 に wipe）して EINVAL、feed の失敗は ENOMEM。import した transport の now は引数、kex_time は export の値（同じ機械の monotonic な時計の前提）。export は out が secret かを確かめない（header の「out must be secret」は呼び手の約束として扱った）。

### 未実施・限界

- crypto library と allocation の失敗（EIO・ENOMEM）の経路は host で起こせないので未実施（読んで確かめた）。coverage（gcov、`build/tmp/p008-cov`）で未実行の行はこれらと、到達しない防御（2 回目の NEWKEYS の確かめ、flush の中の ENOBUFS、前置きの bytes の上限）だけ。
- host の OpenSSH との相互運用は p015 以降で、ここでは未実施。libcrypto-compat での試験は p021。QEMU・実機は未実施。
- coding-style §14 の checklist で自分の diff を見直した（宣言の位置、各 paragraph の comment、条件の中で関数を呼ばない、成功の return を最後に、goto は共通の後始末の label だけ、など）。全文の見直しは WS の最後の Phase。
- 試験の file は規約の全部には合わせていない（他の試験の file と同じ程度）。
