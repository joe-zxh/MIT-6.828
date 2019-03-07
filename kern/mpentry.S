/* See COPYRIGHT for copyright information. */

#include <inc/mmu.h>
#include <inc/memlayout.h>

###################################################################
# entry point for APs
###################################################################

# 
# boot CPU发起一个STARTUP IPI来启动一个 non-boot CPU("AP")
# Multi-Processor Specification的B.4.2章节 说AP会从 CS:IP=XY00:0000开始执行
# 也就是说 实模式的寻址的起始为XY000,16进制的最后3位为0,所以它是对齐到4096的(2^(3*4))
#
# Because this code sets DS to zero, it must run from an address in
# the low 2^16 bytes of physical memory.
#
# init.c中的boot_aps()把这个代码复制到MPENTRY_PADDR的位置。
# 然后，对某个AP，它会在mpentry_kstack中保存 per-core stack的pre-allocated的地址
# 发送STARTUP IPI，然后等待 mpentry.S 调用mp_main来 通知AP已启动
# 
# 这份代码和 boot/boot.S很类似，除了：
# 	- 它没有开启A20
# 	- 它使用MPBOOTPHYS来计算 它的符号的绝对地址，而不是依靠 链接器来 填充它们

#define RELOC(x) ((x) - KERNBASE)
#define MPBOOTPHYS(s) ((s) - mpentry_start + MPENTRY_PADDR)
# MPENTRY_PADDR是0x7000

.set PROT_MODE_CSEG, 0x8	# kernel code segment selector
.set PROT_MODE_DSEG, 0x10	# kernel data segment selector

.code16           
.globl mpentry_start
mpentry_start:
	cli # 关闭中断           

	# 清空重要的 数据段寄存器
	xorw    %ax, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss

	# 从实模式 转换 为 保护模式
  	# 转入保护模式，这里需要指定一个临时的GDT，来翻译逻辑地址。(开启了一个 "傀儡"的分段机制，因为段选择子实际上索引出来的基地址就是0)
  	# 这里使用的GDT通过gdtdesc段定义，它翻译得到的物理地址和虚拟地址相同，所以转换过程中内存映射不会改变
	lgdt    MPBOOTPHYS(gdtdesc)
	movl    %cr0, %eax
	orl     $CR0_PE, %eax
	movl    %eax, %cr0

	ljmpl   $(PROT_MODE_CSEG), $(MPBOOTPHYS(start32)) # 跳转到下一行

.code32
start32:
	movw    $(PROT_MODE_DSEG), %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    $0, %ax
	movw    %ax, %fs
	movw    %ax, %gs

	# Set up initial page table. We cannot use kern_pgdir yet because
	# we are still running at a low EIP.
	movl    $(RELOC(entry_pgdir)), %eax
	movl    %eax, %cr3
	# Turn on paging.
	movl    %cr0, %eax
	orl     $(CR0_PE|CR0_PG|CR0_WP), %eax
	movl    %eax, %cr0

	# Switch to the per-cpu stack allocated in boot_aps() 设置栈的位置
	movl    mpentry_kstack, %esp
	movl    $0x0, %ebp       # nuke frame pointer

	# Call mp_main().  (Exercise for the reader: why the indirect call?)
	movl    $mp_main, %eax
	call    *%eax

	# If mp_main returns (it shouldn't), loop.
spin:
	jmp     spin

# Bootstrap GDT
.p2align 2					# force 4 byte alignment
gdt:
	SEG_NULL				# null seg
	SEG(STA_X|STA_R, 0x0, 0xffffffff)	# code seg
	SEG(STA_W, 0x0, 0xffffffff)		# data seg

gdtdesc:
	.word   0x17				# sizeof(gdt) - 1
	.long   MPBOOTPHYS(gdt)			# address gdt

.globl mpentry_end
mpentry_end:
	nop
