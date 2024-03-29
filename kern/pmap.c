/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/pmap.h>
#include <kern/kclock.h>
#include <kern/env.h>
#include <kern/cpu.h>

// These variables are set by i386_detect_memory()
size_t npages;			// Amount of physical memory (in pages)
static size_t npages_basemem;	// Amount of base memory (in pages)

// These variables are set in mem_init()
pde_t *kern_pgdir;		// Kernel's initial page directory 就是 页目录的位置
struct PageInfo *pages;		// Physical page state array
static struct PageInfo *page_free_list;	// Free list of physical pages


// --------------------------------------------------------------
// Detect machine's physical memory setup.
// --------------------------------------------------------------

static int
nvram_read(int r)
{
	return mc146818_read(r) | (mc146818_read(r + 1) << 8);
}

static void
i386_detect_memory(void)
{
	//jos把整个物理内存空间划分成3个部分：
	//从0x00000~0xA0000，这部分也叫basemem，是可用的(640KB)
	//0xA0000~0x100000，这部分叫做IO hole，是不可用的，主要被用来分配给外部设备了。
	//0x100000~0x，这部分叫做extmem，是可用的，这是最重要的内存区域。
	size_t basemem, extmem, ext16mem, totalmem;

	// Use CMOS calls to measure available base & extended memory.
	// (CMOS calls return results in kilobytes.)
	basemem = nvram_read(NVRAM_BASELO);
	extmem = nvram_read(NVRAM_EXTLO);
	ext16mem = nvram_read(NVRAM_EXT16LO) * 64;

	// Calculate the number of physical pages available in both base
	// and extended memory.
	if (ext16mem)
		totalmem = 16 * 1024 + ext16mem;
	else if (extmem)
		totalmem = 1 * 1024 + extmem;
	else
		totalmem = basemem;

	npages = totalmem / (PGSIZE / 1024);
	npages_basemem = basemem / (PGSIZE / 1024);

	cprintf("Physical memory: %uK available, base = %uK, extended = %uK\n",
		totalmem, basemem, totalmem - basemem);
}


// --------------------------------------------------------------
// Set up memory mappings above UTOP.
// --------------------------------------------------------------

static void mem_init_mp(void);
static void boot_map_region(pde_t *pgdir, uintptr_t va, size_t size, physaddr_t pa, int perm);
static void check_page_free_list(bool only_low_memory);
static void check_page_alloc(void);
static void check_kern_pgdir(void);
static physaddr_t check_va2pa(pde_t *pgdir, uintptr_t va);
static void check_page(void);
static void check_page_installed_pgdir(void);

// 只有在JOS开始设置它的虚拟内存系统时，这个简单的物理内存分配器 才被使用。
// page_alloc()才是真正的分配器
//
// 如果n>0，那么分配连续的n字节的物理内存，但不需要初始化内存。
// 返回的是虚拟地址。
// 
// 如果n==0，那么返回下一个空的page的地址，而不需要分配任何内存。
//
// 如果没有足够多的内存，那么boot_alloc要发起异常。
// 这个函数只能在初始化时使用，且在page_free链表被设置前使用。

// 这个函数是从kernal base 的kernel bss之后开始分配的，也就是 会放到物理内存0x10000之后的。即exmem的位置
// 尽管页表还没设置完成，但是entrypgdir.c里面定义的4MB的虚拟地址转换物理地址的 部分 应该是够用的。
static void *
boot_alloc(uint32_t n) //这个boot_alloc只是在一开始的时候被调用了一次，所以是，可以这么直接分配内存的。
{
	static char *nextfree;	// 下一个未分配的内存的虚拟地址。
	char *result;

	// 如果是第一次，那么初始化nextfree
	// end是一个由链接器产生的magic标号，它指向kernel bss 段的末尾:
	// 也就是第一个空的虚拟地址。
	if (!nextfree) {
		extern char end[];
		nextfree = ROUNDUP((char *) end, PGSIZE);//对齐到PGSIZE
	}

	// 分配一个能够容纳n字节的chunk，然后更新nextfree，
	// 并且需要保证nextfree是对齐到PGSIZE的倍数
	//
	// LAB 2: Your code here.
	result = nextfree;
	nextfree = ROUNDUP(nextfree+n, PGSIZE);
	if((uint32_t)nextfree-KERNBASE>(npages*PGSIZE)){
		panic("Out of memory!\n");
	}
	return result;
}

