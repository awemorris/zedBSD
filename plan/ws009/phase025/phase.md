<!-- awesome-plan project=zedbsd record=ws009-p025 -->

# ws009-p025: DOC-55 managed-wlan.md を今の `net` の口に合わせて書き直す（案 A）

Status: 書いた・link PASS（2026-10-30 P1、q1705-i01。cleared は Q1）
Parent: [WS009](../ws.md)
Approval: 2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」。2026-10-30 Q1: 案 A〜D を Phase にする、A と D は P1 がそのまま続けて実行してよい。
実行の担当: Opus（P1）。振る舞いを code から読み取る量が多いので Sonnet に渡さない。
依存: なし

## 1. 目的

`docs/reference/managed-wlan.md` は「Status: current for the managed WLAN v1 interface」と名乗るが、削除済みの
`net wifi set-key SSID PASSPHRASE [auto]` を唯一の文法として書く。今の `userland/base/net/main.c` の usage は
`net wifi add`・`modify`・`delete`・`enable`・`disable`・`list`・`connect`・`disconnect`・`session open|close [account]`。
文書を今の source と networkd の振る舞いに合わせ、header・source・試験へ辿れる形にする。
`net lan`・`net startup` がどの文書にも無ければ、[network-console.md](../../../docs/reference/network-console.md)（同じ DOC-55）に足す。

## 2. 読む物

- AGENTS.md §4・§7・§10、[docs/style.md](../../../docs/style.md)。
- 今の文書: `docs/reference/managed-wlan.md`・`docs/reference/network-console.md`。
- source（振る舞いの根拠）: `userland/base/net/main.c`（usage・dispatch・`wifi_store_command`・`wifi_session_command`・`wifi_command`・interactive の `net>`）、
  `userland/base/net/wifi-store.c`・`.h`（利用者ごとの store・system の store・file の mode・所有者・上限）、`wifi-conf.c`・`.h`、
  `userland/base/networkd/managed-wlan.c`・`managed-wlan-state.h`・`wifi-child.c`・`main.c`（状態・30 秒の retry・SO_PEERCRED・session）、
  `managed-lan.c`・`lan-configure.c`（`net lan`）。
- 試験の見当: `grep -rln 'wifi add\|wifi session\|wifi-store' plan/tools tests userland --include=*.c --include=*.sh --include=*.py`。

## 3. 作る物

- `docs/reference/managed-wlan.md` の書き直し（文法・store・session・状態・retry・権限・誤り・例・出典の表）。
- 必要なら `docs/reference/network-console.md` の `net lan`・`net startup` の節。
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 4. 確かめの項目

- usage の各形が文書の文法の節と一字一句で合う（option の綴り、既定の値、`-` の意味）。
- store の path・mode・所有者・誰が書けるか、`HOME` を信用しない事、root と一般の利用者の違いを code から確かめて書く。
- 状態の名前（`disabled` など）と遷移、retry の間隔は networkd の定数から。
- 秘密を例に書かない（`REPLACE-WITH-SECRET` の類）。
- 主張ごとの出典の表（file と関数）。試験の無い主張は「試験 無し」と書く。

## 5. 変えてはいけない物

- source・header・試験の道具（文書が source と食い違えば source を正として文書を直し、source の不具合に見えれば結果に Bug の候補として書く）。
- 他の docs（link を直す時を除く）。HAL・toolchain・共有の build。

## 6. 確かめの command

```sh
timeout 120 build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/howto docs/architecture
grep -c 'set-key' docs/reference/managed-wlan.md   # 0
git diff --check
```

## 7. 終わりの条件

上の command が通る。usage の全ての wifi の形が文書にある。commit は
`git commit -m WIP -- docs/reference/managed-wlan.md docs/reference/network-console.md plan/ws009/phase025/phase.md plan/ws009/ws.md`。
結果の節（日付は host の `date`、command と最後の行、自分で決めた点、Bug の候補、未実施）。ws.md の行は「書いた・link PASS」（cleared は Q1）。

## 結果

日付: 2026-10-30（host の `date`）。担当: P1（Opus）。基点 main aaf2bc280 の上（worktree p1-ws009r）。

### 書いた物

- `docs/reference/managed-wlan.md` の全面の書き直し: 文法（`add`・`modify`・`delete`・`enable`・`disable`・`list`・`connect`・`disconnect`・`session open|close [account]`）、
  network の保存（key を標準入力から読む、`--password -`、`--auto yes|no`、既定 yes、誤りの文言）、login session（sessiond、最大 8、root と network の群）、
  権限（owner と network の群、enable と connect が owner を移す）、store（UID ごと、HOME を信用しない、lock した書き直し）、`net wifi list` の行の形
  （radio ごとの行と最後の policy の record `wifi state=… owner=… store=… sessions=… ssid=… rejected=… scan=…`）、候補の store の順、key の拒否の記憶（最大 8）、
  `disconnecting` の過渡の状態、`/var/db/wifi-off` と `net startup`、誤り（ENOENT・ENODEV・ENETUNREACH・EACCES・EBUSY）、応答の待ちの秒（105・45・17）、出典の表。
  開発の段の記述（q071・q124・日付・計測値）は style.md「Schedules … and the state of the work belong in plan/」に従い外した。
