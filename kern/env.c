/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/elf.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/monitor.h>
#include <kern/sched.h>
#include <kern/cpu.h>
#include <kern/spinlock.h>

struct Env *envs = NULL;		// All environments
static struct Env *env_free_list;	// Free environment list
					// (linked by Env->env_link)

#define ENVGENSHIFT	12		// >= LOGNENV

//
// 全局描述符表
// 
// 为内核模式 和 用户模式 设置不同的全局描述符表。
// 段机制 在x86上 有很多目的。
// 我们不是用他们的 内存映射的 特点，但我们需要 用它来进行 特权级别的 转换
// 
// 除了DPL，kernel 和 user的段 是相同的。
// 要加载 对应的 SS 寄存器，CPL(current privilege level)要等于DPL才行
// 
// 最后的那个参数 指定了 全局描述符 权限等级 DPL(Descriptor privilege Level)
// 0表示kernel级别的，3表示user级别的
// 
struct Segdesc gdt[NCPU + 5] = // 前面5个元素定义的 内核、用户的 数据段 和 代码段
// 后面 的NCPU个元素，定义的是 每个CPU自己的 TSS段，先设置为SEG_NULL，后面会在trap_init_percpu()中定义
{
	// 0x0 - unused (always faults -- for trapping NULL far pointers)
	SEG_NULL,

	// 0x8 - kernel code segment
	[GD_KT >> 3] = SEG(STA_X | STA_R, 0x0, 0xffffffff, 0),

	// 0x10 - kernel data segment
	[GD_KD >> 3] = SEG(STA_W, 0x0, 0xffffffff, 0),

	// 0x18 - user code segment
	[GD_UT >> 3] = SEG(STA_X | STA_R, 0x0, 0xffffffff, 3),

	// 0x20 - user data segment
	[GD_UD >> 3] = SEG(STA_W, 0x0, 0xffffffff, 3),

	// 每个CPU自己的TSS段描述符，先设置为SEG_NULL，后面会在trap_init_percpu()中定义
	// 一共有NCPU个
	[GD_TSS0 >> 3] = SEG_NULL
};

struct Pseudodesc gdt_pd = {
	sizeof(gdt) - 1, (unsigned long) gdt
};

//
// Converts an envid to an env pointer.
// If checkperm is set, the specified environment must be either the
// current environment or an immediate child of the current environment.
//
// RETURNS
//   0 on success, -E_BAD_ENV on error.
//   On success, sets *env_store to the environment.
//   On error, sets *env_store to NULL.
//
int
envid2env(envid_t envid, struct Env **env_store, bool checkperm)
{
	struct Env *e;

	// If envid is zero, return the current environment.
	if (envid == 0) {
		*env_store = curenv;
		return 0;
	}

	// Look up the Env structure via the index part of the envid,
	// then check the env_id field in that struct Env
	// to ensure that the envid is not stale
	// (i.e., does not refer to a _previous_ environment
	// that used the same slot in the envs[] array).
	e = &envs[ENVX(envid)];
	if (e->env_status == ENV_FREE || e->env_id != envid) {
		*env_store = 0;
		return -E_BAD_ENV;
	}

	// Check that the calling environment has legitimate permission
	// to manipulate the specified environment.
	// If checkperm is set, the specified environment
	// must be either the current environment
	// or an immediate child of the current environment.
	if (checkperm && e != curenv && e->env_parent_id != curenv->env_id) {
		*env_store = 0;
		return -E_BAD_ENV;
	}

	*env_store = e;
	return 0;
}

// 
// 所有在envs里的元素都设置为free, 把它们的env_ids设为0
// 并把它们插入到env_free的链表中。
// 确保env_free中的顺序 和 数组的顺序是一样的：
// 即需要 从后面开始，使用头插法
//
void
env_init(void)
{
	// Set up envs array
    // LAB 3: Your code here.
    int i;
    env_free_list = NULL;
    for(i=NENV-1; i>=0; i--){
        envs[i].env_id = 0;
        envs[i].env_status = ENV_FREE;
        envs[i].env_link = env_free_list;
        env_free_list = &envs[i];
    }
    // Per-CPU part of the initialization
    env_init_percpu();
}