// 设置2级页表：
//    kern_pgdir is its linear (virtual) address of the root
//
// 这个函数只设置了内核的地址空间，也就是 虚拟地址>=UTOP的地址。
// 用户的地址空间 将会在 后面再设置。
// 虚拟地址UTOP到ULIM的位置，用户可以进行读操作，但不能写。
// 大于ULIM的位置，用户不能读、写。
void
mem_init(void)
{
	uint32_t cr0;
	size_t n;

	// Find out how much memory the machine has (npages & npages_basemem).
	i386_detect_memory();

	// Remove this line when you're ready to test this function.
	// panic("mem_init: This function is not finished\n");

	//////////////////////////////////////////////////////////////////////
	// create initial page directory.
	kern_pgdir = (pde_t *) boot_alloc(PGSIZE);//注意：返回的都是虚拟地址
	//pde_t是4个字节的，PGSIZE是4096，即boot_alloc(PGSIZE)分配了4096个字节的内存。
	//所以一共有1024个page directory entry
	memset(kern_pgdir, 0, PGSIZE);

	//////////////////////////////////////////////////////////////////////
	// Recursively insert PD in itself as a page table, to form
	// a virtual page table at virtual address UVPT.
	// (For now, you don't have to understand the greater purpose of the
	// following line.)

	// Permissions: kernel R, user R
	// 因为UVPT的位置处于UTOP～ULIM之间，所以对用户来说是只读的。
	kern_pgdir[PDX(UVPT)] = PADDR(kern_pgdir) | PTE_U | PTE_P;
	// 相当于boot_map_region(kern_pgdir, UVPT, PGSIZE, PADDR(kern_pgdir), PTE_U | PTE_P);
	// 但现在 还不能使用boot_map_region，所以需要通过手动来设置。
	
	//页目录的物理位置也是需要页表来索引的。
	//UVPT的位置是一个页表，它存的第一项 就是有关页目录的物理位置的索引。
	//而页目录的物理位置为PADDR(kern_pgdir)

	//////////////////////////////////////////////////////////////////////
	// 分配npages个元素的数组pages，元素的类型为'struct PageInfo'
	// 内核 通过这个数组来 记录物理页：
	// 对于每个物理页，都会对应一个PageInfo
	// 使用memset把 所分配的PageInfo的每个field 都设置为0
	// Your code goes here:
	pages = (struct PageInfo *) boot_alloc(npages * sizeof(struct PageInfo));
	memset(pages, 0, npages * sizeof(struct PageInfo));

	//为envs数组分配内存空间
	envs = (struct Env*)boot_alloc(NENV*sizeof(struct Env));
	memset(envs, 0, NENV * sizeof(struct Env));

	//////////////////////////////////////////////////////////////////////
	// Make 'envs' point to an array of size 'NENV' of 'struct Env'.
	// LAB 3: Your code here.
	// 既然我们已经分配了初始的内核数据结构，我们可以开始设置page_free_list了。
	// 一旦完成page_free_list的初始化，后面的内存分配都会通过page_*函数来完成。
	// 特别地，我们可以通过boot_map_region和page_insert来映射内存。
	page_init();

	check_page_free_list(1);
	check_page_alloc();
	check_page();

	//////////////////////////////////////////////////////////////////////
	// Now we set up virtual memory

	//////////////////////////////////////////////////////////////////////
	// Map 'pages' read-only by the user at linear address UPAGES
	// Permissions:
	//    - the new image at UPAGES -- kernel R, user R
	//      (ie. perm = PTE_U | PTE_P)
	//    - pages itself -- kernel RW, user NONE
	// Your code goes here: //lab2:
	boot_map_region(kern_pgdir, UPAGES, PTSIZE, PADDR(pages), PTE_U);

	//////////////////////////////////////////////////////////////////////
	// Map the 'envs' array read-only by the user at linear address UENVS
	// (ie. perm = PTE_U | PTE_P).
	// Permissions:
	//    - the new image at UENVS  -- kernel R, user R
	//    - envs itself -- kernel RW, user NONE
	// LAB 3: Your code here. 在页表中设置UENVS的映射关系
	boot_map_region(kern_pgdir, UENVS, PTSIZE, PADDR(envs), PTE_U);

	//////////////////////////////////////////////////////////////////////
	// Use the physical memory that 'bootstack' refers to as the kernel
	// stack.  The kernel stack grows down from virtual address KSTACKTOP.
	// We consider the entire range from [KSTACKTOP-PTSIZE, KSTACKTOP)
	// to be the kernel stack, but break this into two pieces:
	//     * [KSTACKTOP-KSTKSIZE, KSTACKTOP) -- backed by physical memory
	//     * [KSTACKTOP-PTSIZE, KSTACKTOP-KSTKSIZE) -- not backed; so if
	//       the kernel overflows its stack, it will fault rather than
	//       overwrite memory.  Known as a "guard page".
	//     Permissions: kernel RW, user NONE
	// Your code goes here: //lab2:
	// boot_map_region(kern_pgdir, KSTACKTOP - KSTKSIZE, KSTKSIZE, PADDR(bootstack), PTE_W);
	// mem_init_mp()中将会 把 BSP的 stack也一起 初始化，所以 不需要 重复操作了。
	// 注意：bootstack和percpu_kstacks[0]是不一样的

	//////////////////////////////////////////////////////////////////////
	// Map all of physical memory at KERNBASE.
	// Ie.  the VA range [KERNBASE, 2^32) should map to
	//      the PA range [0, 2^32 - KERNBASE)
	// We might not have 2^32 - KERNBASE bytes of physical memory, but
	// we just set up the mapping anyway.
	// Permissions: kernel RW, user NONE
	// Your code goes here:	//lab2:
	boot_map_region(kern_pgdir, KERNBASE, 0xffffffff - KERNBASE, 0, PTE_W);

	

	// Initialize the SMP-related parts of the memory map
	mem_init_mp();

	// Check that the initial page directory has been set up correctly.
	check_kern_pgdir();

	// Switch from the minimal entry page directory to the full kern_pgdir
	// page table we just created.	Our instruction pointer should be
	// somewhere between KERNBASE and KERNBASE+4MB right now, which is
	// mapped the same way by both page tables.
	//
	// If the machine reboots at this point, you've probably set up your
	// kern_pgdir wrong.
	lcr3(PADDR(kern_pgdir));

	check_page_free_list(0);

	// entry.S set the really important flags in cr0 (including enabling
	// paging).  Here we configure the rest of the flags that we care about.
	cr0 = rcr0();
	cr0 |= CR0_PE|CR0_PG|CR0_AM|CR0_WP|CR0_NE|CR0_MP;
	cr0 &= ~(CR0_TS|CR0_EM);
	lcr0(cr0);	

	// Some more checks, only possible after kern_pgdir is installed.
	check_page_installed_pgdir();
}