- `docs/reference/network-console.md`: 「Wired management commands」の節（`net lan enable|disable`・`net lan set`・`net startup`、権限、`wait`）を足し、
  末尾の「WS011 p007 … p008 follow-up」の文（作業の状態）を外し、出典の表に 1 行を足した。

### 実行した command と最後の行

- `noct plan/ws009/tests/check-relative-links.noct docs/reference docs/howto docs/architecture` → `Markdown relative-link check: PASS (1037 links)`
- `grep -c 'set-key' docs/reference/managed-wlan.md` → `0`
- `git diff --check` → 出力なし
- usage の `net wifi` の 9 形（main.c の usage の文字列から抜いた物）が文書に一字一句である事を python で照合 → 9/9 OK

### 主張ごとの出典（試験は全部「無し」: 文書から link できる公開の試験が無い。store と profile の host 試験は plan/ws005/tests にあるが docs から plan/ へ link しない）

| 主張 | 根拠 |
| --- | --- |
| 文法と option の規則、標準入力の key、誤りの文言 | net/main.c `usage`・`wifi_store_command`・`wifi_store_options`・`read_password`・`wifi_command`・`wifi_session_command` |
| console の履歴に add・modify を残さない | net/main.c の `secret_line` |
| add は末尾、modify はその場、delete は後ろを詰める | net/wifi-conf.c `wifi_conf_add`（`wifi_conf_set_key` で追加）・`wifi_conf_modify`・`wifi_conf_delete` |
| store の選び方・lock した書き直し | net/wifi-store.c `wifi_store_edit_for_effective_user`・`wifi_store_update_at`・`select_store` |
| session の規則（root と自分、最大 8、新しい方へ、close の効果） | networkd/main.c `wifi_session_account`・`wifi_session_open`・`wifi_session_close`、`NETWORKD_WIFI_SESSION_MAX` |
| 権限（owner_allowed、enable・list・通知は誰でも、socket 0660 root:network） | networkd/main.c `owner_allowed`・`process_wifi_request`・socket の作成、`client_operation_allowed` の類（3825 行あたり） |
| connect が owner を移す、30 秒の選択、失敗で manual-disconnected | networkd/main.c `wifi_request_connect`・`wifi_join_failed`、`NETWORKD_WLAN_SCAN_SECONDS` |
| 候補の store の順・拒否の飛ばし | networkd/main.c `load_candidates`・`candidate_store_add` |
| 拒否の記憶と忘れる時 | networkd/main.c `wifi_rejection_note`・`wifi_rejection_forget`・`wifi_rejection_forget_store`・`wifi_request_stop` |
| list の行の形・状態の名前（`disconnecting` は RETIRING） | networkd/main.c `append_wifi_snapshot`・`append_managed_status`・`managed_state_name` |
| 再接続の 30 秒 | networkd/main.c `recover_managed_connection`、wifi/main.c `WIFI_CONNECT_SECONDS` |
| 退役の再試行の back-off 最大 60 秒 | networkd/main.c `schedule_retirement_retry` |
| 応答の待ち | net/protocol.h `NETWORKD_WIFI_REQUEST_SECONDS`・`NETWORKD_WIFI_TRANSPORT_MARGIN` |
| wifi-off の記録 | networkd/main.c `wifi_off_remember`・`process_wifi_request`、net/main.c `startup_command`、protocol.h `NETWORKD_WIFI_OFF_PATH` |
| boot が net startup を root で流す | userland/base/init/services/networking |
| net lan の動作と権限、net startup の wait | net/main.c `lan_command`・`lan_set_command`・`lan_send_policy`・`wait_requested`・`wait_for_address`、networkd/main.c `handle_lan_configure`・`lan_configure_target`・LAN_ENABLE の処理・socket member に許す操作の一覧 |

### 自分で決めた点

- 「enable は誰でも（socket に繋がれる者 = root と network の群）」と書いた。`process_wifi_request` は ENABLE・LIST・PROFILES_CHANGED を `owner_allowed` に通さないが、socket の mode が 0660 root:network なので実際の peer は root か network の群に限られる。
- radio の記述は「RTL8822BU と AX211」に留め、受け入れの回（q 番号）・計測の秒は外した（style.md）。
- network-console.md の末尾の WS011 の文を外したのは、p025 で触る file の中の作業の状態の記述だから（範囲の中と判断）。

### Bug の候補・観察（直していない）

- source の comment に開発の管理情報が残る: net/main.c（`(2026-10-03 user decision)`、`(ws005-p024)`、`(ws089-p022)`）、networkd/main.c（`ws005-p019`・`ws005-p024`・`ws052-p010`・`ws089-p021`・`ws130-p007`・`ws254`・`BUG-187`・`q631` ほか）。AGENTS.md §10。drivers の外なので WS296 の範囲でもない。
- `wifi-store.h` の公開の宣言 `wifi_store_set_key_for_effective_user` は、`set-key` の口が消えた後も残る（使い手は grep で net の外に無い。未確認: 他の呼び手）。

### 未実施

- guest の中で `net wifi add` などを実際に流す確かめ（文書は source の読みだけで書いた）。
