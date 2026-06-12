#include <arch/csr.h>
#include <arch/mm.h>
#include <arch/pgtable.h>

void user_access_enable(void)
{
	write_sstatus(read_sstatus() | SSTATUS_SUM);
}

void user_access_disable(void)
{
	write_sstatus(read_sstatus() & ~SSTATUS_SUM);
}
