#ifndef _UAPI_ELF_H
#define _UAPI_ELF_H

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

#define ELFCLASS64 2
#define EI_CLASS 4

struct __brk_elf_hdr {
	__brk_u8 e_ident[EI_NIDENT];
	__brk_u16 e_type; /* Object file type */
	__brk_u16 e_machine; /* Architecture */
	__brk_u32 e_version; /* Object file version */
	__brk_u64 e_entry; /* Enter point virtual address */
	__brk_u64 e_phoff; /* Program header table file offset */
	__brk_u64 e_shoff; /* Section header table file offset */
	__brk_u32 e_flags; /* Processor-specific flags */
	__brk_u16 e_ehsize; /* ELF header size in bytes */
	__brk_u16 e_phentsize; /* Program header table entry size */
	__brk_u16 e_phnum; /* Program header table entry count */
	__brk_u16 e_shentsize; /* Section header table entry size */
	__brk_u16 e_shnum; /* Section header table entry count */
	__brk_u16 e_shstrndx; /* Section header string table index */
};

struct __brk_elf_phdr {
	__brk_u32 p_type;
	__brk_u32 p_flags;
	__brk_u64 p_offset; /* Segment file offset */
	__brk_u64 p_vaddr; /* Segment virtual address */
	__brk_u64 p_paddr; /* Segment physical address */
	__brk_u64 p_filesz; /* Segment size in file */
	__brk_u64 p_memsz; /* Segment size int memory */
	__brk_u64 p_align; /* Segment alignment */
};

/* These constants are for the segment types stored in the image headers */
#define PT_NULL 0
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_NOTE 4
#define PT_SHLIB 5
#define PT_PHDR 6
#define PT_TLS 7 /* Thread local storage segment */

/* OS-specific segment types (0x60000000 .. 0x6fffffff). */
#define PT_LOOS 0x60000000
#define PT_HIOS 0x6fffffff
#define PT_GNU_STACK 0x6474e551u /* stack permission flags */
#define PT_GNU_RELRO 0x6474e552u /* read-only after relocation */

/* These constants define the permissions on sections in the program
   header, p_flags. */
#define PF_R 0x4
#define PF_W 0x2
#define PF_X 0x1

#define EI_DATA 5 /* data encoding byte of e_ident */
#define ELFDATA2LSB 1 /* little-endian */
#define EI_VERSION 6 /* version byte of e_ident */
#define EV_CURRENT 1

#define EM_RISCV 243 /* RISC-V */

/* RISC-V e_flags: float ABI occupies bits 1:2. */
#define EF_RISCV_FLOAT_ABI_MASK 0x6
#define EF_RISCV_FLOAT_ABI_SOFT 0x0
#define EF_RISCV_FLOAT_ABI_SINGLE 0x2
#define EF_RISCV_FLOAT_ABI_DOUBLE 0x4
#define EF_RISCV_FLOAT_ABI_QUAD 0x6

/* Dynamic section tags (the kernel only needs the relocation table
 * pointers to apply base relocations to PIE images; symbol-level
 * dynamic linking happens in user space). */
#define PT_DYNAMIC_WALK
struct __brk_elf_dyn {
	__brk_s64 d_tag;
	union {
		__brk_u64 d_val;
		__brk_u64 d_ptr;
	} d_un;
};

struct __brk_elf_rela {
	__brk_u64 r_offset;
	__brk_u64 r_info;
	__brk_s64 r_addend;
};

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_INIT 12
#define DT_FINI 13
#define DT_SONAME 14
#define DT_REL 17 /* REL relocations (unsupported on RISC-V) */
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_JMPREL 23
#define DT_TEXTREL 22
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_HASH 4

/* RISC-V relocation types. */
#define R_RISCV_NONE 0
#define R_RISCV_64 2
#define R_RISCV_RELATIVE 3
#define R_RISCV_RELAX 51

#endif
