#include <inc/mmu.h>

# Start the CPU: switch to 32-bit protected mode, jump into C.
# The BIOS loads this code from the first sector of the hard disk into
# memory at physical address 0x7c00 and starts executing in real mode
# with %cs=0 %ip=7c00.

.set PROT_MODE_CSEG, 0x8         # kernel code segment selector
.set PROT_MODE_DSEG, 0x10        # kernel data segment selector
.set CR0_PE_ON,      0x1         # protected mode enable flag

.globl start
start:
  .code16                     # Assemble for 16-bit mode
  cli                         # Disable interrupts
  cld                         # String operations increment 使得DF方向标志位 设为0

  # Set up the important data segment registers (DS, ES, SS).
  xorw    %ax,%ax             # Segment number zero
  movw    %ax,%ds             # -> Data Segment
  movw    %ax,%es             # -> Extra Segment
  movw    %ax,%ss             # -> Stack Segment

  # Enable A20
  #   For backwards compatibility with the earliest PCs, physical
  #   address line 20 is tied low, so that addresses higher than
  #   1MB wrap around to zero by default.  This code undoes this.
  # 怎么激活呢，由于历史原因A20地址位由键盘控制器芯片8042管理。所以要给8042发命令激活A20
  # 8042有两个IO端口：0x60和0x64， 激活流程位： 发送0xd1命令到0x64端口 --> 发送0xdf到0x60，done！
seta20.1:
  inb     $0x64,%al               # Wait for not busy
  testb   $0x2,%al  # 等待，直到端口0x64空闲（第2位为0）
  jnz     seta20.1

  movb    $0xd1,%al               # 0xd1 -> port 0x64
  outb    %al,$0x64               # 把0xd1传给端口0x64

seta20.2:
  inb     $0x64,%al               # Wait for not busy
  testb   $0x2,%al # 等待，直到端口0x64空闲（第2位为0）这里是不是一个bug???难道不是0x60???
  jnz     seta20.2

  movb    $0xdf,%al               # 0xdf -> port 0x60
  outb    %al,$0x60

  # Switch from real to protected mode, using a bootstrap GDT
  # and segment translation that makes virtual addresses 
  # identical to their physical addresses, so that the 
  # effective memory map does not change during the switch.
  # 从实模式 转换 为 保护模式
  # 转入保护模式，这里需要指定一个临时的GDT，来翻译逻辑地址。
  # 这里使用的GDT通过gdtdesc段定义，它翻译得到的物理地址和虚拟地址相同，所以转换过程中内存映射不会改变
  lgdt    gdtdesc
  movl    %cr0, %eax
  orl     $CR0_PE_ON, %eax # CR0_PE_ON是1 相当于将%cr0的最低位 设为1. 也就是保护模式
  movl    %eax, %cr0
  
  # Jump to next instruction, but in 32-bit code segment.
  # Switches processor into 32-bit mode.
  ljmp    $PROT_MODE_CSEG, $protcseg
  
# PROT_MODE_CSEG的值为0x8。根据段选择子的格式定义，0x8就翻译成：
# INDEX　　　　　　　　   TI     CPL
# 0000 0000 0000 1      00      0
#　INDEX代表GDT中的索引，TI代表使用GDTR中的GDT， CPL代表处于特权级。
# 索引出来，也就是 code seg，它的基地址就是０.
#　加上偏移量protcseg，就是跳到下一行代码

  .code32                     # Assemble for 32-bit mode
protcseg:
  # Set up the protected-mode data segment registers
  movw    $PROT_MODE_DSEG, %ax    # Our data segment selector
  movw    %ax, %ds                # -> DS: Data Segment
  movw    %ax, %es                # -> ES: Extra Segment
  movw    %ax, %fs                # -> FS
  movw    %ax, %gs                # -> GS
  movw    %ax, %ss                # -> SS: Stack Segment
  
  # Set up the stack pointer and call into C.
  movl    $start, %esp
  call bootmain # 转去执行bootmain了

  # If bootmain returns (it shouldn't), loop.
spin:
  jmp spin

# Bootstrap GDT
.p2align 2                                # force 4 byte alignment
gdt:
  SEG_NULL				# null seg
  SEG(STA_X|STA_R, 0x0, 0xffffffff)	# code seg
  SEG(STA_W, 0x0, 0xffffffff)	        # data seg

# SEG(type, base, lim)
# type表示访问权限。代码段是 可执行(STA_X) 和 可读的(STA_R)    数据段是可写的(STA_W)
# base表示起始地址
# limit表示段的大小接线。
# 因为xv6里面没有分段机制，数据 和 代码都写在一起的
# 所以 数据段和代码段的起始地址都是0x0，大小都是0xffffffff=4GB。

# GDT的一个表项是8字节(https://www.cnblogs.com/fatsheep9146/p/5115086.html的gdt_entry_struct)，
# 所以一共24字节。处于历史原因 .word记录的是sizeof(gdt)-1
# 也就是23(0x17)字节

gdtdesc:
  .word   0x17                            # sizeof(gdt) - 1
  .long   gdt                             # address gdt

