// User-level page fault handler support.
// Rather than register the C page fault handler directly with the
// kernel as the page fault handler, we register the assembly language
// wrapper in pfentry.S, which in turns calls the registered C
// function.

#include <inc/lib.h>


// Assembly language pgfault entrypoint defined in lib/pfentry.S.
extern void _pgfault_upcall(void);

// Pointer to currently installed C-language pgfault handler.
void (*_pgfault_handler)(struct UTrapframe *utf);



// 
// 设置page fault handler函数
// 如果现在还没有page fault handler函数，那么_pgfault_handler将会是0
// 我们第一次注册handler的时候，我们需要在UXSTACKTOP上分配一个exception stack
// 然后 在发生page fault时，告诉内核 去调用 汇编代码的_pgfault_upcall。
// 
void
set_pgfault_handler(void (*handler)(struct UTrapframe *utf))
{
	int r;

	if (_pgfault_handler == 0) {
		// First time through!
		// LAB 4: Your code here.
		// panic("set_pgfault_handler not implemented");
		envid_t e_id = sys_getenvid();
		if (sys_page_alloc(e_id, (void *)(UXSTACKTOP - PGSIZE), PTE_W|PTE_U|PTE_P)) {
			// 分配exception stack
            panic("set_pgfault_handler page_alloc failed");
        }   
        if (sys_env_set_pgfault_upcall(e_id, _pgfault_upcall)!=0) {
			// 注册_pgfault_upcall
            panic("set_pgfault_handler set_pgfault_upcall failed");
        }
	}

	// Save handler pointer for assembly to call.
	_pgfault_handler = handler;
}
