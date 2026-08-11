/*
 * タスク管理部
 */

#include <hal/runtime.h>
#include "task.h"
#include "asm.h"

extern uint32 tss_area[26];	/* TSS */

static struct task_info	*task_list;		/* 全タスクのリスト */
static struct task_info	*running_task;	/* CPUで実行中のタスク */

static void set_initial_resume_frame(struct task_info *ti, void *start, void *param, void *user_sp);
static void tasklist_add(struct task_info *ti);
static void tasklist_del(struct task_info *ti);


/*
 * タスク管理部を初期化する
 */
void i386_task_init(void)
{
	struct task_info *ti;

	task_list = NULL;

	/* TSSをゼロクリアする */
	hal_memset(tss_area, 0, 104);
	tss_area[2] = SEG_SYS_DATA;	/* SS0 */

	/* 現在CPUで実行中のコンテキストを表すタスクを作成する */
	ti = hal_malloc(sizeof(struct task_info));
	hal_memset(ti, 0, sizeof(struct task_info));
	ti->universe = UNIV_SYS;		/* カーネル空間で動作するタスクである */
	ti->run_cpu = 0;			/* CPUの番号 */

	/* タスクリストに追加する */
	tasklist_add(ti);

	/* 作成したタスクを、現在のCPUで実行中のタスクとして設定する */
	running_task = ti;
}

/*
 * 新しいタスクを作成する
 */
task_t task_create(
	univ_t universe,	/* 動作アドレス空間 */
	void *start,		/* 開始関数のアドレス */
	void *param,		/* 開始関数の引数 */
	void *user_sp)	/* ユーザスタックポインタ(カーネルタスクではNULL) */
{
	struct task_info *ti;

	/* タスク構造体のメモリを確保してメンバを設定する */
	ti = hal_malloc(sizeof(struct task_info));
	hal_memset(ti, 0, sizeof(struct task_info));
	ti->universe = universe;		/* 動作アドレス空間 */
	ti->run_cpu = -1;			/* 非実行状態 */

	/* システムスタックを割り当てる */
	ti->sys_stack = hal_malloc(SYS_STACK_SIZE);

	/* システムスタックの最低位アドレスにタスク構造体へのポインタを格納する */
	*(uint32 *)(ti->sys_stack) = (uint32) ti;

	/* タスクの初期スタックフレームをセットする */
	set_initial_resume_frame(ti, start, param, user_sp);

	/* タスクリストに追加する */
	tasklist_add(ti);

	/* task型にキャストして返す*/
	return (task_t)ti;
}

/* タスク開始時のレジュームフレームを設定する */
static void set_initial_resume_frame(
	struct task_info *ti,
	void	*start,
	void	*param,
	void	*user_sp)
{
	struct task_resume_frame *fp;

	/* 初期レジュームフレームポインタの位置を求める */
//	ti->resume_esp = (struct task_resume_frame *)((uint32)ti->sys_stack - sizeof(struct task_resume_frame));
	ti->resume_esp = (struct task_resume_frame *)((uint32)ti->sys_stack + SYS_STACK_SIZE - sizeof(struct task_resume_frame));
    
	/*
	 * レジュームフレームを設定する
	 */

	/* ゼロクリアする */
	fp = ti->resume_esp;
	hal_memset(fp, 0, sizeof(struct task_resume_frame));

	/* asm_task_entrypoint()から実行を開始する */
	fp->eflags	= asm_get_eflags();
	fp->ret_eip = (uint32) asm_task_entrypoint;

	if(ti->universe == UNIV_SYS) {
		/* カーネルモードセグメントをセットする */
		fp->ds = fp->es = fp->fs = fp->gs = SEG_SYS_DATA;

		/* asm_task_entrypoint()のiret命令でstartにジャンプする */
		fp->init.sys.eip	= (uint32) start;
		fp->init.sys.cs		= SEG_SYS_CODE;
		fp->init.sys.eflags	= EFLAGS_IF | EFLAGS_RSV1 | EFLAGS_IOPL_0;

		/* 開始関数start()の呼び出しスタック(引数) */
		fp->init.sys._ret_eip	= 0;
		fp->init.sys.param	= (uint32) param;
	} else {
		/* ユーザモードセグメントをセットする */
		fp->ds = fp->es	= fp->fs = fp->gs = SEG_USER_DATA;

		/* asm_task_entrypoint()のiret命令でstartにジャンプする */
		fp->init.usr.eip	= (uint32) start;
		fp->init.usr.cs		= SEG_USER_CODE | SEG_RPL_3;
		fp->init.usr.eflags	= EFLAGS_IF | EFLAGS_RSV1 | EFLAGS_IOPL_3;

		/* 特権レベル移行によってスタック切り替えが発生する */
		fp->init.usr.esp	= (uint32) user_sp - 8; /* -8は呼び出し規約の分 */
		fp->init.usr.ss		= SEG_USER_DATA | SEG_RPL_3;

		/* 開始関数start()の呼び出しスタック(引数) */
		*((uint32 *) user_sp	) = (uint32) param;
		*((uint32 *) user_sp - 1) = 0;	/* call ret */
	}
}

