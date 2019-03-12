#include <inc/mmu.h>
#include <inc/x86.h>
#include <inc/assert.h>

#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/env.h>
#include <kern/syscall.h>
#include <kern/sched.h>
#include <kern/kclock.h>
#include <kern/picirq.h>
#include <kern/cpu.h>
#include <kern/spinlock.h>
#include <kern/time.h>

static struct Taskstate ts; // 这个ts在lab4中 应该是没用的了
// 在跳转到中断处理程序执行之前
// 把 处理器的状态 保存到ts中，处理完中断后，再恢复

/* For debugging, so print_trapframe can distinguish between printing
 * a saved trapframe and printing the current trapframe and print some
 * additional information in the latter case.
 */
static struct Trapframe *last_tf;

/* Interrupt descriptor table.  (Must be built at run time because
 * shifted function addresses can't be represented in relocation records.)
 */
struct Gatedesc idt[256] = { { 0 } };
struct Pseudodesc idt_pd = {
	sizeof(idt) - 1, (uint32_t) idt
};

void t_divide();
void t_debug();
void t_nmi();
void t_brkpt();
void t_oflow();
void t_bound();
void t_illop();
void t_device();
void t_dblflt();
void t_tss();
void t_segnp();
void t_stack();
void t_gpflt();
void t_pgflt();
void t_fperr();
void t_align();
void t_mchk();
void t_simderr();
void t_syscall();

void irq_timer();
void irq_kbd();
void irq_serial();
void irq_spurious();
void irq_ide();
void irq_error();

static const char *trapname(int trapno)
{
	static const char * const excnames[] = {
		"Divide error",
		"Debug",
		"Non-Maskable Interrupt",
		"Breakpoint",
		"Overflow",
		"BOUND Range Exceeded",
		"Invalid Opcode",
		"Device Not Available",
		"Double Fault",
		"Coprocessor Segment Overrun",
		"Invalid TSS",
		"Segment Not Present",
		"Stack Fault",
		"General Protection",
		"Page Fault",
		"(unknown trap)",
		"x87 FPU Floating-Point Error",
		"Alignment Check",
		"Machine-Check",
		"SIMD Floating-Point Exception"
	};

	if (trapno < ARRAY_SIZE(excnames))
		return excnames[trapno];
	if (trapno == T_SYSCALL)
		return "System call";
	if (trapno >= IRQ_OFFSET && trapno < IRQ_OFFSET + 16)
		return "Hardware Interrupt";
	return "(unknown trap)";
}


void
trap_init(void)
{
	extern struct Segdesc gdt[];

	// LAB 3: Your code here.
	SETGATE(idt[T_DIVIDE], 0, GD_KT, t_divide, 0);
	SETGATE(idt[T_DEBUG], 0, GD_KT, t_debug, 0);
	SETGATE(idt[T_NMI], 0, GD_KT, t_nmi, 0);
	SETGATE(idt[T_BRKPT], 0, GD_KT, t_brkpt, 3); //断点
	SETGATE(idt[T_OFLOW], 0, GD_KT, t_oflow, 0);
	SETGATE(idt[T_BOUND], 0, GD_KT, t_bound, 0);
	SETGATE(idt[T_ILLOP], 0, GD_KT, t_illop, 0);
	SETGATE(idt[T_DEVICE], 0, GD_KT, t_device, 0);
	SETGATE(idt[T_DBLFLT], 0, GD_KT, t_dblflt, 0);
	SETGATE(idt[T_TSS], 0, GD_KT, t_tss, 0);
	SETGATE(idt[T_SEGNP], 0, GD_KT, t_segnp, 0);
	SETGATE(idt[T_STACK], 0, GD_KT, t_stack, 0);
	SETGATE(idt[T_GPFLT], 0, GD_KT, t_gpflt, 0);
	SETGATE(idt[T_PGFLT], 0, GD_KT, t_pgflt, 0);
	SETGATE(idt[T_FPERR], 0, GD_KT, t_fperr, 0);
	SETGATE(idt[T_ALIGN], 0, GD_KT, t_align, 0);
	SETGATE(idt[T_MCHK], 0, GD_KT, t_mchk, 0);
	SETGATE(idt[T_SIMDERR], 0, GD_KT, t_simderr, 0);
	SETGATE(idt[T_SYSCALL], 0, GD_KT, t_syscall, 3);
	//系统调用是允许用户模式下调用的

	//trap.c的trap_init()
	SETGATE(idt[IRQ_OFFSET+IRQ_TIMER], 0, GD_KT, irq_timer, 0);
	SETGATE(idt[IRQ_OFFSET+IRQ_KBD], 0, GD_KT, irq_kbd, 0);
	SETGATE(idt[IRQ_OFFSET+IRQ_SERIAL], 0, GD_KT, irq_serial, 0);
	SETGATE(idt[IRQ_OFFSET+IRQ_SPURIOUS], 0, GD_KT, irq_spurious, 0);
	SETGATE(idt[IRQ_OFFSET+IRQ_IDE], 0, GD_KT, irq_ide, 0);
	SETGATE(idt[IRQ_OFFSET+IRQ_ERROR], 0, GD_KT, irq_error, 0);

	// Per-CPU setup 
	trap_init_percpu();
}

