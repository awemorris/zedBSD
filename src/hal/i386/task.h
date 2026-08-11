/*
 * task.h
 *	- task management
 */

#ifndef _SYS_ARCH_X86_TASK_H_
#define _SYS_ARCH_X86_TASK_H_

#include <hal/task.h>	/* interface definition */
#include <kern/sched.h>	/* (struct schedulable) */

/*
 * システムスタックサイズ
 */
#define SYS_STACK_SIZE	(4096)

/*
 * タスク構造体
 */
struct task_info {
	/* スケジューラのschedulable構造体にキャスト可能とする */
	struct schedulable	_inherit;

	struct task_info *next;	/* タスクリストのリンク */
	univ_t	universe;		/* 動作アドレス空間 */
	int	run_cpu;		/* 実行中のCPU(実行中でなければ-1) */
	void	*sys_stack;		/* 確保したカーネルモードスタック */

	/* コンテキスト */
	struct task_resume_frame *resume_esp;	/* スタックポインタ */
	uint8	fpregs[512];	/* FXSAVE/FXRSTORE用保存域 */
};

/*
 * タスク切り替え時のコンテキスト保存用スタックフレーム
 */
struct task_resume_frame {
	uint32	gs;			/* pushl %gs */
	uint32	fs;			/* pushl %fs */
	uint32	es;			/* pushl %es */
	uint32	ds;			/* pushl %ds */
	uint32	edi;		/*	 (pushal) */
	uint32	esi;		/*	 (pushal) */
	uint32	ebp;		/*	 (pushal) */
	uint32	_esp;		/*	 (pushal) この値は無視 */
	uint32	ebx;		/*	 (pushal) */
	uint32	edx;		/*	 (pushal) */
	uint32	ecx;		/*	 (pushal) */
	uint32	eax;		/* pushal */
	uint32	eflags;		/* pushfl */
	uint32	ret_eip;	/* 初回: asm_task_start()
						 * 以降: asm_task_dispatch()コール直後の命令 */

	union {
		/* システムタスク用(スタック切り替えなし, 引数をプッシュ) */
		struct {
			uint32	eip;		/* 実行開始位置 */
			uint32	cs;
			uint32	eflags;
			uint32	_ret_eip;	/* (C呼び出し規約上の戻り先アドレス) */
			uint32	param;		/* スタック渡しする引数 */
		} sys;

		/* ユーザタスク用(スタック切り替え, 引数はユーザスタックにプッシュ) */
		struct {
			uint32	eip;	/* 実行開始位置 */
			uint32	cs;
			uint32	eflags;
			uint32	esp;	/* ユーザスタック */
			uint32	ss;
		} usr;
	} init;
};

/*
 * task.c
 */
void task_init();

/*
 * dispatch.s
 */
void asm_task_entrypoint();	/* 新規タスクのエントリポイント */
void asm_task_dispatch(		/* タスクを切り替える */
	struct task_resume_frame **save,
	struct task_resume_frame **load
);

#endif
