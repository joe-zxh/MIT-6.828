#include <inc/assert.h>
#include <inc/x86.h>
#include <kern/spinlock.h>
#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/monitor.h>

void sched_halt(void);

// 选择一个用户进程，并运行它
void
sched_yield(void)
{
	struct Env *idle;

	// 实现简单的 round-robin scheduling
	// 
	// 在envs中找到一个ENV_RUNNABLE的进程，然后 从上次CPU结束运行的地方开始运行。
	// 
	// 如果 没有可用的进程，那么直接 运行原来的进程，不用 切换进程了。
	// 
	// 千万不能选择一个ENV_RUNNING的集成。
	// 如果真的没有可运行的进程了，CPU直接暂停sched_halt()即可。
	// 
	// LAB 4: Your code here.
	idle = curenv;
	int start_envid = idle ? ENVX(idle->env_id)+1 : 0; 
	//检查curenv是否为NULL(还没运行过用户进程之前，会是NULL)

	for (int i = 0; i < NENV; i++) {
		int j = (start_envid + i) % NENV;//拿到下一个进程的index值
		if (envs[j].env_status == ENV_RUNNABLE) {
			env_run(&envs[j]);
		}
	}

	if (idle && idle->env_status == ENV_RUNNING) {
		env_run(idle); //没找到一个可运行的进程，那么直接 运行之前那个进程
	}

	// sched_halt never returns 如果真的没有可运行的进程了，CPU直接暂停sched_halt()即可。
	sched_halt();
}

// Halt this CPU when there is nothing to do. Wait until the
// timer interrupt wakes it up. This function never returns.
//
void
sched_halt(void)
{
	int i;

	// For debugging and testing purposes, if there are no runnable
	// environments in the system, then drop into the kernel monitor.
	for (i = 0; i < NENV; i++) {
		if ((envs[i].env_status == ENV_RUNNABLE ||
		     envs[i].env_status == ENV_RUNNING ||
		     envs[i].env_status == ENV_DYING))
			break;
	}
	if (i == NENV) {
		cprintf("No runnable environments in the system!\n");
		while (1)
			monitor(NULL);
	}

	// Mark that no environment is running on this CPU
	curenv = NULL;
	lcr3(PADDR(kern_pgdir));

	// Mark that this CPU is in the HALT state, so that when
	// timer interupts come in, we know we should re-acquire the
	// big kernel lock
	xchg(&thiscpu->cpu_status, CPU_HALTED);

	// Release the big kernel lock as if we were "leaving" the kernel
	unlock_kernel();

	// Reset stack pointer, enable interrupts and then halt.
	asm volatile (
		"movl $0, %%ebp\n"
		"movl %0, %%esp\n"
		"pushl $0\n"
		"pushl $0\n"
		// Uncomment the following line after completing exercise 13
		"sti\n"//exercise 13之后，需要去掉!!!
		"1:\n"
		"hlt\n"
		"jmp 1b\n"
	: : "a" (thiscpu->cpu_ts.ts_esp0));
}