// 修改kern_pgdir中的映射 来支持SMP
// 	 - 把 不同CPU的栈 映射到 它所对应的位置 [KSTACKTOP-PTSIZE, KSTACKTOP]
// 
static void
mem_init_mp(void)
{
	// 对于NCPU个cpu，分别将 它们所对应的CPU栈 映射到对应的位置
	// 
	// 对于CPU i，它使用的物理位置 是percpu_kstacks[i]所对应的物理位置
	// CPU i的 内核栈 从KSTACKTOP - i * (KSTKSIZE + KSTKGAP)开始往下增长。
	// 它包括2个内容：
	//     * [kstacktop_i - KSTKSIZE, kstacktop_i)
	//          -- 正常栈的使用范围
	//     * [kstacktop_i - (KSTKSIZE + KSTKGAP), kstacktop_i - KSTKSIZE)
	//          -- KSTKGAP是用来防止 内核栈溢出时 修改了别的CPU的栈的内容(guard page)
	//     权限: kernel RW, user NONE
	// 
	// LAB 4: Your code here:
	uintptr_t start_addr = KSTACKTOP - KSTKSIZE;    
    for (size_t i=0; i<NCPU; i++) {
        boot_map_region(kern_pgdir, (uintptr_t) start_addr, KSTKSIZE, PADDR(percpu_kstacks[i]), PTE_W | PTE_P);
        start_addr -= (KSTKSIZE + KSTKGAP);
    }
}

// --------------------------------------------------------------
// 记录物理页
// 对于每个物理页，在数组pages中，都会对应一个PageInfo的数据结构
// PageInfo里面会记录物理页的被索引次数，而且空的页都会被保存在page_free_list中。
// --------------------------------------------------------------

// 物理页的初始化
// 初始化物理页结构 和 page_free_list
// 一旦完成，boot_alloc不能再被使用。只能通过下面的页分配器来 分配和释放物理内存。
//
void
page_init(void)
{
	// LAB 4:
	// Change your code to mark the physical page at MPENTRY_PADDR
	// as in use

	// 1) 第一个物理页要被设置成已使用。
	// 这是 为了保护 实模式下的ID和BIOS。尽管xv6不会再使用它们。
	// 2) base内存剩余的部分[PGSIZE, npages_basemem*PGSIZE]要设置为空。
	// 即4KB~A0000的位置设置为空，表示可用。
	// 3) 然后就是IO hole了，用来分配给外部设备的，所以是 不可用的。
	// [IOPHYSMEM, EXTPHYSMEM) 即 0xA0000~0x100000
	// 4) 然后是外部内存区域 [EXTPHYSMEM, ...)
	//     Some of it is in use, some is free. Where is the kernel
	//     in physical memory?  Which pages are already in use for
	//     page tables and other data structures?
	//
	// Change the code to reflect this.
	// NB: DO NOT actually touch the physical memory corresponding to
	// free pages!
	
	size_t i;
	page_free_list = NULL;

	//num_alloc：在extmem区域已经被占用的物理页的个数
	int num_alloc = ((uint32_t)boot_alloc(0) - KERNBASE) / PGSIZE;
	//num_iohole：在io hole区域占用的页数
	int num_iohole = 96;

	size_t mp_page = PGNUM(MPENTRY_PADDR);
	for(i=0; i<npages; i++)
	{
		if(i==mp_page){ //lab4 exercise2: 物理页0x7000的位置 是用来加载多处理器启动代码的
			pages[i].pp_ref = 1;
        	continue;
		}
		else if(i==0) //第一个物理页是已使用的
		{
			pages[i].pp_ref = 1;
		}    
		else if(i >= npages_basemem && i < npages_basemem + num_iohole + num_alloc)
		{//iohole位置，以及 已使用的exmem的位置
			pages[i].pp_ref = 1;
		}
		else
		{
			pages[i].pp_ref = 0;
			pages[i].pp_link = page_free_list;//链表是逆序连接的
			page_free_list = &pages[i];
		}
	}
}

//
// 分配一个物理页。如果alloc_flags&ALLOC_ZERO(即alloc_flags==ALLOC_ZERO=1)
// 那么返回一个填充'\0'的物理页。
// 不需要对pp_ref进行自增。caller会对此负责的(显式地++或者通过page_insert)
//
// 记得把pp_link的内容设置为NULL
// 
// free memory不够，那么返回NULL
//
// 提示：使用page2kva和memset
struct PageInfo *
page_alloc(int alloc_flags)
{
	// Fill this function in
	struct PageInfo *result;
    if (page_free_list == NULL){
		return NULL;
	}        

    result= page_free_list;
    page_free_list = result->pp_link; //page_free_list指向下一个空的值
    result->pp_link = NULL;

    if (alloc_flags & ALLOC_ZERO){
		memset(page2kva(result), 0, PGSIZE); 
	}     

    return result;
}

//
// 把一个物理页释放，放到free list中
// 需要先检查该物理页的引用数是否为0
//
void
page_free(struct PageInfo *pp)
{
	// Fill this function in
	// Hint: You may want to panic if pp->pp_ref is nonzero or
	// pp->pp_link is not NULL.
	assert(pp->pp_ref == 0);
    assert(pp->pp_link == NULL);

    pp->pp_link = page_free_list;
    page_free_list = pp;
}

//
// 对一个物理页的引用数进行自减，如果是0，那么就释放它
//
void
page_decref(struct PageInfo* pp)
{
	if (--pp->pp_ref == 0)
		page_free(pp);
}

