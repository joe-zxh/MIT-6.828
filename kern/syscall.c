/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/syscall.h>
#include <kern/console.h>
#include <kern/sched.h>

// Print a string to the system console.
// The string is exactly 'len' characters long.
// Destroys the environment on memory errors.
static void
sys_cputs(const char *s, size_t len)
{
	// Check that the user has permission to read memory [s, s+len).
	// Destroy the environment if not.

	// LAB 3: Your code here.
	user_mem_assert(curenv, s, len, 0);
	//检查用户程序 是否有 对虚拟地址空间[s, s+len]的访问权限

	// Print the string supplied by the user. ???这个难道不是/kern/cprintf???
	cprintf("%.*s", len, s);
}

// Read a character from the system console without blocking.
// Returns the character, or 0 if there is no input waiting.
static int
sys_cgetc(void)
{
	return cons_getc();
}

// Returns the current environment's envid.
static envid_t
sys_getenvid(void)
{
	return curenv->env_id;
}

// Destroy a given environment (possibly the currently running environment).
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_destroy(envid_t envid)
{
	int r;
	struct Env *e;

	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;
	env_destroy(e);
	return 0;
}

// Deschedule current environment and pick a different one to run.
static void
sys_yield(void)
{
	sched_yield();
}

// 分配一个新的进程
// 返回新进程的envid。如果出错，返回<0。错误包括:
//   -E_NO_FREE_ENV: 达到 系统的总的 进程个数的上限
// 	 -E_NO_MEM: 物理内存不足
static envid_t
sys_exofork(void)
{
	// 通过kern/env.c中的env_alloc()创建新的进程
	// 进程状态设为ENV_NOT_RUNNABLE，把当前进程中的寄存器的内容 复制给 新的进程
	// 微调一下，使得sys_exofork在子进程中返回0，在父进程中 返回childrenId

	// LAB 4: Your code here.
	// panic("sys_exofork not implemented");
	struct Env *e;
	int ret = env_alloc(&e, curenv->env_id);
	if (ret<0){
		return ret;
	}
	e->env_status = ENV_NOT_RUNNABLE;
	e->env_tf = curenv->env_tf; // 复制寄存器的值
	e->env_tf.tf_regs.reg_eax = 0; // 子进程返回0. 而父进程返回e->env_id
	// 具体为什么，可以参考https://www.jianshu.com/p/10f822b3deda?utm_campaign=maleskine&utm_content=note&utm_medium=seo_notes&utm_source=recommendation
	return e->env_id;
}

// 把envid对应的进程状态设置为status，status参数必须为ENV_RUNNABLE或ENV_NOT_RUNNABLE
// 
// 如果成功，返回0；失败，返回<0。错误有：
//   -E_BAD_ENV: envid对应的进程不存在，或者 调用者没有修改权限
//   -E_INVAL: status参数 不合法
static int
sys_env_set_status(envid_t envid, int status)
{
	// 提示：使用kern/env.c中的函数envid2env来把一个envid转换成一个进程Env
	// envid2env的第3个参数需要设置成1，用于检查是否有修改进程的权限

	// LAB 4: Your code here.
	// panic("sys_env_set_status not implemented");
	struct Env *e;
    if (envid2env(envid, &e, 1)){
		return -E_BAD_ENV;
	}
    
    if (status != ENV_NOT_RUNNABLE && status != ENV_RUNNABLE){
		return -E_INVAL;
	}
    
    e->env_status = status;
    return 0;
}


// 把envid对应的进程的trap frame设为tf
// tf修改了一下，使得 用户进程 在CPL 3(code protection level3)下运行、允许中断、IOPL=0
// 
// 成功返回0，失败返回<0。错误有：
//   -E_BAD_ENV: 如果envid对应的进程不存在，或者 调用者 没有修改权限
static int
sys_env_set_trapframe(envid_t envid, struct Trapframe *tf)
{
	// LAB 5: Your code here.
	// 记得检查 用户是否提供了 一个好的地址
	// panic("sys_env_set_trapframe not implemented");

	struct Env *e;
	if (envid2env(envid, &e, 1)) {
			return -E_BAD_ENV;
	}

	e->env_tf = *tf;
	e->env_tf.tf_cs |= 3;
	e->env_tf.tf_eflags |= FL_IF;
	e->env_tf.tf_eflags &= ~FL_IOPL_MASK;
	return 0;
}

