OUTPUT_ARCH("riscv")
ENTRY(_start)

#include <aosd/asm.h>

SECTIONS
{
  . = KERNEL_LINK_ADDR;
  _skernel = .;

  .text : AT(KERNEL_LOAD_ADDR + (ADDR(.text) - KERNEL_LINK_ADDR)) {
    _stext = .;
    *(.text.head)
    *(.text)
    *(.text.*)
    _etext = .;
  }

  . = ALIGN(PAGE_SIZE);
  .rodata : AT(KERNEL_LOAD_ADDR + (ADDR(.rodata) - KERNEL_LINK_ADDR)) {
    _srodata = .;
    *(.srodata)
    *(.srodata.*)
    *(.rodata)
    *(.rodata.*)
    _erodata = .;
  }

  . = ALIGN(PAGE_SIZE);
  .data : AT(KERNEL_LOAD_ADDR + (ADDR(.data) - KERNEL_LINK_ADDR)) {
    _sdata = .;
    *(.sdata)
    *(.sdata.*)
    *(.data)
    *(.data.*)
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
    init_stack = .;
    . += NR_INIT_STACK_PAGES * PAGE_SIZE * NR_PROCESSORS;
    init_stack_top = .;

    . = ALIGN(PAGE_SIZE);
    early_pgdir = .;
    . += NR_EARLY_PGDIR_PAGES * PAGE_SIZE;

    . = ALIGN(PAGE_SIZE);
    kernel_pgdir = .;
    . += PAGE_SIZE;

    _ebss = .;
  }

  . = ALIGN(PAGE_SIZE);
  _ekernel = .;
}