// 给定一个指向页目录的指针pgdir，函数pgdir_walk通过现行地址va返回
// 一个指向page table entry的指针。
// 这需要 2级页表的操作(page dir entry+page table entry)
//
// 对应的页表页(存放页表的一个物理页) 可能还不存在
// 如果对应的页表页 不存在，且create==false，那么返回NULL
// 否则，通过page_alloc分配一个新的页表页：
// 			如果分配失败，那么返回NULL
//			否则，分配到的页表页的索引数++
//		新分配的页表页清0，返回指向这个页表页的指针
//
// 提示1：你可以通过page2pa()来把PageInfo*转换成一个页的物理地址
//
// 提示2：x86 MMU会检查页目录 和 页表的 权限位，所以把权限位设得更宽松一点也是安全的
// Hint 2: the x86 MMU checks permission bits in both the page directory
// and the page table, so it's safe to leave permissions in the page
// directory more permissive than strictly necessary.
//
// 提示3：考虑使用inc/mmu.h中的操作页表和页目录项的 宏。
//
pte_t * pgdir_walk(pde_t *pgdir, const void * va, int create)
{
	pde_t* pde = pgdir + PDX(va);// page directory entry

	if(!(*pde & PTE_P)){//检查 对应的页表页 是否存在
		if(create){
			struct PageInfo* newPageTablePage = page_alloc(1);
			if (newPageTablePage==NULL){ //分配失败
				return NULL;
			}else{
				newPageTablePage->pp_ref++;
				*pde = (page2pa(newPageTablePage) | PTE_P | PTE_W | PTE_U);
			}
		}else{
			return NULL;
		}
	}

	pte_t* pte = NULL;
	pte = KADDR(PTE_ADDR(*pde));

	return &pte[PTX(va)];
}


// 
// 在页表中，把虚拟内存[va, va+size)映射到物理内存[pa, pa+size)
// 参数size, va, pa都是PGSIZE(4096)的倍数
// PDE中对PTE的权限 设置为perm|PTE_P
// 
// 这个函数 仅仅是用来 设置UTOP以上的虚拟内存的静态映射的。
// 静态是说：在操作系统的运行过程中，这部分的映射是不会改变的
// 这个函数也不允许改变已经映射好的物理页的pp_ref
// 
// 提示：助教的解法中使用了pgdir_walk
// 
static void
boot_map_region(pde_t *pgdir, uintptr_t va, size_t size, physaddr_t pa, int perm)
{
	// Fill this function in
	int nadd;
    pte_t *pageTableEntry = NULL;
    for(nadd = 0; nadd < size; nadd += PGSIZE)
    {
        pageTableEntry = pgdir_walk(pgdir,(void *)va, 1); //Get the table entry of this page.
        *pageTableEntry = (pa | perm | PTE_P);
                
        pa += PGSIZE;
        va += PGSIZE;
	}
}

// 
// 把物理页pp映射到虚拟地址va上
// page table entry的权限位设置为perm|PTE_P
// 
// 要求：
// 		- 如果已经有一个物理页映射在va上了，那么这个物理页要被移除page_remove()
// 		- 如果需要的话，可以分配一个页表，然后农户插入到pgdir中
// 		- 如果映射成功，那么pp->pp_ref要自增一下。
// 		- The TLB must be invalidated if a page was formerly present at 'va'.
// 
// 边界情况的提示：想想物理页pp在同一个pgdir中，重新插入到相同va时的情况。
// 但其实这种情况并不需要单独考虑，有一个简洁的方式来处理这种情况。
// 
// 返回值：
// 		成功，返回0
// 		页表不能分配，返回E_NO_MEM  (-E_NO_MEM, if page table couldn't be allocated)
// 
// 提示：TA的解法中使用了pgdir_walk, page_removehe page2pa
// 
int
page_insert(pde_t *pgdir, struct PageInfo *pp, void *va, int perm)
{
	// Fill this function in
	pte_t *entry = NULL;
    entry =  pgdir_walk(pgdir, va, 1);    //Get the mapping page of this address va.
    if(entry == NULL) 
		return -E_NO_MEM;

    pp->pp_ref++;
	//pp->pp_ref++这条语句，一定要放在page_remove之前，这是为了处理一种特殊情况：pp已经映射到va上了
	//因为page_remove应该会--
    if((*entry) & PTE_P) //已经有一个物理页pp被映射在这个va上了
    {
        tlb_invalidate(pgdir, va);
        page_remove(pgdir, va);
    }
    *entry = (page2pa(pp) | perm | PTE_P);
    pgdir[PDX(va)] |= perm;         //Remember this step!

	return 0;
}

// 
// 返回映射在va上的物理页
// 如果pte_store不是NULL时，那么我们 把该物理页的pte的地址 存到pte_store中
// page_remove的时候需要pte_store,
// 或者 别的系统调用的 参数中需要 page的权限时，需要pte_store。
// This is used by page_remove and
// can be used to verify page permissions for syscall arguments
// 
// 如果va上没有映射物理页，那么返回NULL
// 
// 提示：TA的解法中 使用了pgdir_walk和pa2page
// 
struct PageInfo *
page_lookup(pde_t *pgdir, void *va, pte_t **pte_store)
{
	// Fill this function in
	pte_t *entry = NULL;
    struct PageInfo *ret = NULL;

    entry = pgdir_walk(pgdir, va, 0);
    if(entry == NULL)
        return NULL;
    if(!(*entry & PTE_P)) //va上没有映射物理页
        return NULL;
    
    ret = pa2page(PTE_ADDR(*entry));
    if(pte_store != NULL){
        *pte_store = entry;
    }
    return ret;
}

