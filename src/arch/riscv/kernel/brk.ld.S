OUTPUT_ARCH("riscv")
ENTRY(_start)

#include <arch/asm/vmlayout.h>
#include <arch/asm/page.h>
#include <arch/asm/mm.h>
#include <brk/asm/cpu.h>

SECTIONS
{
  . = KERNEL_LINK_ADDR;
  _skernel = .;

  .text : AT(KERNEL_LOAD_ADDR + (ADDR(.text) - KERNEL_LINK_ADDR)) {
    _stext = .;
    *(.text.head)
    *(.text)
    *(.text.*)
    . = ALIGN(PAGE_SIZE);
    _etext = .;
  }

  . = ALIGN(PAGE_SIZE);
  .rodata : AT(KERNEL_LOAD_ADDR + (ADDR(.rodata) - KERNEL_LINK_ADDR)) {
    _srodata = .;
    *(.srodata)
    *(.srodata.*)
    *(.rodata)
    *(.rodata.*)

    . = ALIGN(8);
    __initcall_start = .;
    KEEP(*(.initcall0))
    KEEP(*(.initcall1))
    KEEP(*(.initcall2))
    KEEP(*(.initcall3))
    __initcall_end = .;

    . = ALIGN(PAGE_SIZE);
    _erodata = .;
  }

  . = ALIGN(PAGE_SIZE);
  .data : AT(KERNEL_LOAD_ADDR + (ADDR(.data) - KERNEL_LINK_ADDR)) {
    _sdata = .;
    *(.sdata)
    *(.sdata.*)
    *(.data)
    *(.data.*)
    . = ALIGN(PAGE_SIZE);
    _edata = .;
  }

  . = ALIGN(PAGE_SIZE);
  .bss : AT(KERNEL_LOAD_ADDR + (ADDR(.bss) - KERNEL_LINK_ADDR)) {
    _sbss = .;
    *(.sbss)
    *(.sbss.*)
    *(.bss)
    *(.bss.*)

    . = ALIGN(PAGE_SIZE);
    init_stack_rsvd_pages = .;
    . += NR_INIT_STACK_RSVD_PAGES * PAGE_SIZE;

    . = ALIGN(PAGE_SIZE);
    early_pgdir_rsvd_pages = .;
    . += NR_EARLY_PGDIR_RSVD_PAGES * PAGE_SIZE;

    . = ALIGN(PAGE_SIZE);
    kernel_pgdir = .;
    . += PAGE_SIZE;

    . = ALIGN(PAGE_SIZE);
    _ebss = .;
  }

  . = ALIGN(PAGE_SIZE);
  _ekernel = .;
}
