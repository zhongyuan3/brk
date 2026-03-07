OUTPUT_ARCH("riscv")
ENTRY(_start)

#include <aosd/asm.h>

USER_BASE = 0x10000;

SECTIONS
{
  . = USER_BASE;

  . = ALIGN(PAGE_SIZE);
  .text : {
    *(.text)
    *(.text.*)
    *(.gnu.linkonce.t*)
  }

  . = ALIGN(PAGE_SIZE);
  .rodata : {
    *(.rodata)
    *(.rodata.*)
    *(.gnu.linkonce.r*)
  }

  . = ALIGN(PAGE_SIZE);
  .data : {
    __data_start = .;
    *(.data)
    *(.data.*)
    *(.gnu.linkonce.d*)
    __data_end = .;
  }

  . = ALIGN(PAGE_SIZE);
  .bss : {
    __bss_start = .;
    *(.bss)
    *(.bss.*)
    *(COMMON)
    __bss_end = .;
  }

  . = ASSERT(. < 0x8000000000000000, "User program overflow to kernel space!");
}