// Initialize and load the per-CPU TSS and IDT
// 初始化 和 加载 每个CPU的 TSS和IDT
void
trap_init_percpu(void)
{
	// 这里的样例代码 为CPU0建立了Task State Segment(TSS)和TSS 描述符。
	// 但如果 要运行在别的CPU上，这里的代码是不正确的，因为每个CPU都有它自己的内核栈
	// 修改这里的代码，使得 多CPU环境下也能运行。
	// 
	// 提示：
	// 	 - thiscpu宏 通常 指向 当前CPU的CpuInfo的结构体
	// 	 - 当前CPU的ID 可以通过cpunum()或者thiscpu->cpu_id来获得
	// 	 - 使用“thiscpu->cpu_ts”作为当前CPU的TSS，而不是用全局变量ts
	//   - 使用gdt[(GD_TSS0 >> 3) + i]来索引 第i个CPU的TSS段描述符
	// 	 - 在mem_init_mp()中映射每个CPU的内核栈 (不是已经映射过了吗???exercise3的时候)
	//   - 初始化 cpu_ts.ts_iomb 来避免 未授权的进程 来使用IO(直接设为0是不对的!)
	// 
	// ltr会在TSS选择子中 设置busy的标识符(flag)
	// 所以 如果你 在不同的CPU上加载了同一个TSS，会报 “triple fault”
	// 
	// 如果，你把某个CPU的TSS设置错了，你可能 要等到从用户空间返回到 那个CPU时，
	// 才会报错
	// 
	// LAB 4: Your code here:
	int cpu_id = thiscpu->cpu_id;
	struct Taskstate *this_ts = &thiscpu->cpu_ts;

	// Setup a TSS so that we get the right stack
	// when we trap to the kernel.
	this_ts->ts_esp0 = KSTACKTOP - cpu_id * (KSTKSIZE + KSTKGAP);
	this_ts->ts_ss0 = GD_KD;
	this_ts->ts_iomb = sizeof(struct Taskstate);

	// Initialize the TSS slot of the gdt.
	gdt[(GD_TSS0 >> 3) + cpu_id] = SEG16(STS_T32A, (uint32_t) (this_ts),
					sizeof(struct Taskstate) - 1, 0);
	gdt[(GD_TSS0 >> 3) + cpu_id].sd_s = 0;

	// Load the TSS selector (like other segment selectors, the
	// bottom three bits are special; we leave them 0)
	ltr(GD_TSS0 + (cpu_id << 3));

	// Load the IDT 因为 每个CPU的idt实际都是一样的，所以直接加载就可以了
	lidt(&idt_pd);
}

void
print_trapframe(struct Trapframe *tf)
{
	cprintf("TRAP frame at %p from CPU %d\n", tf, cpunum());
	print_regs(&tf->tf_regs);
	cprintf("  es   0x----%04x\n", tf->tf_es);
	cprintf("  ds   0x----%04x\n", tf->tf_ds);
	cprintf("  trap 0x%08x %s\n", tf->tf_trapno, trapname(tf->tf_trapno));
	// If this trap was a page fault that just happened
	// (so %cr2 is meaningful), print the faulting linear address.
	if (tf == last_tf && tf->tf_trapno == T_PGFLT)
		cprintf("  cr2  0x%08x\n", rcr2());
	cprintf("  err  0x%08x", tf->tf_err);
	// For page faults, print decoded fault error code:
	// U/K=fault occurred in user/kernel mode
	// W/R=a write/read caused the fault
	// PR=a protection violation caused the fault (NP=page not present).
	if (tf->tf_trapno == T_PGFLT)
		cprintf(" [%s, %s, %s]\n",
			tf->tf_err & 4 ? "user" : "kernel",
			tf->tf_err & 2 ? "write" : "read",
			tf->tf_err & 1 ? "protection" : "not-present");
	else
		cprintf("\n");
	cprintf("  eip  0x%08x\n", tf->tf_eip);
	cprintf("  cs   0x----%04x\n", tf->tf_cs);
	cprintf("  flag 0x%08x\n", tf->tf_eflags);
	if ((tf->tf_cs & 3) != 0) {
		cprintf("  esp  0x%08x\n", tf->tf_esp);
		cprintf("  ss   0x----%04x\n", tf->tf_ss);
	}
}

