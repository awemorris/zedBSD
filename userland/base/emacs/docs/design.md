# remacs 設計書

**remacs** — Re-implemented Editing Macros.

Noct VM 上で GNU Emacs の基本機能を再実装するエディタ。

## 1. 目的と非目的

目的:

- Noct VM の実アプリ負荷テスト。長時間・大規模可変データ・対話入力という、
  Webサーバでは踏めない負荷パターンを踏む。
- GNU Emacs の C プリミティブ(DEFUN)のサブセットを Noct ネイティブ API と
  して再実装し、その上に Emacs の基本編集機能を Noct で書く。
- 本物の GNU Emacs をテストオラクルとして使い、期待値を手書きしない
  差分テスト体制を作る。

非目的 (v1 では扱わない):

- MULE / coding-system。内部表現は UTF-8 のみ。
- GUI。ただし差し替え可能な境界は最初から切る。
- elisp 実行互換。互換なのは「関数名と観測可能な挙動」だけ。
- ミニバッファ補完、複数フレーム、フェイス継承などの Emacs 深部。

ライセンス方針: GNU Emacs のソースコード(C も Lisp も)は参照しない。
利用するのは公開インタフェース(関数名・引数・ドキュメント化された挙動)と、
黒箱としての実行結果のみ。クリーンルームである旨を NOTICE に明記する。

## 2. リポジトリ構成

```
remacs/
  NoctLang/               # git submodule (upstream追従)
  CMakeLists.txt          # noct をサブディレクトリとしてビルド、CLI拡張をリンク
  src/
    api-term.c            # Term.*   端末抽象 (ANSI/VT100直実装)
    api-buffer.c          # Buffer層 Emacsプリミティブ (ギャップバッファ)
    napi.def              # ★API定義表 (単一情報源、下記)
  editor/
    boot.noct             # 起動、コマンドループ
    keymap.noct           # キーマップとバインディング
    commands.noct         # 編集コマンド群
    redisplay.noct        # 再表示
    minibuf.noct          # ミニバッファ/エコーエリア
    search.noct           # 検索 (正規表現エンジンはNoct実装)
    files.noct            # find-file / save-buffer
  tools/
    gen-napi.py           # napi.def から C登録表 + elisp shim + docs を生成
  tests/
    oracle/               # Emacs差分テスト (*.noct)
    term/                 # ptyハーネスによる画面ダンプ比較
    unit/                 # 通常のdiffテスト
  shim/
    noct-shim.el          # 生成物: defalias群 + 方言ヘルパ
```

- Noct 本体の改善(バグ修正、intrinsic追加)は NoctLang サブモジュールで行い
  随時 upstream に反映する。エディタ側リポジトリはコミットを進めて追従。
- ビルドは noct を `NOCT_ENABLE_OBJECT` または STATIC でリンクし、
  remacs 専用 CLI (main.c) が Term/Buffer API を登録して editor/boot.noct
  を実行する。開発中は `noct` CLI + `System.import()` でも動く形を保つ。

## 3. レイヤ構造

```
+--------------------------------------------------------------+
| editor/*.noct   コマンド、キーマップ、再表示、検索、undo     |  Noct
+--------------------------------------------------------------+
| Buffer.* (api-buffer.c)     Emacsプリミティブ相当            |  C
|   ギャップバッファ、point/マーカー、文字⇔バイト変換          |
+--------------------------------------------------------------+
| Term.* (api-term.c)         端末抽象                          |  C
|   rawモード、キーデコーダ、出力バッファ、リサイズ            |
+--------------------------------------------------------------+
| Noct VM (submodule)         GC / JIT / Thread / File          |
+--------------------------------------------------------------+
```

判断基準は Emacs と同じ: 「Emacs で DEFUN (C) のものは C、
Lisp のものは Noct」。迷ったら Noct で書き、遅ければ C に落とす。

## 4. 命名規約と Emacs オラクル

### 4.1 命名規約と名前変換

Noct の識別子は `[a-zA-Z_0-9]+` でありハイフンを含められない。
エディタ API は **`Editor.` 名前空間 + キャメルケース** で書く。
既存標準API (String.charCount, Thread.createThread) と同じ流儀:

```
Noct側:   Editor.gotoChar(pos)  Editor.pointMin()  Editor.bufferModifiedP()
Emacs側:  (goto-char pos)       (point-min)        (buffer-modified-p)
```

