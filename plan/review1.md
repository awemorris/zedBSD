
hal.h

下記の２つの関数について、名前が直交していない。
stateとterminal, 実態としてどちらがより正確？

void
hal_cons_save_state(
	struct hal_cons_state *state);

void
hal_cons_restore_terminal(
	const struct hal_cons_state *state);

下記２関数について、どちらがブロッキングでどちらが非ブロッキングなのかが一瞬では判別できなかった。
加えて、pollの戻り値も不明。
すべてのHAL関数に、挙動、自明でないなら背景と目的、前提条件と事前条件と事後条件による契約、戻り値を記載してほしい。

int
hal_cons_read_event(
	struct hal_key_event *event);

int
hal_cons_poll_event(
	struct hal_key_event *event);

次の関数の目的が不明。

void
hal_cons_update_cursor(void);

次の関数はキーイベント取得が追加されたため、すでに不要。
そのためにHALが全キーのステートを保持してエミュレーションするのは非効率であると考えるため。
アプリがキーイベントをevdevで受け取って、自前でステートを管理するのがいい。

int
hal_cons_key_state(
	int key);

次の関数の使途が不明。

void
hal_cons_suspend(void);

void
hal_cons_resume(void);

次の決め打ちはおかしい。hal_cons_get_col_row_size()を追加して置き換える。

#define HAL_CONS_COLUMNS		80U
#define HAL_CONS_ROWS			25U
