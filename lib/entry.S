#include <inc/mmu.h>
#include <inc/memlayout.h>

.data
// Define the global symbols 'envs', 'pages', 'uvpt', and 'uvpd'
// so that they can be used in C as if they were ordinary global arrays.
// 设置一些全局变量
	.globl envs
	.set envs, UENVS
	.globl pages
	.set pages, UPAGES
	.globl uvpt
	.set uvpt, UVPT
	.globl uvpd
	.set uvpd, (UVPT+(UVPT>>12)*4)


// Entrypoint - this is where the kernel (or our parent environment)
// starts us running when we are initially loaded into a new environment.
// 用户程序的入口
.text
.globl _start
_start:
	// 检查esp是否等于USTACKTOP，如果不是，说明 有参数放在栈上面，直接跳转到libmain执行
	cmpl $USTACKTOP, %esp
	jne args_exist

	// 如果没有参数传递(这种情况是在lab3中，通过kernel加载用户程序的时候会出现)，那么 压入2个0。
	// If not, push dummy argc/argv arguments.
	// This happens when we are loaded by the kernel,
	// because the kernel does not know about passing arguments.
	pushl $0
	pushl $0

args_exist:
	call libmain
1:	jmp 1b