名前変換は **elisp トランスレータ (elback) 自身が行う**。
defalias の羅列ではなく、`Editor.member(...)` という呼び出し形を
kebab-case の直接呼び出しに変換する。名前空間方式の利点:

- Noct 側で insert / mark / point のような裸のグローバル120個を
  作らずに済む (ローカル変数との衝突回避)
- C 側の登録も Thread./HttpServer. と同一パターン
- 生成される elisp が素の Emacs 呼び出しになり読みやすい

**upstream に入れる形は汎用機構にする**: elback に名前空間マップを
渡せるようにする (`--elisp-ns-map <file>`)。

```
# ns-map (gen-napi.py が napi.def から生成)
Editor.*          → kebab(member)          # 既定則: gotoChar → goto-char
Editor.searchForward → noct--search-forward  # 例外: shim のアダプタへ
String.*          → noct-string-<kebab>    # Noctランタイム関数 (shim実装)
Dict.* / Array.*  → noct-dict-* など        # 同上
```

- 既定則で足りる関数はそのまま Emacs 名に落ちる。
- **Emacs の optional 引数 / nil 規約と合わない関数だけ** napi.def に
  adapt マークを付け、shim 側の `noct--*` アダプタ関数を経由させる
  (例: search-forward の BOUND=nil / NOERROR=t は Noct の 0/1 と
  直交しないため、アダプタで変換する)。
- String./Dict./Array. 等の Noct 組込みは Emacs には存在しないので、
  shim がランタイムとして実装する (オラクルテストのコードが使うため)。

gen-napi.py は napi.def から ns-map と shim を両方生成する。
手書きの対応コードは持たない。

### 4.2 napi.def (単一情報源)

1行1関数のタブ区切り表:

```
# noct名(Editor.)   emacs名        impl    shim      引数      備考
gotoChar            goto-char      c       direct    pos
point               point          c       direct    -
insert              insert         c       direct    text      文字列1引数のみ(v1制限)
forwardChar         forward-char   noct    direct    n
searchForward       search-forward c       adapt     s bound noerror   nil/t変換が必要
...
```

emacs名は原則キャメルケースからの機械導出 (lint で矛盾検出)、
shim=adapt の行だけ noct--* アダプタを生成する。

- impl=c は api-buffer.c に実装、impl=noct は editor/ に実装。
- gen-napi.py はここから (1) C の FFI 登録表 (2) defalias shim
  (3) 関数一覧ドキュメント (4) v1スコープの検査(表にない関数を
  editor/ が呼んでいたらエラー) を生成する。
- **スコープ膨張はこの表で物理的に止める。** v1 の表は約120行で凍結。

### 4.3 オラクル差分テストの仕組み

```
tests/oracle/insert-delete.noct
    ↓ noct (VM実行)               ↓ noct --elisp (トランスパイル)
  観測ログ A                    insert-delete.el
                                   ↓ emacs --batch -l noct-shim.el -l ...
                                観測ログ B
              diff A B  → 一致すれば pass
```

- テストは `oracleEmit(x)` だけで観測する。Noct側は print、
  shim側は `(defun oracleEmit (x) (princ ...) (terpri))`。
  print の書式差(浮動小数など)を吸収する唯一の関数にする。
- 観測対象: バッファ内容 (buffer_string)、point、mark、マーカー位置、
  buffer_size、関数の返り値。
- Emacs はこのマシンに未インストール。`sudo apt-get install emacs-nox`
  が必要 (バージョンを CI と揃えて記録する)。

### 4.4 elisp バックエンドの方言ギャップ (upstream課題)

現状の elback.c は次を素朴に出力するため、shim かバックエンド修正が要る:

