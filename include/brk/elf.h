#ifndef BRK_ELF_H
#define BRK_ELF_H

#include <brk/types.h>

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

struct elf64_hdr {
	uint8_t e_ident[EI_NIDENT];
	uint16_t e_type; /* Object file type */
	uint16_t e_machine; /* Architecture */
	uint32_t e_version; /* Object file version */
	uint64_t e_entry; /* Enter point virtual address */
	uint64_t e_phoff; /* Program header table file offset */
	uint64_t e_shoff; /* Section header table file offset */
	uint32_t e_flags; /* Processor-specific flags */
	uint16_t e_ehsize; /* ELF header size in bytes */
	uint16_t e_phentsize; /* Program header table entry size */
	uint16_t e_phnum; /* Program header table entry count */
	uint16_t e_shentsize; /* Section header table entry size */
	uint16_t e_shnum; /* Section header table entry count */
	uint16_t e_shstrndx; /* Section header string table index */
};

struct elf64_phdr {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset; /* Segment file offset */
	uint64_t p_vaddr; /* Segment virtual address */
	uint64_t p_paddr; /* Segment physical address */
	uint64_t p_filesz; /* Segment size in file */
	uint64_t p_memsz; /* Segment size int memory */
	uint64_t p_align; /* Segment alignment */
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

/* These constants define the permissions on sections in the program
   header, p_flags. */
#define PF_R 0x4
#define PF_W 0x2
#define PF_X 0x1

#endif
