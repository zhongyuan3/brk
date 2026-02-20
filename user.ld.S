OUTPUT_ARCH("riscv")
ENTRY(_start)

#include <aosd/asm.h>

SECTIONS
{
  . = 0x0;

  . = ALIGN(PAGE_SIZE);
  .text : {
    *(.text .text.*)
  }

  . = ALIGN(PAGE_SIZE);
  .rodata : {
    *(.srodata .srodata.*)
    *(.rodata .rodata.*)
  }

  . = ALIGN(PAGE_SIZE);
  .data : {
    *(.sdata .sdata.*)
    *(.data .data.*)
  }

  . = ALIGN(PAGE_SIZE);
  .bss : {
    *(.sbss .sbss.*)
    *(.bss .bss.*)
  }
}