// 加载新的全局描述符表(GDT) 和 段描述符(segment descriptor)
void
env_init_percpu(void)
{
	lgdt(&gdt_pd);
	// 因为kernel从来不会使用 寄存器GS和FS
	// 所以我们把 user data segment存到 GS和FS中
	asm volatile("movw %%ax,%%gs" : : "a" (GD_UD|3)); //"a"表示 把(GD_UD|3)放入eax。
	asm volatile("movw %%ax,%%fs" : : "a" (GD_UD|3));
	// The kernel does use ES, DS, and SS.  We'll change between
	// the kernel and user data segments as needed.
	asm volatile("movw %%ax,%%es" : : "a" (GD_KD)); //在es，ds，ss中 记录kernel data segment
	asm volatile("movw %%ax,%%ds" : : "a" (GD_KD));
	asm volatile("movw %%ax,%%ss" : : "a" (GD_KD));

	// Load the kernel text segment into CS.
	asm volatile("ljmp %0,$1f\n 1:\n" : : "i" (GD_KT)); // 相当于使用了新的全局描述符表，并且跳到下一行代码
	// For good measure, clear the local descriptor table (LDT),
	// since we don't use it.
	lldt(0);
}

// 
// 给环境e(实际上就是进程)初始化 内核虚拟地址
// 分配页目录项e->env_pgdir
// 初始化e的内核部分的地址空间
// 用户部分的地址空间 不需要进行任何操作
// 
// 如果成功，返回0；如果出错，返回<0。错误包括：
// 		-如果页目录(页) 或者 页表(页) 不能 被分配，那么返回 E_NO_MEM
// 
static int
env_setup_vm(struct Env *e)
{
	int i;
	struct PageInfo *p = NULL;

	// 分配一个物理页，作为 页目录 的物理页
	if (!(p = page_alloc(ALLOC_ZERO)))
		return -E_NO_MEM;

	// 现在，设置e->env_pgdir并初始化页目录
	// 
	// 提示：
	//     - 对于每个envs来说，除了UVPT地方的内容，UTOP上面的虚拟地址空间都是一样的
	// 		通过inc/memlayout.h来查看虚拟地址空间的 权限 和 布局
	// 		可以使用kern_pgdir作为一个模板
	//     - 在UTOP之下的VA都是空的
	//     - 后面不再需要显式地调用page_alloc了
	//     - 注意：通常来说，高于UTOP的虚拟地址对应的物理页的pp_ref 是不需要维护的。
	// 		但是 在UVPT的env_pgdir是一个例外：你需要对env_pgdir的pp_ref进行自增
	//		(我估计是为了以后多线程?)
	// 	   - kern/pmap.h里面的函数 很有用。
	//

	// LAB 3: Your code here.
	e->env_pgdir = (pde_t *)page2kva(p);
    p->pp_ref++;

    // 低于UTOP的页目录的内容设为空
    for(i = 0; i < PDX(UTOP); i++) {
        e->env_pgdir[i] = 0;        
    }

    // 高于UTOP的页目录的内容 通过kern_pgdir来设置
    for(i = PDX(UTOP); i < NPDENTRIES; i++) {
        e->env_pgdir[i] = kern_pgdir[i];
    }

	// UVPT处对应的是env自己的 页目录页，它是只读的
	// 权限：内核读/用户读
	e->env_pgdir[PDX(UVPT)] = PADDR(e->env_pgdir) | PTE_P | PTE_U;

	return 0;
}