| Noct | 現状の出力 | 問題 | 対処 |
|---|---|---|---|
| `a + b` (文字列) | `(+ a b)` | elispの`+`は数値専用 | elbackを `(noct-add a b)` 出力に変え、shimで型分岐実装 |
| `a == b` | 出力確認要 | 文字列比較は`equal` | 同上 `(noct-eq a b)` |
| 辞書リテラル | `(setq d '((k . v)))` | **閉じ括弧欠落 (elbackバグ、実測)**。またクォートリテラルは不変データでmutate不可 | 括弧修正 + `(list (cons ...))` か hash-table 生成に変更 |
| 関数値の呼び出し | `(cmd ed)` | elispはLisp-2、変数経由は `(funcall cmd ed)` が必要 (実測) | elback修正 |
| 関数値の代入 | `(setq cmd $anon...)` | シンボル値として未束縛。`#'` が必要 (実測) | elback修正 |
| `for (k,v in d)` | 出力確認要 | maphash | shimヘルパ |
| 無名関数名 | `$anon.<絶対パス>.0` | パス依存で環境非可搬 | 相対名に変更 |
| `if (x)` の真偽 | `(if x ...)` | **Noctは0が偽、elispは0が真**。数値を返す関数を条件に使うと分岐が逆転する | elbackが `(if (noct-truthy x) ...)` を出力、shimで 0/nil/"" を偽と定義 |
| nil の不在 | - | Noctにnilがなく、Emacsのoptional引数 (BOUND=nil等) を表現できない | napi.def の adapt マーク + shimアダプタで変換 (4.1) |

方針: elback は「noct- プレフィクスのランタイム関数を呼ぶ elisp」を出力し、
ランタイムは shim 側に置く。elback の修正は NoctLang upstream に入れる。
オラクルテスト自体が elback のテストにもなる(一石二鳥)。

## 5. Term.* 外部仕様

### 5.1 セッション

```
Term.open()                 rawモード + 代替スクリーン開始。成功で1
Term.close()                復元。atexit相当の安全網もC側に持つ
Term.size()                 → {rows: int, cols: int}
Term.resized()              → 前回呼び出し以降にSIGWINCHがあれば1 (ポーリング)
```

### 5.2 出力 (すべて内部バッファへ、flushで一括write)

```
Term.moveTo(row, col)       1始まり
Term.write(text)            UTF-8文字列をそのまま
Term.clear()                全画面消去
Term.clearToEol()           行末まで消去
Term.setStyle(style)        style = {fg: n, bg: n, bold: 0/1, reverse: 0/1,
                                     underline: 0/1}  n: -1=default, 0-255
Term.showCursor(visible)
Term.flush()                バッファを1回のwrite(2)で吐く
```

- 再表示は「フレームを組み立てて flush 1回」を規約とする。
  ちらつき防止と、pty テストでの出力比較のしやすさのため。
- スクロール領域 (CSI r) は v1 では使わない(全再描画方式のため不要)。

### 5.3 入力

```
Term.readKey(timeoutMs)     → キーイベント int、タイムアウトで -1
Term.pendingInput()         → 未処理入力があれば1 (再表示スキップ判定用)
```

キーイベントは **Emacs のイベント整数表現をそのまま採用する**:

- 下位21bit: Unicode コードポイント、または特殊キー定数
- modifier ビット: meta=2^27, control=2^26, shift=2^25,
  hyper=2^24, super=2^23, alt=2^22 (Emacs と同一のビット配置)
- 特殊キー (arrow/home/end/pgup/pgdn/f1-f12/delete) は
  私用面 (U+E000-) に割り当てた定数。Term.KEY_UP 等で公開。

同一表現にする理由: (1) X11/Win32 バックエンドへの差し替えが
「keysym+modifier→この整数」の変換だけで済む (2) 将来キーマップ関連を
オラクル比較する際に値がそのまま一致する (3) `C-x` = `(1<<26)|'x'` の
ような合成がキーマップ実装側で単純になる。

### 5.4 内部仕様 (api-term.c)

**エンコーディング前提**: UTF-8端末、metaSendsEscape (ESCプレフィックス)。
8-bit meta は UTF-8 と多義になるため対応しない。

**初期化シーケンス**:
```
termios: ICANON/ECHO/ISIG/IXON/ICRNL 等を落とす (cfmakeraw基準、ISIGは残すか要検討→v1は落とす)
出力:    CSI ? 1049 h   (代替スクリーン)
         CSI ? 25 l/h   (カーソル制御)
         CSI > 4 ; 2 m  (modifyOtherKeys=2)
終了時は全て逆順で復元。SIGTERM/クラッシュ時の端末復元のため、
tcsetattr の元設定は static に保持し atexit + シグナルで復元。
```

**キーデコーダ**: 状態機械 + リングバッファ。

