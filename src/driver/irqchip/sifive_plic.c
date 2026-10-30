#include <arch/hart.h>
#include <arch/pgtable.h>
#include <brk/dt.h>
#include <brk/initcall.h>
#include <brk/ioremap.h>
#include <brk/irq.h>
#include <brk/irq/irqchip.h>
#include <brk/irq/irqdesc.h>
#include <brk/irq/irqdomain.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/mmio.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <libfdt.h>
#include <uapi/errno.h>

#define PLIC_PRIORITY_OFFSET 0x000000
#define PLIC_ENABLE_OFFSET 0x002000
#define PLIC_THRESHOLD_OFFSET 0x200000
#define PLIC_CLAIM_COMPLETE_OFFSET 0x200004

struct sifive_plic {
	u8 *base;
	u32 ndev;
	struct irq_domain *domain;
	struct irq_controller controller;
};

static inline u32 plic_ctx(unsigned int hart)
{
	return 2 * hart + 1;
}

static inline volatile u32 *plic_enable_reg(struct sifive_plic *p,
					    unsigned int hart)
{
	return (volatile u32 *)(p->base + PLIC_ENABLE_OFFSET +
				plic_ctx(hart) * 0x80);
}

static inline volatile u32 *plic_threshold_reg(struct sifive_plic *p,
					       unsigned int hart)
{
	return (volatile u32 *)(p->base + PLIC_THRESHOLD_OFFSET +
				plic_ctx(hart) * 0x1000);
}

static inline volatile u32 *plic_claim_reg(struct sifive_plic *p,
					   unsigned int hart)
{
	return (volatile u32 *)(p->base + PLIC_CLAIM_COMPLETE_OFFSET +
				plic_ctx(hart) * 0x1000);
}

static void plic_set_priority(struct sifive_plic *p, u32 hwirq, u32 priority)
{
	((volatile u32 *)(p->base + PLIC_PRIORITY_OFFSET))[hwirq] = priority;
}

static void plic_irq_mask(struct irq_data *d)
{
	struct sifive_plic *p = d->chip_data;
	volatile u32 *en;

	if (!p || d->hwirq >= p->ndev)
		return;

	en = plic_enable_reg(p, d->hart);
	en[d->hwirq / 32] &= ~(1u << (d->hwirq % 32));
}

static void plic_irq_unmask(struct irq_data *d)
{
	struct sifive_plic *p = d->chip_data;
	volatile u32 *en;

	if (!p || d->hwirq >= p->ndev)
		return;

	en = plic_enable_reg(p, d->hart);
	en[d->hwirq / 32] |= 1u << (d->hwirq % 32);
}

static int plic_irq_set_affinity(struct irq_data *d, unsigned int hart)
{
	plic_irq_mask(d);
	d->hart = hart;
	plic_irq_unmask(d);
	return 0;
}

static struct irq_chip plic_chip = {
	.name = "sifive-plic",
	.irq_mask = plic_irq_mask,
	.irq_unmask = plic_irq_unmask,
	.irq_set_affinity = plic_irq_set_affinity,
};

static int plic_domain_xlate(struct irq_domain *d, int fdt_node,
			     const u32 *intspec, unsigned int intsize,
			     irq_hw_number_t *out_hwirq, unsigned int *out_type)
{
	(void)d;
	(void)fdt_node;

	if (intsize < 1)
		return -EINVAL;

	*out_hwirq = intspec[0];
	*out_type = intsize >= 2 ? intspec[1] : IRQ_TYPE_LEVEL_HIGH;
	return 0;
}

static int plic_domain_map(struct irq_domain *d, unsigned int virq,
			   irq_hw_number_t hwirq, unsigned int type)
{
	struct sifive_plic *p = d->host_data;

	if (hwirq == 0 || hwirq >= p->ndev)
		return -EINVAL;

	irq_set_chip_and_handler(virq, &plic_chip, handle_simple_irq);
	irq_set_chip_data(virq, p);
	irq_set_type(virq, type);

	/* PLIC priority 0 means "never interrupt". */
	plic_set_priority(p, hwirq, 1);
	return 0;
}

static const struct irq_domain_ops plic_domain_ops = {
	.xlate = plic_domain_xlate,
	.map = plic_domain_map,
};

static int plic_init_hart(struct irq_controller *ctrl, unsigned int hart)
{
	struct sifive_plic *p = ctrl->priv;

	*plic_threshold_reg(p, hart) = 0;
	return 0;
}