// 取消物理页 和 va之间的映射
// 如果va上没有映射物理页，那么 直接返回
//
// 细节：
// 		- 物理页上的ref计数需要-1
// 		- 如果物理页的ref计数==0，那么这个物理页要被释放
// 		- 对应va的页表项要设为0(如果存在的话)
// 		- TLB要禁用这条va的映射
// 
// 提示：TA的解法中，使用了page_lookup, tlb_invalidate 和 page_decref
// 
void
page_remove(pde_t *pgdir, void *va)
{
	// Fill this function in
	pte_t *pte = NULL;
    struct PageInfo *page = page_lookup(pgdir, va, &pte);
    if(page == NULL)
		return;    
    
    page_decref(page);
    tlb_invalidate(pgdir, va);
    *pte = 0;
}

//
// Invalidate a TLB entry, but only if the page tables being
// edited are the ones currently in use by the processor.
//
void
tlb_invalidate(pde_t *pgdir, void *va)
{
	// Flush the entry only if we're modifying the current address space.
	if (!curenv || curenv->env_pgdir == pgdir)
		invlpg(va);
}

// 
// 在MMIO中获取size字节的空间，并 把物理内存的[pa, pa+size)的内容映射到这里
// 返回 该区域的base(虚拟地址).
// 传递的参数size 不一定是PGSIZE的整数倍
// 
void *
mmio_map_region(physaddr_t pa, size_t size)
{
	// 
	// base是调用mmio_map_region前，MMIO开始分配的地方。
	// 初始或为MMIOBASE。
	// 
	static uintptr_t base = MMIOBASE;

	// 把物理地址[pa, pa+size)映射到 虚拟地址[base, base+size)
	// 因为这部分内存是用于外部设备的内存(例如VGA显示)
	// 而不是常规的DRAM，所以需要告诉CPU，对这部分内存进行缓存是不安全的
	// 幸运地，页表项 为这个目的提供了 一些位：只需要 在PTE_W权限的基础上 加入
	// PTE_PCD|PTE_PWT (cache-disable和write through)就可以了。
	// IA32 3A版本的 10.5章节 可以获得更多的详细信息。
	// 
	// 需要 把size变为PGSIZE的整数倍
	// 需要 检查这次预留操作 会不会 超出MMIOLIM的范围
	// (如果超出，发出panic即可)
	// 
	// 提示：TA的解法中 使用了 boot_map_region
	// 
	// Your code here:
    size_t rounded_size = ROUNDUP(size, PGSIZE);

    if (base + rounded_size > MMIOLIM){
		panic("overflow MMIOLIM");
	}
    boot_map_region(kern_pgdir, base, rounded_size, pa, PTE_W|PTE_PCD|PTE_PWT);
    uintptr_t res_region_base = base;   
    base += rounded_size;       
    return (void *)res_region_base;
}

static uintptr_t user_mem_check_addr;

// 
// 检查一个进程 是否有对虚拟 地址[va, va+len)的访问权限 'perm | PTE_P'
// 通常perm里面 会至少包括PTE_U，但这不是必须的
// va和len不一定是 对齐到page的；你必须检查 那个范围中的所有page。
// 你可能需要检查的页的个数为：'len/PGSIZE'，'len/PGSIZE+1', 'len/PGSIZE+2'
// 
// 一个用户进程能访问某个虚拟地址的条件：
// 1. 这个地址 在ULIM之下
// 2. 符合 页表 中 给出的访问权限
// 
// 如果出错，那么设置 变量user_mem_check_addr为 第一个出错的 虚拟地址。
// 
// 如果用户程序 可以访问这个范围的地址，那么返回0；否则 返回 -E_FAULT
// 
int
user_mem_check(struct Env *env, const void *va, size_t len, int perm)
{
	// LAB 3: Your code here.
	char * end = NULL;
    char * start = NULL;
    start = ROUNDDOWN((char *)va, PGSIZE); 
    end = ROUNDUP((char *)(va + len), PGSIZE);//对齐一下
    pte_t *cur = NULL;

    for(; start < end; start += PGSIZE) {
        cur = pgdir_walk(env->env_pgdir, (void *)start, 0);
        if((int)start > ULIM || cur == NULL || ((uint32_t)(*cur) & perm) != perm) {
              if(start == ROUNDDOWN((char *)va, PGSIZE)) {
                    user_mem_check_addr = (uintptr_t)va;
              }
              else {
                      user_mem_check_addr = (uintptr_t)start;
              }
              return -E_FAULT;
        }
    }

	return 0;
}

// 
// 检查env是否 有对虚拟内存 [va, va+len)的访问权限 'perm | PTE_U | PTE_P'
// 如果有，那么 函数直接返回
// 如果没有，那么销毁env；如果 env是当前的进程，那么函数不会返回
// 
void
user_mem_assert(struct Env *env, const void *va, size_t len, int perm)
{
	if (user_mem_check(env, va, len, perm | PTE_U) < 0) {
		cprintf("[%08x] user_mem_check assertion failure for "
			"va %08x\n", env->env_id, user_mem_check_addr);
		env_destroy(env);	// may not return
	}
}


// --------------------------------------------------------------
// Checking functions.
// --------------------------------------------------------------