```
状態: GROUND / ESC / CSI / SS3 / UTF8(n)
遷移:
  GROUND: 0x20-0x7E → そのまま文字イベント
          0x01-0x1A → control | (c+0x60)   ※C-i/C-m/C-h は TAB/RET/BS に正規化
          0x1B      → ESC状態へ (タイムアウト開始)
          0xC0-0xF7 → UTF8状態へ (継続バイト収集 → コードポイント)
  ESC:    [ → CSI、O → SS3、
          その他の文字c → meta | decode(c)  (ESCプレフィックスmeta)
          タイムアウト(既定50ms) → 素のESCイベント
  CSI:    パラメータ収集 → 終端文字で解釈
          A/B/C/D → 矢印、~系 → home/end/pgup/del/F5-12、
          1;m 系modifier → shift/alt/ctrlビット合成
          27;m;c~ (modifyOtherKeys) → modifier|c
  SS3:    P/Q/R/S → F1-F4 など
```

- ESC 単独判定のタイムアウトは readKey の poll で実装
  (ESC受信後だけ50msの短いpollを回す)。Emacs と同じ体感になる。
- 既知の限界 (v1で受容): C-i/TAB 等の融合、モードによっては C-S-a 不可。
  modifyOtherKeys 対応端末では拾える。kitty keyboard protocol は将来。
- **ブロッキング規約**: readKey の poll は noct_enter_blocking() で包む。
  エディタが入力待ちでも他スレッド(将来の非同期処理)の GC を止めない。

**リサイズ**: SIGWINCH ハンドラはフラグを立てるだけ。
readKey が poll から戻る際にフラグを見て、-2 (RESIZE擬似キー) を返す。
シグナルハンドラから VM に触らない。

**pty テスト**: tests/term のハーネスが openpty で remacs を起動し、
キー列を書き込み、出力ストリームを正規化(CSI引数の揺れ吸収)して
期待ダンプと diff する。Term 層はオラクル比較ができない分、ここを厚くする。

## 6. Buffer.* 外部仕様 (Emacs プリミティブ層)

### 6.1 モデル

- **point は 1 始まりの文字位置** (Emacs互換。オラクル一致の絶対条件)。
- **カレントバッファは暗黙** (Emacs互換)。`set_buffer(buf)` で切替。
  ほとんどの関数はカレントバッファに作用する。
- v1 はエディタ全体がシングルスレッド。Buffer API はカレントバッファを
  VM グローバルに持つ。スレッド安全化は将来(Emacs 30のasync-threadsと
  同じ問題であり、v1で背負わない)。

### 6.2 v1 関数表 (napi.def の初期内容、impl=c のもの)

```
バッファ管理:  current_buffer, set_buffer, generate_new_buffer,
              get_buffer, kill_buffer, buffer_name, buffer_list,
              buffer_modified_p, set_buffer_modified_p
位置:         point, point_min, point_max, goto_char, bobp, eobp,
              bolp, eolp, buffer_size
読み出し:     char_after, char_before, buffer_string,
              buffer_substring, following_char, preceding_char
変更:         insert, insert_char, delete_region, delete_char, erase_buffer
行:           forward_line, beginning_of_line, end_of_line,
              line_beginning_position, line_end_position,
              count_lines, line_number_at_pos
マーカー:     make_marker, set_marker, marker_position, copy_marker,
              point_marker, mark_marker
マーク:       set_mark, mark, region_beginning, region_end
変更フック:   buffer_chars_modified_tick (undo/再表示の変更検知用)
```

impl=noct (Buffer.*の上にNoctで書き、それでもオラクル比較する):

```
forward_char, backward_char, forward_word, backward_word,
kill_line, kill_region, yank, kill_ring_save, open_line,
newline, self_insert_command, transpose_chars, capitalize_word,
upcase_region, downcase_region, ...
```

### 6.3 内部仕様 (api-buffer.c)

**ギャップバッファ**:

```c
struct buffer {
    uint8_t *text;        /* noct_malloc、VMヒープ外 */
    size_t   gap_start;   /* バイト */
    size_t   gap_end;     /* バイト */
    size_t   capacity;    /* バイト */
    size_t   char_count;  /* 文字数キャッシュ */
    /* 文字⇔バイト変換キャッシュ (最重要の性能機構) */
    size_t   cache_char;  /* 直近アクセスの文字位置 */
    size_t   cache_byte;  /* 対応するバイト位置(ギャップ無視の論理位置) */
    struct marker *markers;  /* 連結リスト */
    long     modiff;
    struct buffer *next;
    char     name[64];
    int      modified;
};
```

- テキスト本体は VM ヒープ外 (noct_malloc)。GC 対象にしない。
  バッファハンドルは native_pointer 付き辞書 (Thread/HttpServerと同型)。
  ※辞書拡張・GC昇格で native_pointer が保持されることは今回修正済み。
