#include <arch/pgtable.h>
#include <brk/dt.h>
#include <brk/ioremap.h>
#include <brk/irq.h>
#include <brk/mm.h>
#include <brk/panic.h>
#include <brk/plic.h>
#include <brk/sifive_plic.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>

static struct plic_device plic;

int irqchip_init(void)
{
	dt_parse_plic(&plic);
	sifive_plic_init(&plic);
	irq_register_chip(plic.chip, &plic);
	return 0;
}