//
// Check that the pages on the page_free_list are reasonable.
//
static void
check_page_free_list(bool only_low_memory)
{
	struct PageInfo *pp;
	unsigned pdx_limit = only_low_memory ? 1 : NPDENTRIES;
	int nfree_basemem = 0, nfree_extmem = 0;
	char *first_free_page;

	if (!page_free_list)
		panic("'page_free_list' is a null pointer!");

	if (only_low_memory) {
		//相当于page_free_list指向地址小的位置开始。
		// Move pages with lower addresses first in the free
		// list, since entry_pgdir does not map all pages.
		struct PageInfo *pp1, *pp2;
		struct PageInfo **tp[2] = { &pp1, &pp2 };
		for (pp = page_free_list; pp; pp = pp->pp_link) {
			int pagetype = PDX(page2pa(pp)) >= pdx_limit;
			*tp[pagetype] = pp;
			tp[pagetype] = &pp->pp_link;
		}
		*tp[1] = 0;
		*tp[0] = pp2;
		page_free_list = pp1;
	}

	// if there's a page that shouldn't be on the free list,
	// try to make sure it eventually causes trouble.
	for (pp = page_free_list; pp; pp = pp->pp_link)
		if (PDX(page2pa(pp)) < pdx_limit)
			memset(page2kva(pp), 0x97, 128);

	first_free_page = (char *) boot_alloc(0);
	for (pp = page_free_list; pp; pp = pp->pp_link) {
		// check that we didn't corrupt the free list itself
		assert(pp >= pages);
		assert(pp < pages + npages);
		assert(((char *) pp - (char *) pages) % sizeof(*pp) == 0);

		// check a few pages that shouldn't be on the free list
		assert(page2pa(pp) != 0);
		assert(page2pa(pp) != IOPHYSMEM);
		assert(page2pa(pp) != EXTPHYSMEM - PGSIZE);
		assert(page2pa(pp) != EXTPHYSMEM);
		assert(page2pa(pp) < EXTPHYSMEM || (char *) page2kva(pp) >= first_free_page);
		// (new test for lab 4)
		assert(page2pa(pp) != MPENTRY_PADDR);

		if (page2pa(pp) < EXTPHYSMEM)
			++nfree_basemem;
		else
			++nfree_extmem;
	}

	assert(nfree_basemem > 0);
	assert(nfree_extmem > 0);

	cprintf("check_page_free_list() succeeded!\n");
}

//
// Check the physical page allocator (page_alloc(), page_free(),
// and page_init()).
//
static void
check_page_alloc(void)
{
	struct PageInfo *pp, *pp0, *pp1, *pp2;
	int nfree;
	struct PageInfo *fl;
	char *c;
	int i;

	if (!pages)
		panic("'pages' is a null pointer!");

	// check number of free pages
	for (pp = page_free_list, nfree = 0; pp; pp = pp->pp_link)
		++nfree;

	// should be able to allocate three pages
	pp0 = pp1 = pp2 = 0;
	assert((pp0 = page_alloc(0)));
	assert((pp1 = page_alloc(0)));
	assert((pp2 = page_alloc(0)));

	assert(pp0);
	assert(pp1 && pp1 != pp0);
	assert(pp2 && pp2 != pp1 && pp2 != pp0);
	assert(page2pa(pp0) < npages*PGSIZE);
	assert(page2pa(pp1) < npages*PGSIZE);
	assert(page2pa(pp2) < npages*PGSIZE);

	// temporarily steal the rest of the free pages
	fl = page_free_list;
	page_free_list = 0;

	// should be no free memory
	assert(!page_alloc(0));

	// free and re-allocate?
	page_free(pp0);
	page_free(pp1);
	page_free(pp2);
	pp0 = pp1 = pp2 = 0;
	assert((pp0 = page_alloc(0)));
	assert((pp1 = page_alloc(0)));
	assert((pp2 = page_alloc(0)));
	assert(pp0);
	assert(pp1 && pp1 != pp0);
	assert(pp2 && pp2 != pp1 && pp2 != pp0);
	assert(!page_alloc(0));

	// test flags
	memset(page2kva(pp0), 1, PGSIZE);
	page_free(pp0);
	assert((pp = page_alloc(ALLOC_ZERO)));
	assert(pp && pp0 == pp);
	c = page2kva(pp);
	for (i = 0; i < PGSIZE; i++)
		assert(c[i] == 0);

	// give free list back
	page_free_list = fl;

	// free the pages we took
	page_free(pp0);
	page_free(pp1);
	page_free(pp2);

	// number of free pages should be the same
	for (pp = page_free_list; pp; pp = pp->pp_link)
		--nfree;
	assert(nfree == 0);

	cprintf("check_page_alloc() succeeded!\n");
}

//
// Checks that the kernel part of virtual address space
// has been set up roughly correctly (by mem_init()).
//
// This function doesn't test every corner case,
// but it is a pretty good sanity check.
//

static void
check_kern_pgdir(void)
{
	uint32_t i, n;
	pde_t *pgdir;

	pgdir = kern_pgdir;

	// check pages array
	n = ROUNDUP(npages*sizeof(struct PageInfo), PGSIZE);
	for (i = 0; i < n; i += PGSIZE)
		assert(check_va2pa(pgdir, UPAGES + i) == PADDR(pages) + i);

	// check envs array (new test for lab 3)
	n = ROUNDUP(NENV*sizeof(struct Env), PGSIZE);
	for (i = 0; i < n; i += PGSIZE)
		assert(check_va2pa(pgdir, UENVS + i) == PADDR(envs) + i);

	// check phys mem
	for (i = 0; i < npages * PGSIZE; i += PGSIZE)
		assert(check_va2pa(pgdir, KERNBASE + i) == i);

	// check kernel stack
	// (updated in lab 4 to check per-CPU kernel stacks)
	for (n = 0; n < NCPU; n++) {
		uint32_t base = KSTACKTOP - (KSTKSIZE + KSTKGAP) * (n + 1);
		for (i = 0; i < KSTKSIZE; i += PGSIZE)
			assert(check_va2pa(pgdir, base + KSTKGAP + i)
				== PADDR(percpu_kstacks[n]) + i);
		for (i = 0; i < KSTKGAP; i += PGSIZE)
			assert(check_va2pa(pgdir, base + i) == ~0);
	}

	// check PDE permissions
	for (i = 0; i < NPDENTRIES; i++) {
		switch (i) {
		case PDX(UVPT):
		case PDX(KSTACKTOP-1):
		case PDX(UPAGES):
		case PDX(UENVS):
		case PDX(MMIOBASE):
			assert(pgdir[i] & PTE_P);
			break;
		default:
			if (i >= PDX(KERNBASE)) {
				assert(pgdir[i] & PTE_P);
				assert(pgdir[i] & PTE_W);
			} else
				assert(pgdir[i] == 0);
			break;
		}
	}
	cprintf("check_kern_pgdir() succeeded!\n");
}