void
print_regs(struct PushRegs *regs)
{
	cprintf("  edi  0x%08x\n", regs->reg_edi);
	cprintf("  esi  0x%08x\n", regs->reg_esi);
	cprintf("  ebp  0x%08x\n", regs->reg_ebp);
	cprintf("  oesp 0x%08x\n", regs->reg_oesp);
	cprintf("  ebx  0x%08x\n", regs->reg_ebx);
	cprintf("  edx  0x%08x\n", regs->reg_edx);
	cprintf("  ecx  0x%08x\n", regs->reg_ecx);
	cprintf("  eax  0x%08x\n", regs->reg_eax);
}

static void
trap_dispatch(struct Trapframe *tf)
{
	// Handle processor exceptions.
	// LAB 3: Your code here.
	if (tf->tf_trapno == T_PGFLT) {
		return page_fault_handler(tf);
	}

	if (tf->tf_trapno == T_BRKPT) {
		return monitor(tf);
	}

	if (tf->tf_trapno == T_SYSCALL) {
		tf->tf_regs.reg_eax = syscall(
			tf->tf_regs.reg_eax,
			tf->tf_regs.reg_edx,
			tf->tf_regs.reg_ecx,
			tf->tf_regs.reg_ebx,
			tf->tf_regs.reg_edi,
			tf->tf_regs.reg_esi
		);
		return;
	}

	// Handle spurious interrupts
	// The hardware sometimes raises these because of noise on the
	// IRQ line or other reasons. We don't care.
	if (tf->tf_trapno == IRQ_OFFSET + IRQ_SPURIOUS) {
		cprintf("Spurious interrupt on irq 7\n");
		print_trapframe(tf);
		return;
	}

	// Handle clock interrupts. Don't forget to acknowledge the
	// interrupt using lapic_eoi() before calling the scheduler!
	// LAB 4: Your code here.
	if (tf->tf_trapno == IRQ_OFFSET + IRQ_TIMER) {
       lapic_eoi();
	   if (thiscpu->cpu_id == 0){
		   time_tick();
	   }
       sched_yield();
       return;
	}

	// Add time tick increment to clock interrupts.
	// Be careful! In multiprocessors, clock interrupts are
	// triggered on every CPU.
	// LAB 6: Your code here.


	// Handle keyboard and serial interrupts.
	// LAB 5: Your code here.
	if (tf->tf_trapno == IRQ_OFFSET + IRQ_KBD) {
		kbd_intr();
		return;
	}

	if (tf->tf_trapno == IRQ_OFFSET + IRQ_SERIAL) {
		serial_intr();
		return;
	}	

	// Unexpected trap: The user process or the kernel has a bug.
	print_trapframe(tf);
	if (tf->tf_cs == GD_KT)
		panic("unhandled trap in kernel");
	else {
		env_destroy(curenv);
		return;
	}
}

void
trap(struct Trapframe *tf)
{
	// The environment may have set DF and some versions
	// of GCC rely on DF being clear
	asm volatile("cld" ::: "cc");//cld表示 清除方向标志

	// Halt the CPU if some other CPU has called panic()
	extern char *panicstr;
	if (panicstr)
		asm volatile("hlt");

	// Re-acqurie the big kernel lock if we were halted in
	// sched_yield()
	if (xchg(&thiscpu->cpu_status, CPU_STARTED) == CPU_HALTED)
		lock_kernel();
	// Check that interrupts are disabled.  If this assertion
	// fails, DO NOT be tempted to fix it by inserting a "cli" in
	// the interrupt path.
	assert(!(read_eflags() & FL_IF));

	if ((tf->tf_cs & 3) == 3) { // cs的最后3位
		// Trapped from user mode.
		// Acquire the big kernel lock before doing any
		// serious kernel work.
		// LAB 4: Your code here.
		lock_kernel();
		assert(curenv);

		// Garbage collect if current enviroment is a zombie
		if (curenv->env_status == ENV_DYING) {
			env_free(curenv);
			curenv = NULL;
			sched_yield();
		}

		// Copy trap frame (which is currently on the stack)
		// into 'curenv->env_tf', so that running the environment
		// will restart at the trap point.
		curenv->env_tf = *tf;
		// The trapframe on the stack should be ignored from here on.
		tf = &curenv->env_tf;
	}

	// Record that tf is the last real trapframe so
	// print_trapframe can print some additional information.
	last_tf = tf;

	// Dispatch based on what type of trap occurred
	trap_dispatch(tf);

	// If we made it to this point, then no other environment was
	// scheduled, so we should return to the current environment
	// if doing so makes sense.
	if (curenv && curenv->env_status == ENV_RUNNING)
		env_run(curenv);
	else
		sched_yield();
}


