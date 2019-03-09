// implement fork from user space

#include <inc/string.h>
#include <inc/lib.h>

// PTE_COW marks copy-on-write page table entries.
// It is one of the bits explicitly allocated to user processes (PTE_AVAIL).
#define PTE_COW		0x800

// 
// 常规的page fault handler
// 如果出错的页 是copy-on-write的，那么 映射我们自己私有的可写的版本。
// 
static void
pgfault(struct UTrapframe *utf)
{
    void *addr = (void *) utf->utf_fault_va;
    uint32_t err = utf->utf_err;
    int r;

	// 检查出错的过程是否是：(1)写入 (2)到一个copy-on-write的 物理页
	// 如果不是，那么panic
	// 提示：
	//   使用uvpt中的page table的映射(inc/memlayout.h)

    // LAB 4: Your code here.
    if ((err & FEC_WR)==0 || (uvpt[PGNUM(addr)] & PTE_COW)==0) {
        panic("pgfault: invalid user trap frame");
    }

	// 分配一个新的物理页，并把它映射到一个 临时的位置(PFTEMP)
	// 把数据从旧的页中复制到 新的页中，然后 映射一下新的页
	// 
	// 提示：
	//   你需要使用3个系统调用

    // LAB 4: Your code here.
    // panic("pgfault not implemented");
    envid_t envid = sys_getenvid();
    if ((r = sys_page_alloc(envid, (void *)PFTEMP, PTE_P | PTE_W | PTE_U)) < 0)
        panic("pgfault: page allocation failed %e", r);

    addr = ROUNDDOWN(addr, PGSIZE);
    memmove(PFTEMP, addr, PGSIZE);// 把数据从旧的页中复制到 新的页中
    if ((r = sys_page_unmap(envid, addr)) < 0)
        panic("pgfault: page unmap failed (%e)", r);
    if ((r = sys_page_map(envid, PFTEMP, envid, addr, PTE_P | PTE_W |PTE_U)) < 0)
        panic("pgfault: page map failed (%e)", r);
    if ((r = sys_page_unmap(envid, PFTEMP)) < 0)
        panic("pgfault: page unmap failed (%e)", r);
}

// 
// 把当前进程的 从pn*PGSIZE开始的 虚拟页面的 映射 复制到 envid进程对应的位置。
// 如果 物理页面是可写的 或者 是copy-on-write的，那么新的映射要设置为copy-on-write的，
// 且当前进程 的页面 也要设置为copy-on-write的。exercise：为什么 又要再mark一次? 
// 应该是 为了设置成只读吧?
// 
// 成功返回0，失败返回<0
// 失败时，可以抛出异常panic
// 
static int
duppage(envid_t envid, unsigned pn)
{
	int r;

    // LAB 4: Your code here.
    // panic("duppage not implemented");

    void *addr = (void *)(pn * PGSIZE);
	if (uvpt[pn] & PTE_SHARE) {
		// cprintf("dup share page :%d\n", pn);
		if ((r = sys_page_map(0, addr, envid, addr, PTE_SYSCALL)) < 0)
			panic("duppage sys_page_map:%e", r);
	} else if (uvpt[pn] & (PTE_W|PTE_COW)) {
		if ((r = sys_page_map(0, addr, envid, addr, PTE_COW|PTE_U|PTE_P)) < 0)
			panic("sys_page_map COW:%e", r);

		if ((r = sys_page_map(0, addr, 0, addr, PTE_COW|PTE_U|PTE_P)) < 0)
			panic("sys_page_map COW:%e", r);
	} else {
		if ((r = sys_page_map(0, addr, envid, addr, PTE_U|PTE_P)) < 0)
			panic("sys_page_map UP:%e", r);
	}
	return 0;
}

// 
// 用户级别的copy-on-write的fork()
// 设置page fault handler
// 产生一个child
// 把地址空间 和 page fault handler复制 给child
// 然后把child设为runnable，然后返回
// 
// 父进程返回 子进程的envid；子进程返回0；出错返回<0。
// 出错时抛出异常panic也是OK的。
// 
// 提示：
//   使用uvpd, uvpt, 和 duppage
//   记得修改子进程的thisenv
//   user exception stack不能被设置为copy-on-write，
//   我们必须为他在物理内存上 分配一个user exception stack的页
//
envid_t
fork(void)
{
    // LAB 4: Your code here.
    // panic("fork not implemented");

    set_pgfault_handler(pgfault);// 设置page fault handler
    envid_t e_id = sys_exofork();// 产生一个child
    if (e_id < 0){
		panic("fork: %e", e_id);
	}
    if (e_id == 0) {
        // child：修改子进程的thisenv
        thisenv = &envs[ENVX(sys_getenvid())];
        return 0;
    }

    // parent
    // extern unsigned char end[];
    // for ((uint8_t *) addr = UTEXT; addr < end; addr += PGSIZE) 估计是lab5在这个地方有bug
    for (uintptr_t addr = UTEXT; addr < USTACKTOP; addr += PGSIZE) {
        if ( (uvpd[PDX(addr)] & PTE_P) && (uvpt[PGNUM(addr)] & PTE_P) ) {
            // dup page to child
            duppage(e_id, PGNUM(addr));
        }
    }
    // alloc page for exception stack
    int r = sys_page_alloc(e_id, (void *)(UXSTACKTOP-PGSIZE), PTE_U | PTE_W | PTE_P);
    if (r < 0) panic("fork: %e",r);

    // DO NOT FORGET
    extern void _pgfault_upcall();
    r = sys_env_set_pgfault_upcall(e_id, _pgfault_upcall);
    if (r < 0){
		panic("fork: set upcall for child fail, %e", r);
	}

    // mark the child environment runnable
    if ((r = sys_env_set_status(e_id, ENV_RUNNABLE)) < 0)
        panic("sys_env_set_status: %e", r);

    return e_id;
}

// Challenge!
int
sfork(void)
{
	panic("sfork not implemented");
	return -E_INVAL;
}
