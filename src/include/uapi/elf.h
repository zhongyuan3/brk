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

#define ELFCLASS64 2
#define EI_CLASS 4

struct elf_hdr {
	__u8 e_ident[EI_NIDENT];
	__u16 e_type; /* Object file type */
	__u16 e_machine; /* Architecture */
	__u32 e_version; /* Object file version */
	__u64 e_entry; /* Enter point virtual address */
	__u64 e_phoff; /* Program header table file offset */
	__u64 e_shoff; /* Section header table file offset */
	__u32 e_flags; /* Processor-specific flags */
	__u16 e_ehsize; /* ELF header size in bytes */
	__u16 e_phentsize; /* Program header table entry size */
	__u16 e_phnum; /* Program header table entry count */
	__u16 e_shentsize; /* Section header table entry size */
	__u16 e_shnum; /* Section header table entry count */
	__u16 e_shstrndx; /* Section header string table index */
};

struct elf_phdr {
	__u32 p_type;
	__u32 p_flags;
	__u64 p_offset; /* Segment file offset */
	__u64 p_vaddr; /* Segment virtual address */
	__u64 p_paddr; /* Segment physical address */
	__u64 p_filesz; /* Segment size in file */
	__u64 p_memsz; /* Segment size int memory */
	__u64 p_align; /* Segment alignment */
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