- **文字⇔バイト変換**: 直近位置キャッシュ1点 + 差分走査。
  エディタのアクセスは局所的なので、実用上 O(移動距離) で足りる。
  Emacs も同種のキャッシュで解いている。プロファイルして足りなければ
  行インデックスを足す (v1では入れない)。
- **マーカー調整**: insert/delete のたびに全マーカーを走査して調整。
  マーカー数は少ない前提 (Emacsも同様の線形調整)。
- **UTF-8整合性**: insert は入力が正当な UTF-8 であることを検証する。
  Term から来る文字列は Term 側で検証済みなので二重チェックは軽くてよい。
- undo は v1 では Noct 側 (編集プリミティブをラップして逆操作を記録)。
  Emacs の buffer-undo-list と同形式のリストにすると将来オラクル比較可能。

## 7. エディタ本体 (Noct側) の設計要点

**コマンドループ** (boot.noct):

```
while (running) {
    redisplay();                     // pendingInput()==1 ならスキップ
    var key = Term.readKey(-1);
    var cmd = keymapLookup(currentKeymap, key);
    executeCommand(cmd);             // prefix (C-x等) は状態を進めるだけ
}
```

**キーマップ**: Noct 辞書。キーはイベント整数を文字列化したもの
(辞書キーは文字列のため)。`globalMap["C-x"]` がさらに辞書ならプレフィックス。

**コマンド規約 (環境辞書スタイル)**: クロージャの代わりに、
コマンドは環境辞書を明示引数 `_` で受ける Noct の流儀に従う:

```
cmdForwardWord = (_) => {
    forwardWord(prefixArgOr(_, 1));
};
```

- 辞書は参照渡しなので `_.field = v` の変更は呼び出し元に見える
  (実測確認済み)。書き換えられないのは束縛そのもの (呼び出し元
  ローカル変数の再代入) だけで、エディタ状態の更新には十分。
- **引数個数は厳格に検査される** (不一致は実行時エラー) ため、
  キーマップから呼ばれるコマンドは全て `(_)` の1引数に統一する。
  プレフィックス引数は `_.prefixArg` 経由 — Emacs の動的変数
  current-prefix-arg と同型になる。
- **2層に分ける**: ロジック関数は Emacs と同じシグネチャ
  (`forwardWord(n)`) で書き、オラクル比較の対象にする。
  環境辞書を受けるのはキーバインド対象の薄いラッパ層のみ。
  ラッパ層は pty 統合テストで覆う。
- 性能規約: `_.x` は毎アクセスがハッシュ検索なので、ホットループでは
  先頭でローカル変数に取り出す。

**再表示** (redisplay.noct): v1 は**毎回全再描画**。

```
1. ウィンドウ開始位置(window-start)を point が見えるよう調整 (スクロール)
2. 各行: バッファから1行取り出し、タブ展開・長行切詰めして Term.write
3. モードライン、エコーエリア
4. カーソル位置決め → Term.flush()
```

80x24〜200x60 の全再描画は数千文字の write で、Packed/文字列処理の
速度からして毎キー全再描画で問題ない。差分再表示は「遅くなったら」着手
(それ自体が良いVMベンチマークになる)。

**検索/正規表現** (search.noct): 合意済みの通り Noct 実装。
Emacs 正規表現方言 (`\\(` グループ、`\\|` 選択) のサブセット。
バックトラック型で素直に書く。これが最大の VM ストレステストになる。

## 8. テスト戦略