static int plic_handle_irq(struct irq_controller *ctrl, unsigned int hart)
{
	struct sifive_plic *p = ctrl->priv;
	volatile u32 *claim = plic_claim_reg(p, hart);
	u32 hwirq = *claim;
	unsigned int virq;

	if (!hwirq) {
		irq_note_spurious();
		return 0;
	}

	virq = irq_find_mapping(ctrl->domain, hwirq);
	if (virq) {
		generic_handle_irq(virq);
	} else {
		irq_note_spurious();
		pr_warn("plic: unmapped hwirq %u\n", hwirq);
	}

	*claim = hwirq;
	return 1;
}

static bool plic_hwirq_enabled(struct sifive_plic *p, unsigned int hart,
			       u32 hwirq)
{
	volatile u32 *en = plic_enable_reg(p, hart);

	return (en[hwirq / 32] & (1u << (hwirq % 32))) != 0;
}

static void plic_hwirq_clear_enable(struct sifive_plic *p, unsigned int hart,
				    u32 hwirq)
{
	volatile u32 *en = plic_enable_reg(p, hart);

	en[hwirq / 32] &= ~(1u << (hwirq % 32));
}

static void plic_selftest(struct sifive_plic *p)
{
	bool h0, h1, h0_after, h1_after;
	u32 test_hwirq = p->ndev - 1;
	unsigned int virq;

	if (p->ndev <= 1)
		return;

	/*
	 * Use the last PLIC source, which no QEMU virt device uses, so the
	 * test never disturbs a real device IRQ.  The mapping is left in
	 * place (it is harmless and shows up as <none> in /proc/interrupts).
	 */
	virq = irq_create_mapping(p->domain, test_hwirq, IRQ_TYPE_LEVEL_HIGH);
	if (!virq) {
		pr_err("plic: affinity selftest: cannot map hwirq %u\n",
		       test_hwirq);
		return;
	}

	irq_set_affinity(virq, 0);
	h0 = plic_hwirq_enabled(p, 0, test_hwirq);
	h1 = nr_harts > 1 ? plic_hwirq_enabled(p, 1, test_hwirq) : false;

	if (nr_harts > 1) {
		irq_set_affinity(virq, 1);
		h0_after = plic_hwirq_enabled(p, 0, test_hwirq);
		h1_after = plic_hwirq_enabled(p, 1, test_hwirq);
		irq_set_affinity(virq, 0);

		if (h0 && !h1 && !h0_after && h1_after)
			pr_info("plic: affinity selftest passed (hart 0 <-> 1)\n");
		else
			pr_err("plic: affinity selftest failed "
			       "(h0=%d h1=%d h0_after=%d h1_after=%d)\n",
			       h0, h1, h0_after, h1_after);
	} else {
		if (h0 && !h1)
			pr_info("plic: single-hart affinity selftest passed\n");
		else
			pr_err("plic: single-hart affinity selftest failed "
			       "(h0=%d h1=%d)\n",
			       h0, h1);
	}

	plic_hwirq_clear_enable(p, 0, test_hwirq);
	if (nr_harts > 1)
		plic_hwirq_clear_enable(p, 1, test_hwirq);
}

static int plic_probe(int fdt_node)
{
	const u32 *ndev;
	struct sifive_plic *p;
	u64 addr = 0, size = 0;
	int len;

	if (dt_get_reg(fdt_node, 0, &addr, &size))
		return -EINVAL;

	p = kzalloc(sizeof(*p));
	if (!p)
		return -ENOMEM;

	p->base = ioremap(addr, size, PTE_R | PTE_W);
	if (!p->base) {
		kfree(p);
		return -ENOMEM;
	}

	ndev = fdt_getprop(dt_get_fdt(), fdt_node, "riscv,ndev", &len);
	p->ndev = ndev && len == sizeof(u32) ? fdt32_to_cpu(*ndev) : 1024;
	if (p->ndev < 2)
		p->ndev = 2;

	p->domain =
		irq_domain_add_linear(fdt_node, p->ndev, &plic_domain_ops, p);
	if (!p->domain) {
		iounmap(p->base, size);
		kfree(p);
		return -ENOMEM;
	}

	p->controller.name = "sifive-plic";
	p->controller.domain = p->domain;
	p->controller.priv = p;
	p->controller.handle_irq = plic_handle_irq;
	p->controller.init_hart = plic_init_hart;
	list_init(&p->controller.list);
	irq_register_controller(&p->controller);

	plic_selftest(p);

	pr_info("plic: %u sources at %#" PRIx64 "\n", p->ndev, addr);
	return 0;
}

static const char *const plic_compatibles[] = {
	"sifive,plic-1.0.0",
	"riscv,plic0",
	NULL,
};

static struct irqchip_driver plic_driver = {
	.name = "sifive-plic",
	.compatibles = plic_compatibles,
	.probe = plic_probe,
};

static void plic_driver_register(void)
{
	irqchip_register_driver(&plic_driver);
}
core_initcall(plic_driver_register);
