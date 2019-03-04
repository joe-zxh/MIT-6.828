// 详细内容可以参考：https://jzhihui.iteye.com/blog/1447570
#ifndef JOS_INC_ELF_H
#define JOS_INC_ELF_H

#define ELF_MAGIC 0x464C457FU	/* "\x7FELF" in little endian */

struct Elf {
	uint32_t e_magic;	// must equal ELF_MAGIC
	uint8_t e_elf[12];
	// 应该是为了和elf32_hdr对齐的，elf32_hdr里面有16个字节，而这里只用了前面4个字节(e_magic)作为MAGIC
	uint16_t e_type; //文件类型，2表示 可执行文件
	uint16_t e_machine; // e_machine表示机器类别，3表示386机器
	uint32_t e_version;
	uint32_t e_entry; //进程开始的虚地址，即系统将控制转移的位置
	uint32_t e_phoff; //program header table的偏移(offset)
	uint32_t e_shoff; //section header table的偏移(offset)
	uint32_t e_flags; //处理器相关的标志
	uint16_t e_ehsize;//ELF文件头的大小
	uint16_t e_phentsize; //program header entry size: PH表中的entry的大小
	uint16_t e_phnum; //PH表中的entry的数量
	uint16_t e_shentsize; //section header entry size
	uint16_t e_shnum; //SH表中的entry的数量
	uint16_t e_shstrndx; //Section header name string table index
};

struct Proghdr {
	uint32_t p_type;
	uint32_t p_offset;
	uint32_t p_va;
	uint32_t p_pa;
	uint32_t p_filesz;
	uint32_t p_memsz; /* size in memory */
	uint32_t p_flags;
	uint32_t p_align;
};

struct Secthdr {
	uint32_t sh_name;
	uint32_t sh_type;
	uint32_t sh_flags;
	uint32_t sh_addr;
	uint32_t sh_offset;
	uint32_t sh_size;
	uint32_t sh_link;
	uint32_t sh_info;
	uint32_t sh_addralign;
	uint32_t sh_entsize;
};

// Values for Proghdr::p_type
#define ELF_PROG_LOAD		1

// Flag bits for Proghdr::p_flags
#define ELF_PROG_FLAG_EXEC	1
#define ELF_PROG_FLAG_WRITE	2
#define ELF_PROG_FLAG_READ	4

// Values for Secthdr::sh_type
#define ELF_SHT_NULL		0
#define ELF_SHT_PROGBITS	1
#define ELF_SHT_SYMTAB		2
#define ELF_SHT_STRTAB		3

// Values for Secthdr::sh_name
#define ELF_SHN_UNDEF		0

#endif /* !JOS_INC_ELF_H */