// 为envid对应的进程设置page fault upcall
// 但envid对应的进程出现了page fault，内核会 把一个fault近路压进exception栈，
// 然后 转去执行func
// 
// 如果成功，返回0；如果失败，返回<0。错误有：
//   -E_BAD_ENV: envid对应的进程不存在 或者 调用这没有修改权限
static int
sys_env_set_pgfault_upcall(envid_t envid, void *func)
{
	// LAB 4: Your code here.
	// panic("sys_env_set_pgfault_upcall not implemented");
	struct Env *e; 
    if (envid2env(envid, &e, 1)){
		return -E_BAD_ENV;
	}
    e->env_pgfault_upcall = func;
    return 0;
}

// 为id为envid的进程 分配一个物理页，把它映射到va的位置，且设置权限为perm
// 物理页的内容设为0
// 如果一个va已经映射了一个物理页了，那么 那个物理页就需要unmap一下
// 
// 参数perm：PTE_U | PTE_P是一定 要设置的，PTE_AVAIL | PTE_W可以设置，也可以不设置
// 除这4个选项之外，不能设置别的权限。参考inc/mmu.h的 PTE_SYSCALL
// 
// 如果成功，返回0；如果失败，返回<0。错误有：
//   -E_BAD_ENV: envid的进程不存在，或者 调用者没有权限修改这个进程的。
// 	 -E_INVAL: va大于UTOP，或者va并不是 pageSize的整数倍
//   -E_INVAL: perm的设置不合理
//   -E_NO_MEM: 物理内存不够 分配一个新的页，或者一个新的 页表页。
static int
sys_page_alloc(envid_t envid, void *va, int perm)
{
	// 提示：这个函数 相当于kern/pmap.c中的page_alloc()和page_insert()的封装
	//   你写的代码必须检查参数的正确性
	//   如果page_insert()失败了，记得 释放你所分配的物理页。

	// LAB 4: Your code here.
	// panic("sys_page_alloc not implemented");
	struct Env *e;
    if (envid2env(envid, &e, 1) < 0){
		return -E_BAD_ENV;
	}

    int valid_perm = (PTE_U|PTE_P); //检查perm是否是合理的
    if (va >= (void *)UTOP || (perm & valid_perm) != valid_perm) {
        return -E_INVAL;
    }

    struct PageInfo *p = page_alloc(1);//分配一个物理页
    if (!p){
		return -E_NO_MEM;
	}

    int ret = page_insert(e->env_pgdir, p, va, perm);//映射
    if (ret) { //映射失败需要释放物理页
        page_free(p);
    }
    return ret;
}

// 把id为srcenvid进程的虚拟地址为srcva对应的物理页，也映射到
//   id为dstenvid进程的虚拟地址为dstva对应的物理页上。
// 权限为perm。perm的格式和sys_page_alloc要求的相同，但它
// 还需要保证 不能对一个只读的物理页 有写权限。
// 
// 如果成功，返回0；如果失败，返回<0。错误有：
//   -E_BAD_ENV: srcenvid或dstenvid的进程不存在，或者调用者 没有权限更改它们
//   -E_INVAL: srcva>=UTOP或者srcva不是pageSize的整数倍；
// 				dstva>=UTOP或者dstva不是pageSize的整数倍；
//   -E_INVAL: srcenvid的进程中 还没映射srcva上的地址。
//   -E_INVAL: perm不符要求(参考 sys_page_alloc的perm要求)
//   -E_INVAL: perm&PTE_W==PTE_W，但srcva只有只读权限
//   -E_NO_MEM: 内存不够分配页表页。
static int
sys_page_map(envid_t srcenvid, void *srcva,
	     envid_t dstenvid, void *dstva, int perm)
{
	// 提示：这个函数是kern/pmap.c的page_lookup()和page_insert()的封装
	//   同样地，你需要检查参数的正确性
	//   使用page_lookcup()的第3个参数来检查物理页的当前权限。

	// LAB 4: Your code here.
	// panic("sys_page_map not implemented");
	struct Env *srcenv, *dstenv;
    if (envid2env(srcenvid, &srcenv, 1) || envid2env(dstenvid, &dstenv, 1)) {
        return -E_BAD_ENV;
    }

    if (srcva >= (void *)UTOP || dstva >= (void *)UTOP || PGOFF(srcva)!=0 || PGOFF(dstva)!=0) {
		//PGOFF(va)!=0说明va并不是页对齐的
        return -E_INVAL;
    }

    pte_t *pte;
    struct PageInfo *p = page_lookup(srcenv->env_pgdir, srcva, &pte);
    if (!p){
		return -E_INVAL;
	} 

    int valid_perm = (PTE_U|PTE_P);
    if ((perm&valid_perm) != valid_perm){
		return -E_INVAL;
	} 

    if ((perm & PTE_W) && !(*pte & PTE_W)){
		return -E_INVAL;
	}

    int ret = page_insert(dstenv->env_pgdir, p, dstva, perm);
    return ret;
}