// 
// 分配和初始化一个environment
// 如果成功，那么新的环境存在*newenv_store中
// 
// 如果成功，返回0；如果失败，返回<0。错误包括：
// 		- E_NO_FREE_ENV: 所有NENV个环境都已经分配满了
// 		- E_NO_MEM: 物理内存已经用完
// 
int
env_alloc(struct Env **newenv_store, envid_t parent_id)
{
	int32_t generation;
	int r;
	struct Env *e;

	if (!(e = env_free_list))
		return -E_NO_FREE_ENV;

	// Allocate and set up the page directory for this environment.
	if ((r = env_setup_vm(e)) < 0)
		return r;

	// Generate an env_id for this environment.
	// generation 记录的是 第i代 使用这个e的 进程。
	// 尽管我们只需要用最右边的10位作为index，但 generation 是从第13位 才开始计数
	// 注意：最左那位是 符号位
	generation = (e->env_id + (1 << ENVGENSHIFT)) & ~(NENV - 1);
	if (generation <= 0)	// Don't create a negative env_id.
		generation = 1 << ENVGENSHIFT;
	e->env_id = generation | (e - envs); // | (e - envs)表示 后面10位 放的是 它在envs的index

	// Set the basic status variables.
	e->env_parent_id = parent_id;
	e->env_type = ENV_TYPE_USER;
	e->env_status = ENV_RUNNABLE;
	e->env_runs = 0;

	// 清空寄存器的值
	memset(&e->env_tf, 0, sizeof(e->env_tf));

	// 给段寄存器设置合适的初始值
	// GD_UD是GDT中的用户数据段选择子 user data segment selector
	// GD_UT是GDT中的用户代码段选择子 user text segment selector
	// 每个段寄存器的最低2位 记录了RPL的值(Requstor Privilege Level)
	// 0表示kernel mode，3表示user mode
	// 当我们切换上下文的时候，例如，从用户态切换成 内核态
	// CPL(current privilege level)从3变为0，但RPL依旧为3
	// 硬件通过RPL，而不是CPL来检查 是否有权限访问 对应的段(DPL: Descriptor Privilege Level) 
	e->env_tf.tf_ds = GD_UD | 3;
	e->env_tf.tf_es = GD_UD | 3;
	e->env_tf.tf_ss = GD_UD | 3;
	e->env_tf.tf_esp = USTACKTOP;
	e->env_tf.tf_cs = GD_UT | 3;
	// e->env_tf.tf_eip 会在后面的load_icode函数中设置

	// 用户模式下，允许中断
	// LAB 4: Your code here.
	e->env_tf.tf_eflags |= FL_IF;

	// Clear the page fault handler until user installs one.
	e->env_pgfault_upcall = 0;

	// Also clear the IPC receiving flag.
	e->env_ipc_recving = 0;

	// 完成分配
	env_free_list = e->env_link; //将这个e移出env_free_list
	*newenv_store = e;

	// cprintf("[%08x] new env %08x\n", curenv ? curenv->env_id : 0, e->env_id);
	return 0;
}

// 
// 为环境env分配len字节的物理内存，并把它映射到env的虚拟地址中va的位置
// Does not zero or otherwise initialize the mapped pages in any way.
// 对于分配到的物理页，不要置0或者初始化。
// 物理页对于用户和kernel来说，都是可写的。
// 如果分配失败，那么抛出异常。
// 
static void
region_alloc(struct Env *e, void *va, size_t len)
{
	// LAB 3: Your code here.
	// (But only if you need it for load_icode.)
	// 
	// 提示：调用者 调用region_alloc时，传递的va和len参数，可能不是对齐的(page size的倍数)
	// 需要先将va往下对齐，把va+len往上对齐
	// (注意 边界情况!)
	// 
	void* start = (void *)ROUNDDOWN((uint32_t)va, PGSIZE);
    void* end = (void *)ROUNDUP((uint32_t)va+len, PGSIZE);
    struct PageInfo *p = NULL;
    void* i;
    int r;
    for(i=start; i<end; i+=PGSIZE){
        p = page_alloc(0);
        if(p == NULL)
            panic(" region alloc, allocation failed.");

        r = page_insert(e->env_pgdir, p, i, PTE_W | PTE_U);
        if(r != 0) {
            panic("region alloc error");
        }
    }
}

