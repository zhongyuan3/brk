#include <brk/initcall.h>
#include <brk/irq.h>
#include <brk/irq/irqchip.h>
#include <brk/irq/irqdesc.h>
#include <brk/irq/irqdomain.h>
#include <brk/printk.h>
#include <brk/types.h>
#include <uapi/errno.h>

struct irq_test_ctx {
	unsigned int calls;
};

static unsigned int mask_count;
static unsigned int unmask_count;
static unsigned int ack_count;
static unsigned int eoi_count;
static unsigned int type_count;
static unsigned int affinity_count;
static unsigned int failures;

#define CHECK(cond, name)                                          \
	do {                                                       \
		if (!(cond)) {                                     \
			pr_err("irq-selftest: %s failed\n", name); \
			failures++;                                \
		}                                                  \
	} while (0)

static void test_irq_mask(struct irq_data *d)
{
	(void)d;
	mask_count++;
}

static void test_irq_unmask(struct irq_data *d)
{
	(void)d;
	unmask_count++;
}

static void test_irq_ack(struct irq_data *d)
{
	(void)d;
	ack_count++;
}

static void test_irq_eoi(struct irq_data *d)
{
	(void)d;
	eoi_count++;
}

static int test_irq_set_type(struct irq_data *d, unsigned int type)
{
	(void)d;
	(void)type;
	type_count++;
	return 0;
}

static int test_irq_set_affinity(struct irq_data *d, unsigned int hart)
{
	(void)d;
	(void)hart;
	affinity_count++;
	return 0;
}

static struct irq_chip test_chip = {
	.name = "irq-test",
	.irq_mask = test_irq_mask,
	.irq_unmask = test_irq_unmask,
	.irq_ack = test_irq_ack,
	.irq_eoi = test_irq_eoi,
	.irq_set_type = test_irq_set_type,
	.irq_set_affinity = test_irq_set_affinity,
};

static const struct irq_domain_ops test_domain_ops = { 0 };

static irqreturn_t test_handler_a(int irq, void *dev_id)
{
	struct irq_test_ctx *ctx = dev_id;

	(void)irq;
	ctx->calls++;
	return IRQ_HANDLED;
}

static irqreturn_t test_handler_b(int irq, void *dev_id)
{
	struct irq_test_ctx *ctx = dev_id;

	(void)irq;
	ctx->calls++;
	return IRQ_HANDLED;
}

static void irq_selftest(void)
{
	struct irq_test_ctx a = { 0 };
	struct irq_test_ctx b = { 0 };
	struct irq_domain *domain;
	unsigned int irq;
	int err;

	pr_info("irq-selftest: starting\n");

	domain = irq_domain_add_linear(-1, 8, &test_domain_ops, NULL);
	if (!domain) {
		pr_err("irq-selftest: cannot allocate test domain\n");
		return;
	}

	irq = irq_create_mapping(domain, 1, IRQ_TYPE_LEVEL_HIGH);
	if (!irq) {
		pr_err("irq-selftest: cannot create test mapping\n");
		irq_domain_remove(domain);
		return;
	}

	irq_set_chip_and_handler(irq, &test_chip, handle_simple_irq);
	irq_set_chip_data(irq, NULL);

	/* Shared IRQ: both actions must run. */
	err = request_irq(irq, test_handler_a, IRQF_SHARED, "test-a", &a);
	CHECK(err == 0, "request shared a");
	err = request_irq(irq, test_handler_b, IRQF_SHARED, "test-b", &b);
	CHECK(err == 0, "request shared b");
	CHECK(unmask_count == 1, "auto-enable unmask");
	CHECK(mask_count == 0, "initial mask");

	err = request_irq(irq, test_handler_b, 0, "test-exclusive", &b);
	CHECK(err == -EBUSY, "exclusive busy");

	generic_handle_irq(irq);
	CHECK(a.calls == 1 && b.calls == 1, "shared dispatch");

	free_irq(irq, &a);
	generic_handle_irq(irq);
	CHECK(a.calls == 1 && b.calls == 2, "free shared a");

	free_irq(irq, &b);
	generic_handle_irq(irq);
	CHECK(a.calls == 1 && b.calls == 2, "free shared b");
	CHECK(mask_count == 1, "last free masks");

	/* Level flow: mask -> ack -> handler -> unmask. */
	mask_count = ack_count = unmask_count = 0;
	irq_set_chip_and_handler(irq, &test_chip, handle_level_irq);
	err = request_irq(irq, test_handler_a, 0, "test-level", &a);
	CHECK(err == 0, "request level");
	mask_count = ack_count = unmask_count = 0;
	a.calls = 0;
	generic_handle_irq(irq);
	CHECK(a.calls == 1, "level handler");
	CHECK(mask_count == 1 && ack_count == 1 && unmask_count == 1,
	      "level flow");
	free_irq(irq, &a);

	/* Edge flow: ack -> handler -> eoi, no mask/unmask. */
	mask_count = ack_count = unmask_count = eoi_count = 0;
	irq_set_chip_and_handler(irq, &test_chip, handle_edge_irq);
	err = request_irq(irq, test_handler_a, 0, "test-edge", &a);
	CHECK(err == 0, "request edge");
	mask_count = ack_count = unmask_count = eoi_count = 0;
	a.calls = 0;
	generic_handle_irq(irq);
	CHECK(a.calls == 1, "edge handler");
	CHECK(ack_count == 1 && eoi_count == 1, "edge flow");
	CHECK(mask_count == 0 && unmask_count == 0, "edge no mask");
	free_irq(irq, &a);

	/* Type and affinity callbacks. */
	type_count = affinity_count = 0;
	CHECK(irq_set_type(irq, IRQ_TYPE_EDGE_RISING) == 0 && type_count == 1,
	      "set type");
	CHECK(irq_set_affinity(irq, 0) == 0 && affinity_count == 1,
	      "set affinity");

	/* disable/enable nesting. */
	mask_count = unmask_count = 0;
	disable_irq(irq);
	disable_irq(irq);
	CHECK(mask_count == 0, "nested disable no mask");
	enable_irq(irq);
	enable_irq(irq);
	CHECK(unmask_count == 0, "nested enable no unmask");
	enable_irq(irq);
	CHECK(unmask_count == 1, "final enable unmasks");
	disable_irq(irq);
	CHECK(mask_count == 1, "final disable masks");

	irq_free_desc(irq);
	irq_domain_remove(domain);

	if (failures)
		pr_err("irq-selftest: %u failure(s)\n", failures);
	else
		pr_info("irq-selftest: all tests passed\n");
}
postcore_initcall(irq_selftest);