// 取消envid对应的进程在虚拟地址va上的映射。
// 如果va并没有映射一个物理页，直接返回成功。
// 
// 如果成功，返回0；如果失败，返回<0。错误有：
//   -E_BAD_ENV: envid对应的进程不存在，或者调用者没有修改权限
//   -E_INVAL: va>=UTOP，或者va不是页对齐的
static int
sys_page_unmap(envid_t envid, void *va)
{
	// 提示：这个函数是page_remove()的封装

	// LAB 4: Your code here.
	// panic("sys_page_unmap not implemented");
	struct Env *e;
    if (envid2env(envid, &e, 1)){
		return -E_BAD_ENV;
	}

    if (va >= (void *)UTOP || PGOFF(va)!=0){
		return -E_INVAL;
	}

    page_remove(e->env_pgdir, va);
    return 0;
}

// 尝试把一个值'value'发送给 目标进程'envid'
// 如果 srcva<UTOP，那么 srcva 映射到的物理页 也需要发送过去，
// 这样 接受者 就会共享这个 物理页。
// 
// 如果 目标的接收进程 不在阻塞状态中(并在 等待IPC)，那么 返回-E_IPC_NOT_RECV
// 
// 发送 也可能因为 别的原因失败。错误类型 会列在下面。
// 
// 如果 发送成功，接受数据的进程 的ipc的各个属性 会更新成如下：
//   env_ipc_recving设置为0，表示 不再接受 数据。
//   env_ipc_from设置为 发送数据的 进程的envid
//   env_ipc_value设置为 要发送的数据value值
//   env_ipc_perm设置为 perm(如果 有需要 映射的虚拟页, 即dstva<UTOP)
// 接受数据的进程 被设置为RUNNABLE，然后在sys_ipc_recv调用中返回0
// (提示：sys_ipc_recv最后真的会 返回吗?)
// 
// 如果发送这 想发送一个物理页，但 接受者 不想要，那么不需要 把 物理页 映射到 dstva上，没有错误发生。
// 
// 如果成功返回0，失败返回<0。错误有：
//   -E_BAD_ENV：envid的进程不存在（注意：不需要检查 是否有 修改env的权限）
//   -E_IPC_NOT_RECV：envid的进程当前并不 阻塞等待接受消息，或者 已经 有别的进程 已经 向它发送过消息了
//   -E_INVAL：srcva<UTOP，但srcva并不是 页对齐的
//   -E_INVAL：srcva<UTOP，但perm不是合法的(合法的perm 请看sys_page_alloc)
//   -E_INVAL：srcva<UTOP，但srcv在发送进程 中 还没映射到物理页上
//   -E_INVAL：如果perm中有PTE_W的权限，但srcva只有 只读的权限。
//   -E_NO_MEM if there's not enough memory to map srcva in envid's
//		address space.(说错了??? srcva上面不是映射过物理页了吗???)
// 
static int
sys_ipc_try_send(envid_t envid, uint32_t value, void *srcva, unsigned perm)
{
	// LAB 4: Your code here.
	// panic("sys_ipc_try_send not implemented");

	struct Env *e; 
    if (envid2env(envid, &e, 0)){ //checkperm设置0，不需要 检查权限
		return -E_BAD_ENV;
	}

    if (!e->env_ipc_recving){ //检查目标进程是否在阻塞接受中
		return -E_IPC_NOT_RECV;
	}

    if (srcva < (void *) UTOP) { //检查 能否共享物理页
        if(PGOFF(srcva)){ // 没有页对齐
			return -E_INVAL;
		}

        pte_t *pte;
        struct PageInfo *p = page_lookup(curenv->env_pgdir, srcva, &pte);
        if (!p){
			return -E_INVAL;
		}

		int valid_perm = (PTE_U|PTE_P); //检查perm是否是合理的
		if ((perm & valid_perm) != valid_perm) {
			return -E_INVAL;
		}
		// 这个地方可能会 有点问题
        // if ((*pte & perm) != perm){
		// 	cprintf("Bug3: *pte: %d\n", *pte);
		// 	// return -E_INVAL;
		// }

        if ((perm & PTE_W) && !(*pte & PTE_W)){//如果perm中有PTE_W的权限，但srcva只有 只读的权限。
			return -E_INVAL;
		}

        if (e->env_ipc_dstva < (void *)UTOP) {//共享物理页
            int ret = page_insert(e->env_pgdir, p, e->env_ipc_dstva, perm);
            if (ret){
				return ret;
			}
            e->env_ipc_perm = perm;
        }//否则 也不需要报错   
    }   

    e->env_ipc_recving = 0;
    e->env_ipc_from = curenv->env_id;
    e->env_ipc_value = value;
    e->env_status = ENV_RUNNABLE;
    e->env_tf.tf_regs.reg_eax = 0;
    return 0;
}

