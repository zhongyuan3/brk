#include <arch/csr.h>
#include <arch/mm.h>
#include <arch/pgtable.h>

void arch_mm_activate(pgde_t *pgd)
{
	switch_pgtable(pgd);
	write_sstatus(read_sstatus() | SSTATUS_SUM);
}

void arch_mm_switch(pgde_t *pgd)
{
	switch_pgtable(pgd);
}
