<!-- awesome-plan project=zedbsd record=ws302-user-questions -->

# WS302 ユーザーへの問い（用語と概念から）

2026-10-31 P5（q1775-i01）。設計の本文は [design.md](design.md)。

## 前置き

- **どの WS か**: WS302「デスクトップスレッドバランサ（DTB）」。
- **どんな WS か**: kernel に、thread の動き（GPU を使うか、syscall なしで忙しいか、terminal の中か、nice）と人の操作（キーボード・マウスの入力）を見て、「この thread は速い core（P-core）向きか、省電力の core（E-core）向きか、どの priority か」を scheduler（sched）に伝える部品を足す。DTB は scheduler ではなく、実際にどの CPU で走らせるかは sched が決める。build の設定 `CONFIG_DTB` で入れ、既定は off（2026-10-27 の決定）。最初の対象は 5330（P-core 2・E-core 8）。
- **今の状態**: 設計の段（p001、P5。第 2 版。敵対的レビュー（blocker 2・major 11・minor 13）を反映済み）。実装はまだ。P/E core の種類を kernel が知る口（WS333）は 2026-10-31 に main に入った。
- **決まっていない事**: 下の 7 つ。Q302-1〜3 は DTB の振る舞いの中心。Q302-4・5 は今の推しで進めても後で変えられる。Q302-6 は UAPI（user から見える kernel の口）なので後で変えにくい。Q302-7 は受け入れの数。実装の順は p002（統計）が Q302-5、p003（sched の口）が Q302-2・3、p004（DTB の本体）が Q302-1・6 の答えの後（どれも答えが推しと違っても直しは小さい）。

## 用語

- **P-core・E-core**: 5330 の CPU の 2 種類の core。P（performance）は速いが電力が多い。E（efficiency）は遅いが省電力。5330 は P が 2 つ（1 つが 2 thread を同時に走らせる SMT で、OS からは 4 つの CPU に見える）、E が 8 つ。
- **sched（scheduler）**: どの thread をどの CPU でいつ走らせるかを決める kernel の部品。
- **priority**: sched が先に走らせる順。zedBSD は 0（最高）〜15（最低）の 16 段で、今は user の thread が全部 8、kernel の中の仕事の thread（USB の入力の処理・画面の監視・memory の追い出しなど）も大半が 8。上の段に走れる thread が居る間、下の段は走らない（厳密な順）。
- **飢え**: 下の段の thread が、上の段の thread が忙しい間ずっと走れない事。
- **nice**: 利用者が process に付ける「遠慮の度合い」（-20〜+20）。大きいほど遠慮する（背景の仕事）。下げる（急ぐ）のは root だけ。今の zedBSD は値を覚えるだけで sched は使っていない。
- **syscall**: program が kernel に頼む呼び出し（file の読み書き・待ちなど）。計算だけを続ける thread は syscall をしない。
- **controlling tty**: その process が繋がっている terminal。shell と、shell から起こした command が持つ。
- **溢れ**: E-core に寄せたい thread が E で待っている時に、空いている P-core が代わりに走らせる事。
- **SMT の兄弟**: 1 つの P-core が同時に走らせる 2 つの CPU。片方が忙しいと、もう片方の速さは落ちる。
- **UAPI**: user の program から見える kernel の口（ioctl・sysctl など）。一度出すと変えにくい。
- **sysctl**: kernel の値を名前で読む UAPI（`hw.cputimes` など）。

## Q302-1 規則の強さの順（ws.md の D2）

- 背景: 規則は「GPU を使う app は速い core」「syscall なしで忙しい thread は遅い core」「terminal の app は UI の応答を落とさない範囲で速い core」「nice」の 4 つ。1 つの thread に 2 つ以上が当たる事がある。例: terminal の中の compiler（terminal の規則と忙しい規則）、game の計算の thread（GPU の規則と忙しい規則）。P は OS から 4 つしか見えないので、全部を速い core にはできない。
- 決まると変わる事: どの thread が P を取り、何が E に行くか。電力と build の時間と game の速さの釣り合い。
- **(A, 推し)** nice → 忙しい計算 → 画面を作る process（compositor）→ GPU を使う app → terminal → 他。GPU を使う app の中でも忙しい計算の thread は E に。terminal は「SMT の兄弟も空いている P がある時に寄せる」（UI の thread の移動を先にする）、人が 60 秒以上操作しない間は止める。
  - **注意（ユーザーの規則との差）**: A では terminal の中の compiler（syscall なしで忙しい）は E に行く。「terminal の app は UI の応答を落とさない範囲で速い core」を、shell・editor・短い command には効かせ、build の計算には効かせない、と読んでいる。shell から起こした command は shell の希望を継ぐ。
