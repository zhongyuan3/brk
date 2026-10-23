#include <arch/mm.h>
#include <brk/clocksource.h>
#include <brk/initcall.h>
#include <brk/kmalloc.h>
#include <brk/memblock.h>
#include <brk/mm.h>
#include <brk/of.h>
#include <brk/panic.h>
#include <brk/printk.h>
#include <brk/types.h>
#include <brk/uart.h>
#include <brk/virtio.h>
#include <libfdt.h>
#include <uapi/errno.h>

static int get_addr_cells(void *fdt, int node, int *addr_cells, int *size_cells)
{
	int ret;

	ret = fdt_address_cells(fdt, node);
	if (ret < 0)
		return ret;
	*addr_cells = ret;

	ret = fdt_size_cells(fdt, node);
	if (ret < 0)
		return ret;
	*size_cells = ret;

	return 0;
}

static int get_parent_addr_cells(void *fdt, int node, int *addr_cells,
				 int *size_cells)
{
	int ret = fdt_parent_offset(fdt, node);
	if (ret < 0)
		return ret;
	return get_addr_cells(fdt, ret, addr_cells, size_cells);
}

static int for_each_reg(void *fdt, int node, int addr_cells, int size_cells,
			int (*callback)(u64 addr, size_t size))
{
	const u32 *reg;
	u64 addr, size;
	int len = 0;
	int reg_bytes = (addr_cells + size_cells) * sizeof(u32);
	int nr_regs;
	int err;

	reg = fdt_getprop(fdt, node, "reg", &len);

	if (!reg)
		return -EINVAL;

	if (len < reg_bytes || len % reg_bytes != 0)
		return -EINVAL;

	nr_regs = len / reg_bytes;
	for (int i = 0; i < nr_regs; ++i) {
		addr = 0;
		size = 0;

		for (int j = 0; j < addr_cells; ++j)
			addr = (addr << 32) | fdt32_to_cpu(*reg++);

		for (int j = 0; j < size_cells; ++j)
			size = (size << 32) | fdt32_to_cpu(*reg++);

		err = callback(addr, size);
		if (err)
			return err;
	}

	return 0;
}

int dtb_early_init_scan_mem(unsigned long dtb_addr)
{
	int err;
	int node;
	int addr_cells, size_cells;
	void *fdt = (void *)dtb_addr;

	node = fdt_path_offset(fdt, "/memory");
	if (node < 0)
		return -EINVAL;

	err = get_parent_addr_cells(fdt, node, &addr_cells, &size_cells);
	if (err)
		return err;

	err = for_each_reg(fdt, node, addr_cells, size_cells, memblock_add);
	if (err)
		return err;

	return 0;
}

int dtb_early_init_scan_reserved_mem(unsigned long dtb_addr)
{
	int err;
	int node, subnode;
	int addr_cells, size_cells;
	void *fdt = (void *)dtb_addr;

	node = fdt_path_offset(fdt, "/reserved-memory");
	if (node < 0)
		return 0;

	err = get_addr_cells(fdt, node, &addr_cells, &size_cells);
	if (err)
		return err;

	fdt_for_each_subnode(subnode, fdt, node) {
		err = for_each_reg(fdt, subnode, addr_cells, size_cells,
				   memblock_reserve);
		if (err)
			return err;
	}

	return 0;
}

int dtb_init_scan_cpu(unsigned long dtb_addr)
{
	int node;
	u32 timebase_freq = 0;
	const u32 *freq;
	int len;
	void *fdt = (void *)dtb_addr;

	node = fdt_path_offset(fdt, "/cpus");
	if (node < 0)
		return -EINVAL;

	freq = fdt_getprop(fdt, node, "timebase-frequency", &len);
	if (!freq || len != sizeof(u32))
		return -EINVAL;

	timebase_freq = fdt32_to_cpu(*freq);
	clocksource_set_timebase_freq(timebase_freq);

	return 0;
}

static int dtb_get_one_reg(void *fdt, int node, u64 *addr, u64 *size)
{
	int len;
	u32 const *reg;
	int addr_cells, size_cells;

	if (get_parent_addr_cells(fdt, node, &addr_cells, &size_cells))
		return -EINVAL;

	int cell_bytes = (addr_cells + size_cells) * sizeof(u32);

	reg = fdt_getprop(fdt, node, "reg", &len);
	if (!reg || len < cell_bytes || len % cell_bytes != 0)
		return -EINVAL;

	for (int i = 0; i < addr_cells; ++i)
		*addr = (*addr << 32) | fdt32_to_cpu(*reg++);
	for (int i = 0; i < size_cells; ++i)
		*size = (*size << 32) | fdt32_to_cpu(*reg++);

	return 0;
}

int dtb_parse_plic(unsigned long dtb_addr, struct plic_device *plic)
{
	int node;
	const u32 *ndev;
	int len;
	u64 addr = 0;
	u64 size = 0;
	void *fdt = (void *)dtb_addr;

	node = fdt_path_offset(fdt, "/soc/plic");
	if (node < 0)
		return -EINVAL;

	if (dtb_get_one_reg(fdt, node, &addr, &size))
		return -EINVAL;

	plic->phys_base = addr;
	plic->size = size;

	ndev = fdt_getprop(fdt, node, "riscv,ndev", &len);
	if (!ndev || len != sizeof(u32))
		return -EINVAL;

	plic->ndev = fdt32_to_cpu(*ndev);

	return 0;
}

