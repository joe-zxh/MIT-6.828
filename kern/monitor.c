// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/x86.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/trap.h>

#define CMDBUF_SIZE	80	// enough for one VGA text line


struct Command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

static struct Command commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
};

/***** Implementations of basic kernel monitor commands *****/

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(commands); i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
	extern char _start[], entry[], etext[], edata[], end[];

	cprintf("Special kernel symbols:\n");
	cprintf("  _start                  %08x (phys)\n", _start);
	cprintf("  entry  %08x (virt)  %08x (phys)\n", entry, entry - KERNBASE);
	cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
	cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
	cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
	cprintf("Kernel executable memory footprint: %dKB\n",
		ROUNDUP(end - entry, 1024) / 1024);
	return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
	// Your code here.
	cprintf("Stack backtrace:\n");

	uint32_t ebp = read_ebp();//如果用指针的话，read_ebp返回的指针就刚好是当前ebp的位置
	//这样后面就不需要疯狂地使用汇编代码了。可以参考：https://github.com/shishujuan/mit6.828-2017/blob/master/docs/lab1-exercize.md
	uint32_t eip;
	uint32_t args[5];
	struct Eipdebuginfo info;

	while(ebp!=0){
		asm volatile("movl 0x4(%1),%0" : "=r" (eip) : "r"(ebp));

		asm volatile("movl 0x8(%1),%0" : "=r" (args[0]) : "r"(ebp));
		asm volatile("movl 0xc(%1),%0" : "=r" (args[1]) : "r"(ebp));
		asm volatile("movl 0x10(%1),%0" : "=r" (args[2]) : "r"(ebp));
		asm volatile("movl 0x14(%1),%0" : "=r" (args[3]) : "r"(ebp));
		asm volatile("movl 0x18(%1),%0" : "=r" (args[4]) : "r"(ebp));

		cprintf("  ebp %x  eip %x  args %08x %08x %08x %08x %08x\n",ebp,eip,args[0],args[1],args[2],args[3],args[4]);
		
		int ret = debuginfo_eip(eip, &info);	
		//cprintf("返回值：%d\n", ret);
		
		//cprintf("%d\n", info.eip_line);
		cprintf("         %s:%d:",info.eip_file,info.eip_line);
		cprintf(" %.*s", info.eip_fn_namelen, info.eip_fn_name);
		cprintf("+%d\n", eip-info.eip_fn_addr);

		asm volatile("movl (%1),%0" : "=r" (ebp) : "r"(ebp));//上一个函数的ebp
	}

	return 0;
}


/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char *buf, struct Trapframe *tf)
{
	int argc;
	char *argv[MAXARGS];
	int i;

	// Parse the command buffer into whitespace-separated arguments
	argc = 0;
	argv[argc] = 0;
	while (1) {
		// gobble whitespace
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// save and scan past next arg
		if (argc == MAXARGS-1) {
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// Lookup and invoke the command
	if (argc == 0)
		return 0;
	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void
monitor(struct Trapframe *tf)
{
	char *buf;

	cprintf("Welcome to the JOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");

	if (tf != NULL)
		print_trapframe(tf);

	while (1) {
		buf = readline("K> ");
		if (buf != NULL)
			if (runcmd(buf, tf) < 0)
				break;
	}
}
