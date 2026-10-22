OUTPUT_ARCH("riscv")
ENTRY(_start)

#include <arch/asm/vmlayout.h>
#include <arch/asm/page.h>
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
    _ebss = .;
  }

  . = ALIGN(PAGE_SIZE);
  _ekernel = .;
}