// 
// 对一个用户进程，设置 初始的 二进制程序，栈 和 处理器的标志位
// 这个程序 仅仅 是在内核初始化时，在运行第一个 用户环境 前调用的。
// 
// 这个函数 往用户的地址空间 装载了所有 可以装载的ELF二进制镜像的 段(segment)。
// 起始的虚拟地址的 位置是在ELF程序的头部说明的。
// 与此同时，对 那些在进程头部 标记说 映射但实际不存在的内容 清空置0
// (也就是bss section)
// 
// 这些操作 和 boot loader 过程很类似，但 boot loader需要从磁盘来读取代码
// 可以 通过boot/main.c来获取 一些idea
// 
// 最后，这个函数 为程序的初始栈 映射了一个物理页。
// 
// 如果load_icode出错，那么 跑出异常。
// 		- load_icode会出现什么错误呢？输入的参数 会有什么潜在的问题吗？
//
static void
load_icode(struct Env *e, uint8_t *binary)
{
	// 提示：
	//   - 把程序 的每个segment都装载进 ELF头部指定的虚拟内存中
	// 	 - 只能加载那些ph->p_type==ELF_PROG_LOAD的segment
	//   - segment的加载地址为ph->p_va，大小为ph->p_memsz
	//   - 从'binary+ph->p_offset'开始的ph->p_filesz的内容
	// 	  要被加载到ph->p_va中。剩余的部分要置0。即>p_filesz且<=p_memsz的部分
	// 	 - 使用之前lab中的函数来 分配和映射 物理页。
	// 
	// 	 - 所有页的保护位现在 都应 设为 对用户 可读/可写
	// 	 - ELF的segment不一定是对齐到page size的
	//	但你可以 假设2个segment不会 访问到相同的virtual page
	// 	 
	// 	 - 使用region_alloc函数
	// 
	// 	 - 如果你可以把数据 直接 移动到 ELF文件定义的 虚拟地址上的话，
	// 	加载 segment会 变得十分简单。
	//  So which page directory should be in force during
	//  this function?
	//
	//   - 对于程序的入口，你也需要做一些事情 来保证 程序确实从那个地方开始执行的。
	//  	What?  (See env_run() and env_pop_tf() below.)

	// LAB 3: Your code here.
 	struct Elf* header = (struct Elf*)binary;
    
    if(header->e_magic != ELF_MAGIC) { // 检查ELF文件的标记位
        panic("load_icode failed: The binary we load is not elf.\n");
    }

    if(header->e_entry == 0){
        panic("load_icode failed: The elf file can't be excuterd.\n");
    }

    e->env_tf.tf_eip = header->e_entry; //eip设为 elf的入口entry

    lcr3(PADDR(e->env_pgdir)); //应该是在硬件上设置 开启分页，且 页目录为e->env_pgdir

    struct Proghdr *ph, *eph;
    ph = (struct Proghdr* )((uint8_t *)header + header->e_phoff); // program header开始的位置
    eph = ph + header->e_phnum; //end of program header
    for(; ph < eph; ph++) {
        if(ph->p_type == ELF_PROG_LOAD) {
            if(ph->p_memsz - ph->p_filesz < 0) {
                panic("load icode failed : p_memsz < p_filesz.\n");
            }

            region_alloc(e, (void *)ph->p_va, ph->p_memsz);
            memmove((void *)ph->p_va, binary + ph->p_offset, ph->p_filesz);
			//从'binary+ph->p_offset'开始的ph->p_filesz的内容，复制到ph->p_va中
            memset((void *)(ph->p_va + ph->p_filesz), 0, ph->p_memsz - ph->p_filesz);
			//剩余的部分要置0。即>p_filesz且<=p_memsz的部分需要置0
        }
    }

	// 现在 在USTACKTOP-PGSIZE的位置上，为用户进程的 栈分配一个PGSIZE大小的栈
	// LAB 3: Your code here.
	region_alloc(e,(void *)(USTACKTOP-PGSIZE), PGSIZE);
}

// 
// 通过env_alloc来分配一个新的env
// 通过load_icode来加载elf二进制文件，并设置env_type
// 
// 这个函数只能在 内核初始化时调用(即在 第一个用户进程运行之前调用)
// 新的env的parent ID设为0
// 
void
env_create(uint8_t *binary, enum EnvType type)
{
	// LAB 3: Your code here.
	struct Env *e;
    int rc;
    if((rc = env_alloc(&e, 0)) != 0) {
        panic("env_create failed: env_alloc failed.\n");
    }

    load_icode(e, binary);
    e->env_type = type;

	// If this is the file server (type == ENV_TYPE_FS) give it I/O privileges.
	// LAB 5: Your code here.
	if (type == ENV_TYPE_FS) { //如果是文件系统，为他设置IO访问权限
        e->env_tf.tf_eflags |= FL_IOPL_MASK;
    }
}