void
page_fault_handler(struct Trapframe *tf)
{
	uint32_t fault_va;

	// Read processor's CR2 register to find the faulting address
	fault_va = rcr2();

	// Handle kernel-mode page faults.

	// LAB 3: Your code here.
	if ((tf->tf_cs & 3) == 0) { //cs的最低3位 放的是CPL
        panic("kernel page fault at:%x\n", fault_va);
    } 

	// 我们已经处理了kernel模式的异常了，如果我们运行到这个位置，说明在 用户模式出现了page fault.
	// 
	// 如果进程存在page fault upcall,那么调用它。
	// 在exception stack上面设置一个page fault的stack frame(在UXSTACKTOP之下)
	// 然后去执行curenv->env_pgfault_upcall
	// 
	// page fault upcall页可能会 产生另一个page fault, 然后递归出现page fault，
	// 然后又 把另一个page fault stack frame压入 用户exception stack
	// 
	// 在lib/pfentry.S中返回的代码中，在trap-time stack的栈顶 需要加入一个word的额外空间
	// 这让我们更加方便地恢复eip/esp
	// 在非递归的情况中，我们不需要担心这种情况，因为正常的用户栈的顶部是空的。
	// 在递归的情况中，我们需要在 当前exceptio stack的栈顶留一个word的长度，
	// 因为exception stack是trap-time stack。
	// 
	// It is convenient for our code which returns from a page fault
	// (lib/pfentry.S) to have one word of scratch space at the top of the
	// trap-time stack; it allows us to more easily restore the eip/esp. In
	// the non-recursive case, we don't have to worry about this because
	// the top of the regular user stack is free.  In the recursive case,
	// this means we have to leave an extra word between the current top of
	// the exception stack and the new stack frame because the exception
	// stack _is_ the trap-time stack.

	// 如果没有page fault upcall，或者 进程不能在exception stack上分配物理页 或 没有写权限，
	// 或者 exception stack溢出了，那么 直接杀死那个出错的进程。
	//
	// If there's no page fault upcall, the environment didn't allocate a
	// page for its exception stack or can't write to it, or the exception
	// stack overflows, then destroy the environment that caused the fault.
	// 
	// 注意：打分的脚本 假设你先 检查page fault upcall，
	// 如果出错，然后打印 "user fault va"消息。
	// 剩下的三个check可以合并到一个test中。
	// 
	// 提示：
	//   user_mem_assert()和env_run()在这里很有用。
	//   要改变 用户进程运行的内容，修改'curenv->env_tf'
	//   tf变量指向curenv->env_tf

	// LAB 4: Your code here.
	if (curenv->env_pgfault_upcall) {
        struct UTrapframe *utf;
        if (tf->tf_esp >= UXSTACKTOP-PGSIZE && tf->tf_esp <= UXSTACKTOP-1) {
			// 已经在User Exception Stack中了：嵌套递归的page fault
            utf = (struct UTrapframe *)(tf->tf_esp - sizeof(struct UTrapframe) - 4);
			// 递归的话，需要保留4个字节(一个word的大小)
        } else {
            utf = (struct UTrapframe *)(UXSTACKTOP - sizeof(struct UTrapframe));
        }   

        user_mem_assert(curenv, (void*)utf, 1, PTE_W);//检查User Exception Stack是否溢出了
        utf->utf_fault_va = fault_va;
        utf->utf_err = tf->tf_err;
        utf->utf_regs = tf->tf_regs;
        utf->utf_eip = tf->tf_eip;
        utf->utf_eflags = tf->tf_eflags;
        utf->utf_esp = tf->tf_esp;

        curenv->env_tf.tf_eip = (uintptr_t)curenv->env_pgfault_upcall;
		//把eip指向pgfault_upcall的地方
        curenv->env_tf.tf_esp = (uintptr_t)utf; //把栈切换到 User Exception Stack上
		//一个struct的内容在栈中的位置：先出现的属性放在低地址的地方，后出现的属性放在低地址的地方。
		//所以utf现在指向栈的top的位置
        env_run(curenv);
    } 

	// Destroy the environment that caused the fault.
	cprintf("[%08x] user fault va %08x ip %08x\n",
		curenv->env_id, fault_va, tf->tf_eip);
	print_trapframe(tf);
	env_destroy(curenv);
}