// This function returns the physical address of the page containing 'va',
// defined by the page directory 'pgdir'.  The hardware normally performs
// this functionality for us!  We define our own version to help check
// the check_kern_pgdir() function; it shouldn't be used elsewhere.

static physaddr_t
check_va2pa(pde_t *pgdir, uintptr_t va)
{
	pte_t *p;

	pgdir = &pgdir[PDX(va)];
	if (!(*pgdir & PTE_P))
		return ~0;
	p = (pte_t*) KADDR(PTE_ADDR(*pgdir));
	if (!(p[PTX(va)] & PTE_P))
		return ~0;
	return PTE_ADDR(p[PTX(va)]);
}


// check page_insert, page_remove, &c
static void
check_page(void)
{
	struct PageInfo *pp, *pp0, *pp1, *pp2;
	struct PageInfo *fl;
	pte_t *ptep, *ptep1;
	void *va;
	uintptr_t mm1, mm2;
	int i;
	extern pde_t entry_pgdir[];

	// should be able to allocate three pages
	pp0 = pp1 = pp2 = 0;
	assert((pp0 = page_alloc(0)));
	assert((pp1 = page_alloc(0)));
	assert((pp2 = page_alloc(0)));

	assert(pp0);
	assert(pp1 && pp1 != pp0);
	assert(pp2 && pp2 != pp1 && pp2 != pp0);

	// temporarily steal the rest of the free pages
	fl = page_free_list;
	page_free_list = 0;

	// should be no free memory
	assert(!page_alloc(0));

	// there is no page allocated at address 0
	assert(page_lookup(kern_pgdir, (void *) 0x0, &ptep) == NULL);

	// there is no free memory, so we can't allocate a page table
	assert(page_insert(kern_pgdir, pp1, 0x0, PTE_W) < 0);

	// free pp0 and try again: pp0 should be used for page table
	page_free(pp0);
	assert(page_insert(kern_pgdir, pp1, 0x0, PTE_W) == 0);
	assert(PTE_ADDR(kern_pgdir[0]) == page2pa(pp0));
	assert(check_va2pa(kern_pgdir, 0x0) == page2pa(pp1));
	assert(pp1->pp_ref == 1);
	assert(pp0->pp_ref == 1);

	// should be able to map pp2 at PGSIZE because pp0 is already allocated for page table
	assert(page_insert(kern_pgdir, pp2, (void*) PGSIZE, PTE_W) == 0);
	assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp2));
	assert(pp2->pp_ref == 1);

	// should be no free memory
	assert(!page_alloc(0));

	// should be able to map pp2 at PGSIZE because it's already there
	assert(page_insert(kern_pgdir, pp2, (void*) PGSIZE, PTE_W) == 0);
	assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp2));
	assert(pp2->pp_ref == 1);

	// pp2 should NOT be on the free list
	// could happen in ref counts are handled sloppily in page_insert
	assert(!page_alloc(0));

	// check that pgdir_walk returns a pointer to the pte
	ptep = (pte_t *) KADDR(PTE_ADDR(kern_pgdir[PDX(PGSIZE)]));
	assert(pgdir_walk(kern_pgdir, (void*)PGSIZE, 0) == ptep+PTX(PGSIZE));

	// should be able to change permissions too.
	assert(page_insert(kern_pgdir, pp2, (void*) PGSIZE, PTE_W|PTE_U) == 0);
	assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp2));
	assert(pp2->pp_ref == 1);
	assert(*pgdir_walk(kern_pgdir, (void*) PGSIZE, 0) & PTE_U);
	assert(kern_pgdir[0] & PTE_U);

	// should be able to remap with fewer permissions
	assert(page_insert(kern_pgdir, pp2, (void*) PGSIZE, PTE_W) == 0);
	assert(*pgdir_walk(kern_pgdir, (void*) PGSIZE, 0) & PTE_W);
	assert(!(*pgdir_walk(kern_pgdir, (void*) PGSIZE, 0) & PTE_U));

	// should not be able to map at PTSIZE because need free page for page table
	assert(page_insert(kern_pgdir, pp0, (void*) PTSIZE, PTE_W) < 0);

	// insert pp1 at PGSIZE (replacing pp2)
	assert(page_insert(kern_pgdir, pp1, (void*) PGSIZE, PTE_W) == 0);
	assert(!(*pgdir_walk(kern_pgdir, (void*) PGSIZE, 0) & PTE_U));

	// should have pp1 at both 0 and PGSIZE, pp2 nowhere, ...
	assert(check_va2pa(kern_pgdir, 0) == page2pa(pp1));
	assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp1));
	// ... and ref counts should reflect this
	assert(pp1->pp_ref == 2);
	assert(pp2->pp_ref == 0);

	// pp2 should be returned by page_alloc
	assert((pp = page_alloc(0)) && pp == pp2);

	// unmapping pp1 at 0 should keep pp1 at PGSIZE
	page_remove(kern_pgdir, 0x0);
	assert(check_va2pa(kern_pgdir, 0x0) == ~0);
	assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp1));
	assert(pp1->pp_ref == 1);
	assert(pp2->pp_ref == 0);

	// test re-inserting pp1 at PGSIZE
	assert(page_insert(kern_pgdir, pp1, (void*) PGSIZE, 0) == 0);
	assert(pp1->pp_ref);
	assert(pp1->pp_link == NULL);

	// unmapping pp1 at PGSIZE should free it
	page_remove(kern_pgdir, (void*) PGSIZE);
	assert(check_va2pa(kern_pgdir, 0x0) == ~0);
	assert(check_va2pa(kern_pgdir, PGSIZE) == ~0);
	assert(pp1->pp_ref == 0);
	assert(pp2->pp_ref == 0);

	// so it should be returned by page_alloc
	assert((pp = page_alloc(0)) && pp == pp1);

	// should be no free memory
	assert(!page_alloc(0));

	// forcibly take pp0 back
	assert(PTE_ADDR(kern_pgdir[0]) == page2pa(pp0));
	kern_pgdir[0] = 0;
	assert(pp0->pp_ref == 1);
	pp0->pp_ref = 0;

	// check pointer arithmetic in pgdir_walk
	page_free(pp0);
	va = (void*)(PGSIZE * NPDENTRIES + PGSIZE);
	ptep = pgdir_walk(kern_pgdir, va, 1);
	ptep1 = (pte_t *) KADDR(PTE_ADDR(kern_pgdir[PDX(va)]));
	assert(ptep == ptep1 + PTX(va));
	kern_pgdir[PDX(va)] = 0;
	pp0->pp_ref = 0;

	// check that new page tables get cleared
	memset(page2kva(pp0), 0xFF, PGSIZE);
	page_free(pp0);
	pgdir_walk(kern_pgdir, 0x0, 1);
	ptep = (pte_t *) page2kva(pp0);
	for(i=0; i<NPTENTRIES; i++)
		assert((ptep[i] & PTE_P) == 0);
	kern_pgdir[0] = 0;
	pp0->pp_ref = 0;

	// give free list back
	page_free_list = fl;

	// free the pages we took
	page_free(pp0);
	page_free(pp1);
	page_free(pp2);

	// test mmio_map_region
	mm1 = (uintptr_t) mmio_map_region(0, 4097);
	mm2 = (uintptr_t) mmio_map_region(0, 4096);
	// check that they're in the right region
	assert(mm1 >= MMIOBASE && mm1 + 8192 < MMIOLIM);
	assert(mm2 >= MMIOBASE && mm2 + 8192 < MMIOLIM);
	// check that they're page-aligned
	assert(mm1 % PGSIZE == 0 && mm2 % PGSIZE == 0);
	// check that they don't overlap
	assert(mm1 + 8192 <= mm2);
	// check page mappings
	assert(check_va2pa(kern_pgdir, mm1) == 0);
	assert(check_va2pa(kern_pgdir, mm1+PGSIZE) == PGSIZE);
	assert(check_va2pa(kern_pgdir, mm2) == 0);
	assert(check_va2pa(kern_pgdir, mm2+PGSIZE) == ~0);
	// check permissions
	assert(*pgdir_walk(kern_pgdir, (void*) mm1, 0) & (PTE_W|PTE_PWT|PTE_PCD));
	assert(!(*pgdir_walk(kern_pgdir, (void*) mm1, 0) & PTE_U));
	// clear the mappings
	*pgdir_walk(kern_pgdir, (void*) mm1, 0) = 0;
	*pgdir_walk(kern_pgdir, (void*) mm1 + PGSIZE, 0) = 0;
	*pgdir_walk(kern_pgdir, (void*) mm2, 0) = 0;

	cprintf("check_page() succeeded!\n");
}

