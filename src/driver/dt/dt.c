#include <arch/hart.h>
#include <arch/mm.h>
#include <brk/assert.h>
#include <brk/clocksource.h>
#include <brk/device.h>
#include <brk/dt.h>
#include <brk/initcall.h>
#include <brk/irq/irqdomain.h>
#include <brk/kmalloc.h>
#include <brk/ktime.h>
#include <brk/memblock.h>
#include <brk/mm.h>
#include <brk/platform.h>
#include <brk/panic.h>
#include <brk/printk.h>
#include <brk/printf.h>
#include <brk/stddef.h>
#include <brk/string.h>
#include <brk/types.h>
#include <brk/uart.h>
#include <brk/virtio.h>
#include <libfdt.h>
#include <uapi/errno.h>

static void *fdt;

static int get_addr_cells(int node, int *addr_cells, int *size_cells)
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

static int get_parent_addr_cells(int node, int *addr_cells, int *size_cells)
{
	int ret = fdt_parent_offset(fdt, node);
	if (ret < 0)
		return ret;
	return get_addr_cells(ret, addr_cells, size_cells);
}

static int for_each_node_reg(int node, int addr_cells, int size_cells,
			     int (*callback)(u64 addr, size_t size, void *arg),
			     void *arg)
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

		err = callback(addr, size, arg);
		if (err)
			return err;
	}

	return 0;
}

void dt_set_dtb(unsigned long dtb)
{
	fdt = (void *)dtb;
}

static int __memblock_add_reserve(u64 addr, size_t size, void *add)
{
	if (add == NULL)
		return memblock_reserve(addr, size);
	else
		return memblock_add(addr, size);
}

int dt_init_scan_mem(void)
{
	int err;
	int node;
	int addr_cells, size_cells;

	node = fdt_path_offset(fdt, "/memory");
	if (node < 0)
		return -EINVAL;

	err = get_parent_addr_cells(node, &addr_cells, &size_cells);
	if (err)
		return err;

	err = for_each_node_reg(node, addr_cells, size_cells,
				__memblock_add_reserve, (void *)1);
	if (err)
		return err;

	return 0;
}

int dt_init_scan_rsvd_mem(void)
{
	int err;
	int node, subnode;
	int addr_cells, size_cells;

	node = fdt_path_offset(fdt, "/reserved-memory");
	if (node < 0)
		return 0;

	err = get_addr_cells(node, &addr_cells, &size_cells);
	if (err)
		return err;

	fdt_for_each_subnode(subnode, fdt, node) {
		err = for_each_node_reg(subnode, addr_cells, size_cells,
					__memblock_add_reserve, (void *)0);
		if (err)
			return err;
	}

	return 0;
}

