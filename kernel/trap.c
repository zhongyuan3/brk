#include <aosd/macros.h>
#include <aosd/riscv.h>
#include <aosd/sbi.h>
#include <aosd/trap.h>
#include <aosd/types.h>

static char const *interrupt_str(unsigned int intno)
{
	static char const *intstrs[] = {
		"U-mode software interrupt",
		"S-mode software interrupt",
		"reserved-1",
		"M-mode software interrupt",
		"U-mode timer interrupt",
		"S-mode timer interrupt",
		"reserved-2",
		"M-mode timer interrupt",
		"U-mode external interrupt",
		"S-mode external interrupt",
		"reserved-3",
		"M-mode external interrupt",
		"reserved-4",
		"reserved-5",
		"reserved-6",
		"reserved-7",
	};

	if (intno >= countof(intstrs))
		return "unknown interrupt";
	else
		return intstrs[intno];
}

static char const *exception_str(unsigned int excno)
{
	static char const *excstrs[] = {
		"Instruction address misaligned",
		"Instruction access fault",
		"Illegal instruction",
		"Breakpoint",
		"Load address misaligned",
		"Load access fault",
		"Store/AMO address misaligned",
		"Store/AMO access fault",
		"Environment call from U-mode",
		"Environment call from S-mode",
		"reserved-1",
		"Environment call from M-mode",
		"Instruction page fault",
		"Load page fault",
		"reserved-2",
		"Store/AMO page fault",
	};

	if (excno >= countof(excstrs))
		return "unknown exception";
	else
		return excstrs[excno];
}

void early_trap_vector(void)
{
	uint64_t scause = read_scause();

	if (scause & (1ULL << 63)) {
		sbi_console_putstr("interrupt: ");
		sbi_console_putstr(interrupt_str(scause & 0x3FF));
	} else {
		sbi_console_putstr("exception: ");
		sbi_console_putstr(exception_str(scause & 0x3FF));
	}

	sbi_console_putchar('\n');

	for (;;)
		;
}