/*
 * タスクを破棄する
 */
void task_destroy(task_t t)
{
	struct task_info *ti;

	ti = (struct task_info *)t;

	/* タスクリストから削除する */
	tasklist_del(ti);

	/* システムスタックとして割り当てたメモリを解放する*/
	hal_free(ti->sys_stack);

	/* task構造体に割り当てたメモリを解放する */
	hal_free(ti);
}

/*
 * タスクを切り替える
 */
void task_switch(task_t t)
{
	struct task_info *switch_to, *switch_from;

	switch_to	 = (struct task_info *) t;	/* 切り替え先のタスク */
	switch_from  = running_task;			/* 現在のCPUで実行中のタスク */
	running_task = switch_to;				/* running_taskを変更する */

	/* スイッチ先タスクが現在のタスクと同一なら何もせずリターンする */
	if(switch_to == switch_from) {
		/*puts("-same task-");*/
		return;
	}

	/* 切り替え先がユーザ空間の場合 */
	if(switch_to->universe != UNIV_SYS) {
		/* アドレス空間を変更する */
		univ_switch(switch_to->universe);

		/* システムスタックポインタを変更する */
		tss_area[1] = (uint32) switch_to->sys_stack + SYS_STACK_SIZE;
	}

	/* 浮動小数点レジスタ群を切り替える */
	asm_fnsave(switch_from->fpregs);
	if(switch_to->resume_esp->ret_eip != (uint32)asm_task_entrypoint)
		asm_frstor(switch_to->fpregs);

	/* スタックを切り替える */
	asm_task_dispatch(&switch_from->resume_esp, &switch_to->resume_esp);

	/*
	 * 新規タスクの場合はエントリへポイントジャンプする。
	 * それ以外の場合は別なコンテキストのこの位置に戻ってくる。
	 */
}

/*
 * 指定したCPUで実行中のタスクを取得する
 */
task_t task_get_current(void)
{
	/*
	 * システムスタックの最低位アドレスに格納されている、タスク構造体への
	 * ポインタを取得する
	 */
//	ti =  (struct task_info *)(asm_get_esp() & 0xfffff000);

//	return (task_t) ti;

	return running_task;
}


/**
 * タスクリスト操作
 */

/* タスクをリストの最後に追加する */
static void tasklist_add(struct task_info *ti)
{
	/* リストが空の場合 */
	if(task_list == NULL) {
		/* tをリストの先頭にセットする */
		task_list = ti;
		return;
	}

	/* リストの末尾ノードを探す */
	struct task_info *p = task_list;
	while(p->next != NULL)	/* リストの末尾を探す */
		p = p->next;

	/* リストの末尾に追加する */
	p->next = ti;
	ti->next = NULL;
}

/* タスクをリストから削除する */
static void tasklist_del(struct task_info *ti)
{
	struct task_info *p, *prev;

	/* pを探すしてpの一つ前のノードを取得する */
	p = task_list, prev = NULL;
	while(p != NULL) {
		if(p == ti)
			break;
		prev = p;
		p = p->next;
	}
	if(p == NULL)
		return;	/* not found */

	/* リストから削除する */
	if(prev == NULL)
		task_list = p->next;	/* pがリストの先頭の場合*/
	else
		prev->next = p->next;	/* pがリストの先頭でない場合 */
	p->next = NULL;
}
