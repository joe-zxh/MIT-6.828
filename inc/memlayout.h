#ifndef JOS_INC_MEMLAYOUT_H
#define JOS_INC_MEMLAYOUT_H

#ifndef __ASSEMBLER__
#include <inc/types.h>
#include <inc/mmu.h>
#endif /* not __ASSEMBLER__ */

/*
 * This file contains definitions for memory management in our OS,
 * which are relevant to both the kernel and user-mode software.
 */

// Global descriptor numbers
#define GD_KT     0x08     // kernel text 0000 1000
#define GD_KD     0x10     // kernel data 0001 0000
#define GD_UT     0x18     // user text   0001 1000
#define GD_UD     0x20     // user data   0010 0000
#define GD_TSS0   0x28     // Task segment selector for CPU 0   0010 1000

/*
 * Virtual memory map:                                Permissions
 *                                                    kernel/user
 *
 *    4 Gig -------->  +------------------------------+
 *                     |                              | RW/--
 *                     ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *                     :              .               :
 *                     :              .               :
 *                     :              .               :
 *                     |~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~| RW/--
 *                     |                              | RW/--
 *                     |   Remapped Physical Memory   | RW/--
 *                     |                              | RW/--
 *    KERNBASE, ---->  +------------------------------+ 0xf0000000      --+
 *    KSTACKTOP        |     CPU0's Kernel Stack      | RW/--  KSTKSIZE   |
 *                     | - - - - - - - - - - - - - - -|                   |
 *                     |      Invalid Memory (*)      | --/--  KSTKGAP    |
 *                     +------------------------------+                   |
 *                     |     CPU1's Kernel Stack      | RW/--  KSTKSIZE   |
 *                     | - - - - - - - - - - - - - - -|                 PTSIZE
 *                     |      Invalid Memory (*)      | --/--  KSTKGAP    |
 *                     +------------------------------+                   |
 *                     :              .               :                   |
 *                     :              .               :                   |
 *    MMIOLIM ------>  +------------------------------+ 0xefc00000      --+
 *                     |       Memory-mapped I/O      | RW/--  PTSIZE
 * ULIM, MMIOBASE -->  +------------------------------+ 0xef800000
 *                     |  Cur. Page Table (User R-)   | R-/R-  PTSIZE
 *    UVPT      ---->  +------------------------------+ 0xef400000
 *                     |          RO PAGES            | R-/R-  PTSIZE
 *    UPAGES    ---->  +------------------------------+ 0xef000000
 *                     |           RO ENVS            | R-/R-  PTSIZE
 * UTOP,UENVS ------>  +------------------------------+ 0xeec00000
 * UXSTACKTOP -/       |     User Exception Stack     | RW/RW  PGSIZE
 *                     +------------------------------+ 0xeebff000
 *                     |       Empty Memory (*)       | --/--  PGSIZE
 *    USTACKTOP  --->  +------------------------------+ 0xeebfe000
 *                     |      Normal User Stack       | RW/RW  PGSIZE
 *                     +------------------------------+ 0xeebfd000
 *                     |                              |
 *                     |                              |
 *                     ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *                     .                              .
 *                     .                              .
 *                     .                              .
 *                     |~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~|
 *                     |     Program Data & Heap      |
 *    UTEXT -------->  +------------------------------+ 0x00800000
 *    PFTEMP ------->  |       Empty Memory (*)       |        PTSIZE
 *                     |                              |
 *    UTEMP -------->  +------------------------------+ 0x00400000      --+
 *                     |       Empty Memory (*)       |                   |
 *                     | - - - - - - - - - - - - - - -|                   |
 *                     |  User STAB Data (optional)   |                 PTSIZE
 *    USTABDATA ---->  +------------------------------+ 0x00200000        |
 *                     |       Empty Memory (*)       |                   |
 *    0 ------------>  +------------------------------+                 --+
 *
 * (*) Note: The kernel ensures that "Invalid Memory" is *never* mapped.
 *     "Empty Memory" is normally unmapped, but user programs may map pages
 *     there if desired.  JOS user programs map pages temporarily at UTEMP.
 */


// All physical memory mapped at this address
#define	KERNBASE	0xF0000000