- (B) nice → 画面を作る process → GPU を使う app → 忙しい計算 → terminal。game の計算の thread も P に残る（game は速いかもしれないが、P の席が足りず電力も下がりにくい）。
- (C) 画面を作る process → GPU を使う app → nice → 忙しい計算 → terminal（UI が利用者の nice より強い）。
- (D) nice → terminal（能動的に P へ寄せる）→ 忙しい計算 → …（terminal の build も P に張り付く。P が 4 つの 5330 では UI の席が無くなりやすい）。
- (E) A と同じだが terminal を忙しい計算より上に置き、terminal の規則は A と同じ「兄弟も空いた P がある時だけ」。build の compiler も空いた P に寄るが、UI の thread が来れば priority で譲る。build は速くなりうるが、P が空いている間は電力が上がる。
- 推しの理由: nice は利用者の明示の意図なので最初。GPU に仕事を出す thread と compositor の thread は出すたびに syscall を通るので「忙しい」に当たらず、A でも P に残る。忙しい計算を先にすると、電力を下げる目標（S2 の場面）がそのまま効く。build の速さは Q302-2 の溢れで守る。

## Q302-2 忙しい計算の thread が空いた P-core に溢れてよいか

- 背景: E-core は 8 つ。`make -j12` のように忙しい thread が 8 より多いと、E で順番を待つ thread が出る。その時 P が空いていれば、P で走らせると build は速いが、電力は増える。
- 決まると変わる事: build の時間（受け入れ A5: off に比べ 10% 以内の悪化）と、build の間の電力。
- **(A, 推し)** nice 0 の忙しい thread は、空いている P が 1 tick（1 ms）以上待った thread を盗んでよい（溢れる）。nice > 0 の thread は溢れない（飢えの防ぎの 100 ms を越えた時だけ）。UI の thread は priority が上なので、P に戻ってくれば溢れた thread より先に走る。溢れた thread は、E に空きが出来た時に E へ戻す。
- (B) 溢れない（忙しい thread は E だけ。電力は下がるが build は遅くなる）。
- (C) 人が操作していない（60 秒以上）間だけ溢れる。
- 推しの理由: 背景の計算 4 本の場面（電力の受け入れ A4）では E に余裕があるので溢れは起きず、電力の目標と build の時間の目標を両方守れる。nice > 0 は利用者が「背景」と言った物なので溢れさせない。

## Q302-3 priority の段、nice の段と飢えの防ぎ、DTB が on の時だけにするか（ws.md の D3 の続き）

- 背景: 2026-10-27 に「nice は sched の priority にも効かせる」「GPU を使う app は応答の priority を上げる」と決まった。今の sched は厳密な順で、aging（待った thread を上げる仕組み）が無い。kernel の中の仕事の thread（USB の入力の処理・画面の監視など）の大半は 8 で、user の thread を 8 より上に上げると、入力から画面までの kernel の仕事そのものを待たせる（設計の第 1 版はそうしていて、レビューで止めた）。下げた thread は、上の段が忙しい間は全く走れない（飢え）。
- 決まると変わる事: GPU を使う app・nice を付けた仕事の進み方、kernel の thread と user の thread の順、DTB が off・`CONFIG_DTB=n` の kernel の振る舞い。
- **(A, 推し)** DTB が on の時、user の thread の既定を 10 に下げ、上げは 8・9 で行う（kernel の 8 より上には誰も置かない）: compositor（画面を作る）8、GPU を使う app 9（ただし CPU を半分以上使い続ける thread は上げない。GPU に 1 回仕事を出すだけで上がる抜け道を塞ぐ）、普通・terminal・忙しい計算 10、nice -20〜-10 → 8、-9〜-1 → 9、1〜9 → 11、10〜20 → 12。下げた user の thread が 100 ms 待ったら 1 回だけ（10 ms）走れるように上げる（最悪でも CPU の約 9% は回る。kernel の段には上げない）。kernel の中に居る user の thread は 10 より下げない（kernel の lock を持ったまま待たされるのを防ぐ）。**DTB が on の時だけ**効かせる（DTB が off・`CONFIG_DTB=n` の kernel は今と同じ、全部 8）。
  - 変わる事: DTB が on の時、kernel の既定の thread（8）は user の既定（10）より先に走る（今は同じ段で順番）。kernel の thread が長く忙しい場面では user が待たされうる（飢えの防ぎで 100 ms ごとに 10 ms は走る）。QEMU と 5330 の試験で確かめる。
