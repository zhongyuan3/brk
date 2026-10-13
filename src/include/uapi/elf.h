#ifndef UAPI_ELF_H
#define UAPI_ELF_H

#include <uapi/types.h>

#define EI_NIDENT 16

#define ELFMAG "\177ELF"
#define SELFMAG 4

/* These constants define the different elf file types */
#define ET_NONE 0
#define ET_REL 1
#define ET_EXEC 2
#define ET_DYN 3
#define ET_CORE 4
#define ET_LOPROC 0xff00
#define ET_HIPROC 0xffff

#define ELFCLASS32 1
#define ELFCLASS64 2
#define EI_CLASS 4

#if __riscv_xlen == 32

struct elf_hdr {
	u8 e_ident[EI_NIDENT];
	u16 e_type; /* Object file type */
	u16 e_machine; /* Architecture */
	u32 e_version; /* Object file version */
	u32 e_entry; /* Enter point virtual address */
	u32 e_phoff; /* Program header table file offset */
	u32 e_shoff; /* Section header table file offset */
	u32 e_flags; /* Processor-specific flags */
	u16 e_ehsize; /* ELF header size in bytes */
	u16 e_phentsize; /* Program header table entry size */
	u16 e_phnum; /* Program header table entry count */
	u16 e_shentsize; /* Section header table entry size */
	u16 e_shnum; /* Section header table entry count */
	u16 e_shstrndx; /* Section header string table index */
};

struct elf_phdr {
	u32 p_type;
	u32 p_offset; /* Segment file offset */
	u32 p_vaddr; /* Segment virtual address */
	u32 p_paddr; /* Segment physical address */
	u32 p_filesz; /* Segment size in file */
	u32 p_memsz; /* Segment size int memory */
	u32 p_flags;
	u32 p_align; /* Segment alignment */
};

#else

struct elf_hdr {
	u8 e_ident[EI_NIDENT];
	u16 e_type; /* Object file type */
	u16 e_machine; /* Architecture */
	u32 e_version; /* Object file version */
	u64 e_entry; /* Enter point virtual address */
	u64 e_phoff; /* Program header table file offset */
	u64 e_shoff; /* Section header table file offset */
	u32 e_flags; /* Processor-specific flags */
	u16 e_ehsize; /* ELF header size in bytes */
	u16 e_phentsize; /* Program header table entry size */
	u16 e_phnum; /* Program header table entry count */
	u16 e_shentsize; /* Section header table entry size */
	u16 e_shnum; /* Section header table entry count */
	u16 e_shstrndx; /* Section header string table index */
};

struct elf_phdr {
	u32 p_type;
	u32 p_flags;
	u64 p_offset; /* Segment file offset */
	u64 p_vaddr; /* Segment virtual address */
	u64 p_paddr; /* Segment physical address */
	u64 p_filesz; /* Segment size in file */
	u64 p_memsz; /* Segment size int memory */
	u64 p_align; /* Segment alignment */
};

#endif

/* These constants are for the segment types stored in the image headers */
#define PT_NULL 0
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_NOTE 4
#define PT_SHLIB 5
#define PT_PHDR 6
#define PT_TLS 7 /* Thread local storage segment */

/* These constants define the permissions on sections in the program
   header, p_flags. */
#define PF_R 0x4
#define PF_W 0x2
#define PF_X 0x1

#endif