// check page_insert, page_remove, &c, with an installed kern_pgdir
static void
check_page_installed_pgdir(void)
{
	struct PageInfo *pp, *pp0, *pp1, *pp2;
	struct PageInfo *fl;
	pte_t *ptep, *ptep1;
	uintptr_t va;
	int i;

	// check that we can read and write installed pages
	pp1 = pp2 = 0;
	assert((pp0 = page_alloc(0)));
	assert((pp1 = page_alloc(0)));
	assert((pp2 = page_alloc(0)));
	page_free(pp0);
	memset(page2kva(pp1), 1, PGSIZE);
	memset(page2kva(pp2), 2, PGSIZE);
	page_insert(kern_pgdir, pp1, (void*) PGSIZE, PTE_W);
	assert(pp1->pp_ref == 1);
	assert(*(uint32_t *)PGSIZE == 0x01010101U);
	page_insert(kern_pgdir, pp2, (void*) PGSIZE, PTE_W);
	assert(*(uint32_t *)PGSIZE == 0x02020202U);
	assert(pp2->pp_ref == 1);
	assert(pp1->pp_ref == 0);
	*(uint32_t *)PGSIZE = 0x03030303U;
	assert(*(uint32_t *)page2kva(pp2) == 0x03030303U);
	page_remove(kern_pgdir, (void*) PGSIZE);
	assert(pp2->pp_ref == 0);

	// forcibly take pp0 back
	assert(PTE_ADDR(kern_pgdir[0]) == page2pa(pp0));
	kern_pgdir[0] = 0;
	assert(pp0->pp_ref == 1);
	pp0->pp_ref = 0;

	// free the pages we took
	page_free(pp0);

	cprintf("check_page_installed_pgdir() succeeded!\n");
}