int dt_setup_timebase_freq(void)
{
	int node;
	u32 timebase_freq = 0;
	const u32 *freq;
	int len;

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

int dt_init_scan_cpu(void)
{
	int node, subnode;

	node = fdt_path_offset(fdt, "/cpus");
	if (node < 0)
		return -EINVAL;

	fdt_for_each_subnode(subnode, fdt, node) {
		const char *type = fdt_getprop(fdt, subnode, "device_type", 0);
		if (type && !strcmp(type, "cpu")) {
			pr_debug("dtb: found cpu: %s\n",
				 fdt_get_name(fdt, subnode, 0));
			nr_harts += 1;
		}
	}

	return 0;
}

void *dt_get_fdt(void)
{
	return fdt;
}

int dt_get_reg(int node, int index, u64 *addr, u64 *size)
{
	int len, addr_cells, size_cells, cell_bytes, nr;
	const u32 *reg;

	if (!addr || !size || index < 0)
		return -EINVAL;

	if (get_parent_addr_cells(node, &addr_cells, &size_cells))
		return -EINVAL;

	cell_bytes = (addr_cells + size_cells) * sizeof(u32);
	reg = fdt_getprop(fdt, node, "reg", &len);
	if (!reg || len < cell_bytes || len % cell_bytes != 0)
		return -EINVAL;

	nr = len / cell_bytes;
	if (index >= nr)
		return -EINVAL;

	reg += index * (addr_cells + size_cells);
	*addr = 0;
	*size = 0;

	for (int i = 0; i < addr_cells; ++i)
		*addr = (*addr << 32) | fdt32_to_cpu(*reg++);
	for (int i = 0; i < size_cells; ++i)
		*size = (*size << 32) | fdt32_to_cpu(*reg++);

	return 0;
}

static int dtb_get_one_reg(int node, u64 *addr, u64 *size)
{
	return dt_get_reg(node, 0, addr, size);
}

int dt_parse_rtc(struct rtc_device *rtc)
{
	int node;
	u64 addr = 0;
	u64 size = 0;

	node = fdt_node_offset_by_compatible(fdt, -1, "google,goldfish-rtc");
	if (node < 0)
		return node;

	if (dt_get_reg(node, 0, &addr, &size))
		return -EINVAL;

	rtc->phys_base = addr;
	rtc->size = size;

	return 0;
}

int dt_init_scan_virtio_dev(void)
{
	static bool scanned;
	struct virtio_device **vdevs;
	int node;
	int count = 0;
	int i = 0;
	u64 addr = 0;
	u64 size = 0;
	u32 irq = 0;

	/* Several drivers may call this during their initcall. */
	if (scanned)
		return 0;

	for (node = fdt_node_offset_by_compatible(fdt, -1, "virtio,mmio");
	     node >= 0;
	     node = fdt_node_offset_by_compatible(fdt, node, "virtio,mmio"))
		count++;

	if (!count) {
		scanned = true;
		return 0;
	}

	vdevs = kcalloc(count, sizeof(*vdevs));
	if (!vdevs)
		return -ENOMEM;

	for (node = fdt_node_offset_by_compatible(fdt, -1, "virtio,mmio");
	     node >= 0;
	     node = fdt_node_offset_by_compatible(fdt, node, "virtio,mmio")) {
		struct virtio_device *vdev;

		addr = 0;
		size = 0;
		irq = 0;
		if (dtb_get_one_reg(node, &addr, &size))
			continue;

		irq = irq_of_parse_and_map(node, 0);
		if (!irq)
			continue;

		vdev = virtio_dev_create(addr, size, irq);
		if (!vdev)
			continue;

		vdevs[i++] = vdev;
	}

	/*
	 * The QEMU virt machine numbers virtio-mmio buses from the highest
	 * MMIO address down.  DT nodes are walked in the opposite order, so
	 * register them in reverse to keep virtio_blk0 on bus 0.
	 */
	while (i > 0) {
		--i;
		if (virtio_dev_add(vdevs[i]))
			virtio_dev_destroy(vdevs[i]);
	}

	kfree(vdevs);
	scanned = true;
	return 0;
}

void dt_init_scan_serial(void)
{
	int parent;
	const u32 *clock_freq;
	int len;
	u64 addr = 0;
	u64 size = 0;
	u32 irq = 0;

	int err = 0;
	int subnode;
	struct ns16550a_platform_data *pdata;
	char name[DEVICE_NAME_LEN];

	parent = fdt_path_offset(fdt, "/soc/");
	if (parent < 0) {
		pr_debug("no /soc/ node found\n");
		return;
	}

	fdt_for_each_subnode(subnode, fdt, parent) {
		pr_debug("subnode: %d, name: %s\n", subnode,
			 fdt_get_name(fdt, subnode, NULL));
		if (fdt_node_check_compatible(fdt, subnode, "ns16550a") == 0) {
			pr_debug("found ns16550a node: %d, name: %s\n", subnode,
				 fdt_get_name(fdt, subnode, NULL));

			err = dtb_get_one_reg(subnode, &addr, &size);
			if (err) {
				pr_debug("failed to get one reg: %d\n", err);
				continue;
			}
			irq = irq_of_parse_and_map(subnode, 0);
			if (!irq) {
				pr_debug("failed to map irq\n");
				continue;
			}
			clock_freq = fdt_getprop(fdt, subnode,
						 "clock-frequency", &len);
			if (!clock_freq || len != sizeof(u32)) {
				pr_debug("failed to get clock frequency: %d\n",
					 (int)(unsigned long)clock_freq);
				continue;
			}

			pdata = kzalloc(sizeof(*pdata));
			if (!pdata)
				continue;
			pdata->clock_freq = fdt32_to_cpu(*clock_freq);

			snprintf(name, sizeof(name), "ns16550a@%lx", addr);
			err = platform_device_register_simple(
				name, "ns16550a", addr, size, irq, pdata);
			if (err) {
				kfree(pdata);
				pr_debug("failed to register ns16550a device: %d\n",
					 err);
				continue;
			}
			pr_info("ns16550a platform device registered: %s\n",
				name);
		}
	}
}
subsys_initcall(dt_init_scan_serial);