| 層 | 方法 | オラクル |
|---|---|---|
| Buffer.* (C) | tests/oracle/*.noct 差分実行 | GNU Emacs --batch |
| 編集コマンド (Noct) | 同上 (同じ仕組みに乗る) | GNU Emacs --batch |
| 正規表現 | 同上 (string-match等と比較) | GNU Emacs --batch |
| Term.* | ptyハーネス + 画面ダンプdiff | 期待値ファイル |
| 統合 | ptyでキー列注入→最終バッファをファイル保存→diff | 期待値ファイル |
| VM負荷 | 長時間ランダム編集 (fuzz) + mt-tsanビルド | クラッシュしないこと |

fuzz は初期から用意する: ランダムなキー列を数百万打鍵し、
(1) クラッシュしない (2) buffer_size とマーカーの不変条件が保たれる
(3) 同じ乱数種で Emacs オラクルと最終状態一致、を見る。
今回の経験上、**GC バグはこの種の長時間負荷でしか出ない**。

## 9. 開発フェーズ

```
P0  リポジトリ作成、submodule、ビルド接続、gen-napi.py の骨格      (小)
P1  Term.* 実装 + キーデコーダ + pty ハーネス + 打鍵デモ           (中)
P2  Buffer.* 実装 + オラクルハーネス稼働 (emacs-nox導入)           (中)
    → 最初の差分テストが回った時点が最初のマイルストーン
P3  コマンドループ + キーマップ + 全再描画 → 「メモ帳」水準        (中)
P4  Emacs コマンド v1 セット、kill ring、undo、リージョン           (大)
P5  検索(正規表現)、置換、find-file/save-buffer、モードライン      (大)
P6  fuzz 常設、長時間試験、性能プロファイル                         (継続)
将来: C-x 2 (複数ウィンドウ)、シンタックスハイライト、GUIバックエンド
```

## 10. リスク

1. **elback の方言ギャップ** — オラクル体制の前提。P2 で最初に洗う。
   直すのは upstream (NoctLang) 側。
2. **キーデコードの端末差** — modifyOtherKeys 非対応端末では C-S-a 等が
   拾えない。「Emacsも同じ制約」と明記して受容。
3. **クロージャ不在** — コマンド規約(引数なし+グローバル状態)で吸収。
   規約を破ると後から直せないので P3 で確定させる。
4. **スコープ膨張** — napi.def の凍結で物理的に防ぐ。
5. **文字⇔バイト変換の性能** — キャッシュ1点で始め、プロファイルで判断。
6. **Emacs のバージョン差** — オラクルの Emacs バージョンを固定し記録。
   挙動がバージョン依存の関数 (undo境界など) はテスト対象から外す。
7. **long/float 条件の JIT 未対応** — インタプリタは数値型全般を
   if/while 条件に取れるよう修正済み (upstream 6a52f0c)。JIT は依然
   値スロットの下位32bitのみを test するため、下位32bitが0のlong
   (2^32等) や -0.0 で誤分岐する。remacs 側は「C APIの戻り値はintに
   限る」規約で回避。JIT修正は10アーキ横断のためupstream課題として
   別トラック。
8. **エラー捕捉機構の不在** — Noct には try/catch がなく、コマンド内の
   実行時エラーは VM トップまで伝播してプロセスが終了する。エディタは
   「コマンドが失敗してもエコーエリアに表示して継続」が必須 (Emacs の
   command-error 相当)。対処は次のいずれか:
   (a) remacs 専用 CLI の C 側でコマンドループの1周を noct_call し、
       失敗時はエラーメッセージを回収して継続する。
   (b) upstream に `Error.try(func, arg)` 相当の intrinsic を追加する
       (pcall 型。汎用性があるのでこちらが本筋)。
   P3 のコマンドループ設計時に (b) を upstream 提案する。
```


## 付録A: 将来の JIT 最適化メモ (ABCE)

エディタ完成後、実測で遅い場合にのみ着手する (方針決定 2026-08-08)。
それまで Packed アクセスの高速化は行わない。

Packed アクセス (Editor.* のホットループ) の高速化計画:

- Array Boundary Check Elimination をループのバージョン化で実装する。
- HIR 表現で、添字が一次式のとき index の下限・上限とそのときの
  変数値を求め、`0 <= index && index <= length` を満たすことを
  チェックしてループを高速版/一般版にバージョン化する。
- 高速版ループでは、ループ前にアドレスベースを計算し、ベース相対で
  アクセスするよう変形する。アドレスは 64bit 固定 (32bit バックエンドは
  切り詰め)。
- ベース相対アクセスの HIR op (型幅つき) と、それに 1:1 対応する
  LIR op を追加する。
- JIT はベース相対アクセスを直接マシン語 (幅バリエーションのある
  load/store のみ) に変換し、ベース変数はレジスタに乗せることを目標と
  する。

現状の性能ベースライン (build-debug, JIT既定):

- 全ASCIIバッファ: 20k行挿入 + ランダム100ジャンプ+編集 + 200行
  後方移動 = 0.13秒 (char==byte ショートカットが効く)
- マルチバイト混在バッファ: char→byte 走査が Noct ループになるため
  ランダムアクセスは著しく遅い (同等負荷で数分)。ABCE の主対象。