//
// Frees env e and all memory it uses.
//
void
env_free(struct Env *e)
{
	pte_t *pt;
	uint32_t pdeno, pteno;
	physaddr_t pa;

	// If freeing the current environment, switch to kern_pgdir
	// before freeing the page directory, just in case the page
	// gets reused.
	if (e == curenv)
		lcr3(PADDR(kern_pgdir));

	// Note the environment's demise.
	// cprintf("[%08x] free env %08x\n", curenv ? curenv->env_id : 0, e->env_id);

	// Flush all mapped pages in the user portion of the address space
	static_assert(UTOP % PTSIZE == 0);
	for (pdeno = 0; pdeno < PDX(UTOP); pdeno++) {

		// only look at mapped page tables
		if (!(e->env_pgdir[pdeno] & PTE_P))
			continue;

		// find the pa and va of the page table
		pa = PTE_ADDR(e->env_pgdir[pdeno]);
		pt = (pte_t*) KADDR(pa);

		// unmap all PTEs in this page table
		for (pteno = 0; pteno <= PTX(~0); pteno++) {
			if (pt[pteno] & PTE_P)
				page_remove(e->env_pgdir, PGADDR(pdeno, pteno, 0));
		}

		// free the page table itself
		e->env_pgdir[pdeno] = 0;
		page_decref(pa2page(pa));
	}

	// free the page directory
	pa = PADDR(e->env_pgdir);
	e->env_pgdir = 0;
	page_decref(pa2page(pa));

	// return the environment to the free list
	e->env_status = ENV_FREE;
	e->env_link = env_free_list;
	env_free_list = e;
}

//
// Frees environment e.
// If e was the current env, then runs a new environment (and does not return
// to the caller).
//
void
env_destroy(struct Env *e)
{
	// If e is currently running on other CPUs, we change its state to
	// ENV_DYING. A zombie environment will be freed the next time
	// it traps to the kernel.
	if (e->env_status == ENV_RUNNING && curenv != e) {
		e->env_status = ENV_DYING;
		return;
	}

	env_free(e);

	if (curenv == e) {
		curenv = NULL;
		sched_yield();
	}
}


//
// Restores the register values in the Trapframe with the 'iret' instruction.
// This exits the kernel and starts executing some environment's code.
//
// This function does not return.
//
void
env_pop_tf(struct Trapframe *tf)
{
	// Record the CPU we are running on for user-space debugging
	curenv->env_cpunum = cpunum();

	asm volatile(
		"\tmovl %0,%%esp\n" //把tf的内容放到esp中
		"\tpopal\n" //通过pop esp中的内容，恢复所有的r32的寄存器
		"\tpopl %%es\n"
		"\tpopl %%ds\n"
		"\taddl $0x8,%%esp\n" /* skip tf_trapno and tf_errcode */
		"\tiret\n" //iret是 中断返回指令
		: : "g" (tf) : "memory");
	panic("iret failed");  /* mostly to placate the compiler */
}

// 
// 上下文 从curenv 切换到e
// 注意：如果是第一次运行env_run，那么curenv是NULL
// 
// 这个方程没有返回值
// 
void
env_run(struct Env *e)
{
	// Step1: 如果这是一个上下文的切换：
	// 		1. 设置当前正在运行(ENV_RUNNING)的进程的状态为ENV_RUNNABLE
	// 		2. 设置curenv为新的环境
	// 		3. 设置curenv的状态为ENV_RUNNING
	// 		4. 更新它的env_runs的计数(估计是用来做一些 换入、换出的策略的)
	// 		5. 用lcr3()来切换到它的地址空间
	// 
	// Step2: 使用env_pop_tf()函数，来恢复 进程的寄存器，并 进入用户模式 
	// 
	// 提示：这个函数 通过e->env_tf来 加载新的进程的状态。
	// 检查你之前写的代码，确保你 设置了e->env_tf的相关内容
	// 其中，我们在load_icode中设置了eip，也就是程序的入口。
	// 在env_alloc中，设置了别的寄存器的内容。
	
	// LAB 3: Your code here.
	if(curenv != NULL && curenv->env_status == ENV_RUNNING) {
        curenv->env_status = ENV_RUNNABLE;
    }

    curenv = e;
    curenv->env_status = ENV_RUNNING;
    curenv->env_runs++;
    lcr3(PADDR(curenv->env_pgdir));
	unlock_kernel();//释放锁

    env_pop_tf(&curenv->env_tf);
	// 通过e->env_tf的内容 来恢复寄存器，实现切换进程的上下文。

	panic("env_run not yet implemented");
}

