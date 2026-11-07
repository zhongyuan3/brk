#include <brk/initcall.h>
#include <brk/kmalloc.h>
#include <brk/of.h>
#include <brk/platform.h>
#include <brk/printk.h>
#include <brk/string.h>
#include <uapi/errno.h>

#define PLATFORM_MAX_RESOURCES 16

static bool of_platform_skip(const char *name)
{
	return !strcmp(name, "cpus") || !strcmp(name, "memory") ||
	       !strcmp(name, "chosen") || !strcmp(name, "aliases") ||
	       !strcmp(name, "reserved-memory") || !strncmp(name, "__", 2);
}

static int of_platform_device_create(struct device_node *node)
{
	struct platform_device *pdev;
	struct resource *res;
	unsigned int nr = 0;
	const char *compatible;
	int len;
	int err;

	pdev = kzalloc(sizeof(*pdev));
	if (!pdev)
		return -ENOMEM;

	res = kcalloc(PLATFORM_MAX_RESOURCES, sizeof(*res));
	if (!res) {
		kfree(pdev);
		return -ENOMEM;
	}

	for (int i = 0; i < PLATFORM_MAX_RESOURCES; i++) {
		struct resource tmp;

		if (of_address_to_resource(node, i, &tmp) != 0)
			break;
		res[nr++] = tmp;
	}

	for (int i = 0; i < PLATFORM_MAX_RESOURCES; i++) {
		int irq = of_irq_get(node, i);

		if (irq < 0)
			break;
		res[nr].start = (u64)irq;
		res[nr].end = (u64)irq;
		res[nr].flags = IORESOURCE_IRQ;
		res[nr].name = of_node_name(node);
		nr++;
	}

	compatible = of_get_property(node, "compatible", &len);
	device_init(&pdev->dev, of_node_name(node));
	pdev->dev.bus = &platform_bus;
	pdev->dev.of_node = node;
	pdev->dev.release = platform_device_release;
	pdev->compatible = compatible && len > 0 ? compatible : NULL;
	pdev->resources = res;
	pdev->num_resources = nr;

	err = device_register(&pdev->dev);
	if (err) {
		pr_warn("of: cannot register %s: %d\n", of_node_full_name(node),
			err);
		device_put(&pdev->dev);
		return err;
	}

	pr_debug("of: platform device %s (%u resources)\n",
		 of_node_full_name(node), nr);
	return 0;
}

static u64 of_platform_node_address(const struct device_node *node)
{
	struct resource res;

	if (of_address_to_resource(node, 0, &res))
		return 0;
	return res.start;
}

static int of_platform_node_cmp(const struct device_node *a,
				const struct device_node *b)
{
	u64 a_addr = of_platform_node_address(a);
	u64 b_addr = of_platform_node_address(b);

	if (a_addr < b_addr)
		return -1;
	if (a_addr > b_addr)
		return 1;
	return strcmp(of_node_full_name(a), of_node_full_name(b));
}

/* Sort children by first MMIO address, then by full path. */
static void of_platform_sort_nodes(struct device_node **nodes, size_t count)
{
	size_t i, j;

	for (i = 1; i < count; i++) {
		struct device_node *key = nodes[i];

		j = i;
		while (j > 0 && of_platform_node_cmp(nodes[j - 1], key) > 0) {
			nodes[j] = nodes[j - 1];
			j--;
		}
		nodes[j] = key;
	}
}

static int of_platform_populate_children(struct device_node *parent)
{
	struct device_node *node;
	struct device_node **nodes;
	size_t count = 0;
	size_t i;
	int err = 0;

	of_for_each_child(node, parent) count++;

	if (!count)
		return 0;

	nodes = kcalloc(count, sizeof(*nodes));
	if (!nodes)
		return -ENOMEM;

	i = 0;
	of_for_each_child(node, parent)
	{
		if (i < count)
			nodes[i++] = node;
	}

	of_platform_sort_nodes(nodes, i);

	for (size_t n = 0; n < i; n++) {
		int ret;

		node = nodes[n];
		if (!of_device_is_available(node))
			continue;

		if (of_platform_skip(node->name))
			continue;

		if (of_get_property(node, "compatible", NULL) &&
		    !of_device_is_compatible(node, "simple-bus")) {
			ret = of_platform_device_create(node);

			if (ret && ret != -EEXIST)
				err = ret;
		}

		ret = of_platform_populate_children(node);

		if (ret && !err)
			err = ret;
	}

	kfree(nodes);
	return err;
}

int of_platform_populate(void)
{
	struct device_node *root = of_root();

	if (!root)
		return -ENODEV;
	return of_platform_populate_children(root);
}

static void of_platform_populate_init(void)
{
	int err = of_platform_populate();

	if (err)
		pr_warn("of: platform populate failed: %d\n", err);
}
subsys_initcall(of_platform_populate_init);