// 阻塞，直到一个值被 接受到。
// 通过设置env_ipc_recving=1 和 env_ipc_dstva，来告诉别的 进程，你希望接受值。
// 标记 自己为 不可运行，然后 释放CPU的使用权。
// 
// 如果dstva<UTOP，那么 该进程希望接受 一个物理页的数据。
// dstva是 该发送的物理页 应该映射到的位置
// 
// 如果成功，返回0；失败返回<0。错误有：
//   -E_INVAL: 如果dstva<UTOP且dstva不是页对齐的。
static int
sys_ipc_recv(void *dstva)
{
	// LAB 4: Your code here.
	// panic("sys_ipc_recv not implemented");

	// 如果作为参数的虚拟地址在 UTOP 之上，只需要忽略，而不是报错退出。
	// 因为这种情况是说明接收者只需要接收值，而不需要共享页面（联想在 lib/ipc.c 中的处理）

	if ((dstva < (void *)UTOP) && PGOFF(dstva)){//不是page-aligned的
		return -E_INVAL;
	}        

    curenv->env_ipc_recving = 1;
    curenv->env_status = ENV_NOT_RUNNABLE;
    curenv->env_ipc_dstva = dstva;
    sys_yield();
    return 0;
}

// Dispatches to the correct kernel function, passing the arguments.
int32_t
syscall(uint32_t syscallno, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
	// Call the function corresponding to the 'syscallno' parameter.
	// Return any appropriate return value.
	// LAB 3: Your code here.

	//    panic("syscall not implemented");

    switch (syscallno) {
        case (SYS_cputs):
            sys_cputs((const char *)a1, a2);
            return 0;
        case (SYS_cgetc):
            return sys_cgetc();
        case (SYS_getenvid):
            return sys_getenvid();
        case (SYS_env_destroy):
            return sys_env_destroy(a1);
		case (SYS_yield):
            sys_yield();
            return 0;
		case (SYS_exofork):
			return sys_exofork();
		case (SYS_env_set_status):
			return sys_env_set_status(a1, a2);
		case (SYS_page_alloc):
			return sys_page_alloc(a1, (void *)a2, a3);
		case (SYS_page_map):
			return sys_page_map(a1, (void*)a2, a3, (void*)a4, a5);
		case (SYS_page_unmap):
			return sys_page_unmap(a1, (void *)a2);
		case (SYS_env_set_pgfault_upcall):
			return sys_env_set_pgfault_upcall(a1, (void *)a2);
		case (SYS_ipc_try_send):
        	return sys_ipc_try_send(a1, a2, (void *)a3, a4);
    	case (SYS_ipc_recv):
        	return sys_ipc_recv((void *)a1);
	    case (SYS_env_set_trapframe):
            return sys_env_set_trapframe(a1, (struct Trapframe *)a2);
        default:
            return -E_INVAL;
    }
}