static int dtb_get_irq(void *fdt, int node, u32 *irq)
{
	int len;
	const u32 *prop;

	prop = fdt_getprop(fdt, node, "interrupts", &len);
	if (!prop || len != sizeof(u32))
		return -EINVAL;

	*irq = fdt32_to_cpu(*prop);
	return 0;
}

int dtb_parse_uart(unsigned long dtb_addr, struct ns16550a_device *uart)
{
	int node;
	const u32 *clock_freq;
	int len;
	u64 addr = 0;
	u64 size = 0;
	u32 irq = 0;
	void *fdt = (void *)dtb_addr;

	node = fdt_path_offset(fdt, "/soc/serial");
	if (node < 0)
		return -EINVAL;

	if (dtb_get_one_reg(fdt, node, &addr, &size))
		return -EINVAL;

	uart->phys_base = addr;
	uart->size = size;

	if (dtb_get_irq(fdt, node, &irq))
		return -EINVAL;

	uart->irq = irq;

	clock_freq = fdt_getprop(fdt, node, "clock-frequency", &len);
	if (!clock_freq || len != sizeof(u32))
		return -EINVAL;

	uart->clock_freq = fdt32_to_cpu(*clock_freq);

	return 0;
}

int dtb_parse_rtc(unsigned long dtb_addr, struct rtc_device *rtc)
{
	int node;
	u64 addr = 0;
	u64 size = 0;
	u32 irq = 0;
	void *fdt = (void *)dtb_addr;

	node = fdt_node_offset_by_compatible(fdt, -1, "google,goldfish-rtc");
	if (node < 0)
		return node;

	if (dtb_get_one_reg(fdt, node, &addr, &size))
		return -EINVAL;

	if (dtb_get_irq(fdt, node, &irq))
		return -EINVAL;

	rtc->phys_base = addr;
	rtc->size = size;
	rtc->irq = irq;

	return 0;
}

int dtb_init_scan_virtio_dev(unsigned long dtb_addr)
{
	int node;
	u64 addr = 0;
	u64 size = 0;
	u32 irq = 0;
	struct virtio_device *vdev;
	void *fdt = (void *)dtb_addr;

	for (node = fdt_node_offset_by_compatible(fdt, -1, "virtio,mmio");
	     node >= 0;
	     node = fdt_node_offset_by_compatible(fdt, node, "virtio,mmio")) {
		addr = 0;
		size = 0;
		if (dtb_get_one_reg(fdt, node, &addr, &size))
			continue;

		if (dtb_get_irq(fdt, node, &irq))
			continue;

		vdev = virtio_dev_create(addr, size, irq);
		if (!vdev)
			continue;

		virtio_dev_add(vdev);
	}
	return 0;
}

void dtb_init_scan_serial(unsigned long dtb_addr)
{
	int parent;
	const u32 *clock_freq;
	int len;
	u64 addr = 0;
	u64 size = 0;
	u32 irq = 0;
	void *fdt = (void *)dtb_addr;
	int err = 0;
	int subnode;
	struct ns16550a_device *dev;

	parent = fdt_path_offset(fdt, "/soc/");
	if (parent < 0) {
		klog_debug("no /soc/ node found\n");
		return;
	}

	fdt_for_each_subnode(subnode, fdt, parent) {
		klog_debug("subnode: %d, name: %s\n", subnode,
			   fdt_get_name(fdt, subnode, NULL));
		if (fdt_node_check_compatible(fdt, subnode, "ns16550a") == 0) {
			klog_debug("found ns16550a node: %d, name: %s\n",
				   subnode, fdt_get_name(fdt, subnode, NULL));

			err = dtb_get_one_reg(fdt, subnode, &addr, &size);
			if (err) {
				klog_debug("failed to get one reg: %d\n", err);
				continue;
			}
			if (dtb_get_irq(fdt, subnode, &irq)) {
				klog_debug("failed to get irq: %d\n", err);
				continue;
			}
			clock_freq = fdt_getprop(fdt, subnode,
						 "clock-frequency", &len);
			if (!clock_freq || len != sizeof(u32)) {
				klog_debug(
					"failed to get clock frequency: %d\n",
					(int)(unsigned long)clock_freq);
				continue;
			}

			dev = ns16550a_device_alloc();
			if (!dev) {
				klog_debug(
					"failed to allocate ns16550a device\n");
				continue;
			}

			dev->phys_base = addr;
			dev->size = size;
			dev->irq = irq;
			dev->clock_freq = fdt32_to_cpu(*clock_freq);
			err = ns16550a_device_init(dev);
			if (err) {
				klog_debug(
					"failed to initialize ns16550a device: %d\n",
					err);
				ns16550a_device_free(dev);
				continue;
			}
			err = ns16550a_add_device(dev);
			if (err) {
				klog_debug(
					"failed to add ns16550a device: %d\n",
					err);
				ns16550a_device_finalize(dev);
				ns16550a_device_free(dev);
				continue;
			}
			klog_info("ns16550a device added: %d\n", subnode);
		}
	}
}