// At IOPHYSMEM (640K) there is a 384K hole for I/O.  From the kernel,
// IOPHYSMEM can be addressed at KERNBASE + IOPHYSMEM.  The hole ends
// at physical address EXTPHYSMEM.
#define IOPHYSMEM	0x0A0000 //物理地址：https://www.cnblogs.com/fatsheep9146/p/5078102.html
#define EXTPHYSMEM	0x100000

// Kernel stack.
#define KSTACKTOP	KERNBASE //0xF0000000
#define KSTKSIZE	(8*PGSIZE)   		// size of a kernel stack
#define KSTKGAP		(8*PGSIZE)   		// size of a kernel stack guard

// Memory-mapped IO.
#define MMIOLIM		(KSTACKTOP - PTSIZE) //MM-IO的终止位置：0xf0000000-0x400000 = 0xefc00000
#define MMIOBASE	(MMIOLIM - PTSIZE) //MM-IO的起始位置： 0xef800000

#define ULIM		(MMIOBASE) //0xef800000: user space最高的终止位置，也就是MM-IO的起始位置

/*
 * User read-only mappings! Anything below here til UTOP are readonly to user.
 * They are global pages mapped in at env allocation time.
 */

// User read-only virtual page table (see 'uvpt' below)
#define UVPT		(ULIM - PTSIZE)
// Read-only copies of the Page structures
#define UPAGES		(UVPT - PTSIZE)
// Read-only copies of the global env structures
#define UENVS		(UPAGES - PTSIZE)

/*
 * Top of user VM. User can manipulate VA from UTOP-1 and down!
 */

// Top of user-accessible VM
#define UTOP		UENVS
// Top of one-page user exception stack
#define UXSTACKTOP	UTOP
// Next page left invalid to guard against exception stack overflow; then:
// Top of normal user stack
#define USTACKTOP	(UTOP - 2*PGSIZE)

// Where user programs generally begin
#define UTEXT		(2*PTSIZE)

// Used for temporary page mappings.  Typed 'void*' for convenience
#define UTEMP		((void*) PTSIZE)
// Used for temporary page mappings for the user page-fault handler
// (should not conflict with other temporary page mappings)
#define PFTEMP		(UTEMP + PTSIZE - PGSIZE)
// The location of the user-level STABS data structure
#define USTABDATA	(PTSIZE / 2)

#ifndef __ASSEMBLER__

typedef uint32_t pte_t;
typedef uint32_t pde_t;

#if JOS_USER
/*
 * The page directory entry corresponding to the virtual address range
 * [UVPT, UVPT + PTSIZE) points to the page directory itself.  Thus, the page
 * directory is treated as a page table as well as a page directory.
 *
 * One result of treating the page directory as a page table is that all PTEs
 * can be accessed through a "virtual page table" at virtual address UVPT (to
 * which uvpt is set in lib/entry.S).  The PTE for page number N is stored in
 * uvpt[N].  (It's worth drawing a diagram of this!)
 *
 * A second consequence is that the contents of the current page directory
 * will always be available at virtual address (UVPT + (UVPT >> PGSHIFT)), to
 * which uvpd is set in lib/entry.S.
 */
extern volatile pte_t uvpt[];     // VA of "virtual page table"
extern volatile pde_t uvpd[];     // VA of current page directory
#endif

/*
 * 页描述符数据结构，映射在虚拟内存为UPAGES的位置。
 * 对于内核，这部分是可读写的；对于用户，这部分是只读的。
 *
 * 每个PageInfo结构 存储了一个 物理页的元信息
 * 它不是实际物理页，但和 物理页有一一对应的关系。
 * 你可以通过kern/pmap.h中的page2pa()函数来 通过PageInfo得到 对应的物理地址
 */
struct PageInfo { //就是物理页
	// Next page on the free list.
	struct PageInfo *pp_link;

	// pp_ref是指向这个page的指针(通常是页表中的entry)的个数
	// 对于物理页的分配，要使用page_alloc
	// 在OS启动时，使用boot_alloc分配的物理页，还没有 有效的pp_ref的域的。

	uint16_t pp_ref;
};

#endif /* !__ASSEMBLER__ */
#endif /* !JOS_INC_MEMLAYOUT_H */