- (B) 上げをしない: user の既定は 8 のまま、下げ（忙しい計算 9、nice > 0 は 10・11）だけで GPU の app を相対に上にする。kernel との順は今と同じ。nice < 0 は core の選び（速い core）にだけ効き、priority には効かない（2026-10-27 の決定の半分）。
- (C) A の段で、nice だけは DTB に依らず常に効かせる（sched の既定の振る舞いが変わる）。
- 推しの理由: 決定（nice も GPU の app も priority に効かせる）を満たしつつ、kernel の worker を待たせない。段を少なく保つと飢えの防ぎの働く場面が限られ、結果が読みやすい。DTB の on・off で比べる評価（p006）の時に、off が今の kernel と同じである方が比べが正しい。

## Q302-4 compositor から「前面の app」を kernel に知らせる口を作るか（ws.md の D4）

- 背景: kernel は入力の時刻は分かるが、入力がどの app に行ったかは分からない（それを知るのは compositor）。compositor から「今の前面はこの process」を kernel に知らせる口を作れば、前面の app を P に置ける。作るには UAPI（`/dev/system` の ioctl など）と、Keiland の backend（zedBSD の物、Linux・FreeBSD は空の関数）の変更が要る。
- 決まると変わる事: A なら WS302 の仕事が増える（約 3 LW）。B なら入力は「人が操作中か」だけに使う。
- (A) v1 で作る。
- **(B, 推し)** v1 では作らない。5330 の評価（p006）で応答の受け入れ（A1・A2）が通らず、前面の情報で直る見込みがある時だけ、条件つきの Phase（p005）として改めて聞く。
- 推しの理由: GPU に仕事を出す app と compositor は前面かに依らず P に置くので、前面の app の大半は既に P に居る。UAPI は後で変えにくいので、数で要ると分かってから作る。

## Q302-5 thread の統計を `CONFIG_DTB` の build の時だけ取るか（ws.md の D5）

- 背景: DTB には thread ごとの数（走った時間、syscall の数、syscall の後に走り続けた時間、起こされた数）と、process ごとの GPU の最後の時刻、最後の入力の時刻が要る。取る代価は tick・syscall・起床ごとに数個の加算で小さい。
- 決まると変わる事: `CONFIG_DTB=n`（既定）の kernel に統計の field と加算が入るか。
- **(A, 推し)** `CONFIG_DTB=y` の build の時だけ取る（n の kernel は今と同じ）。y の build で `dtb=off` で起動しても統計は取り、on・off の比べと負担の計測（受け入れ A6）に使う。
- (B) 常に取る（将来 `top` などで thread ごとの数を見せられる）。
- 推しの理由: 既定の kernel を変えないので、負担の心配が無い。thread ごとの数を常に見せたくなったら、その時に B に広げられる（UAPI を足す時に一緒に決める）。

## Q302-6 観察の UAPI

- 背景: 5330 の評価では、どの CPU が P か E か、DTB が何回 thread を寄せたか、忙しい thread がどこで走ったかを、user の script から読む必要がある（kernel の log を読んで判定しない規則のため）。
- 決まると変わる事: UAPI が 1 つ増える（読みだけ）。
- **(A, 推し)** 読みだけの sysctl `kern.dtb`: 有効か、hybrid か、人の操作の状態、周期の数と時間、規則ごとの thread の数、移動・手渡しの数、CPU ごとの種類。process・thread ごとの印は入れない。形の案（720 byte、32 bit と 64 bit で同じ）は [proposed/include/uapi/dtb-sysctl.h](proposed/include/uapi/dtb-sysctl.h)。
- (B) A に、`/dev/system` の process の情報（予備の field）へ process ごとの DTB の印を足す。
- (C) UAPI を足さない（評価は CPU ごとの時間 `hw.cputimes` だけで行う。CPU の種類が user から読めないので、5330 の番号の並びを別に調べて決め打つ）。
- 推しの理由: 評価に要る最小。読みだけなので書きの権限の問題が無い。process ごとの印は、使い道がはっきりしてから B に広げられる。

## Q302-7 受け入れの数

- 背景: ws.md の受け入れの案は「応答を落とさない」「電力が下がる」「build は 10% 以内の悪化」で、数は p001 で決める事になっていた。5330 で DTB の on と off を交互に 5 回ずつ測る（design.md §6.3）。
- 決まると変わる事: p006 の合否。
- **推し（design.md §6.4）**: 応答（cursor の遅れ・打鍵から表示）の中央値と 95 パーセンタイルが on ≤ off × 1.05 ＋ 1 ms、GPU の app の fps が on ≥ off × 0.95、背景の計算 4 本（8 本だと電力の上限に張り付いて差が見えない見込み）の忙しい時間の 90% 以上が E、その時の package の電力が on ≤ off × 0.95、build の時間が on ≤ off × 1.10、DTB の負担は off との差が 2% 以内（測りの揺れの中）で周期の処理が 1 ms 未満、30 分の負荷の試験で止まらない。
- 別の数にしたい所があれば、その項目だけ答えてください（例: 電力は 10% 下がる事を求める、など）。
