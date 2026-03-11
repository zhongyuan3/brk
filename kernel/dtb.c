#include <aosd/cpu.h>
#include <aosd/dtb.h>
#include <aosd/errno.h>
#include <aosd/memblock.h>
#include <aosd/mm.h>
#include <aosd/panic.h>
#include <aosd/printk.h>
#include <aosd/slab.h>
#include <aosd/virtio.h>
#include <libfdt.h>

uint64_t dtb_phys;

static int dtb_get_cells(void *dtb_virt, int node, int *addr_cells,
			 int *size_cells)
{
	int ret;

	ret = fdt_address_cells(dtb_virt, node);
	if (ret < 0)
		return ret;
	*addr_cells = ret;

	ret = fdt_size_cells(dtb_virt, node);
	if (ret < 0)
		return ret;
	*size_cells = ret;

	return 0;
}

static int dtb_get_parent_cells(void *dtb_virt, int node, int *addr_cells,
				int *size_cells)
{
	int ret = fdt_parent_offset(dtb_virt, node);
	if (ret < 0)
		return ret;
	return dtb_get_cells(dtb_virt, ret, addr_cells, size_cells);
}

static int dtb_for_each_reg(void *dtb_virt, int node, int addr_cells,
			    int size_cells,
			    int (*callback)(uint64_t addr, size_t size))
{
	const uint32_t *reg;
	uint64_t addr, size;
	int len = 0;
	int reg_bytes = (addr_cells + size_cells) * sizeof(uint32_t);
	int nr_regs;
	int err;

	reg = fdt_getprop(dtb_virt, node, "reg", &len);

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

int dtb_early_init_scan_mem(void)
{
	int err;
	int node;
	int addr_cells, size_cells;
	void *dtb_virt = (void *)dtb_phys;

	node = fdt_path_offset(dtb_virt, "/memory");
	if (node < 0)
		return -EINVAL;

	err = dtb_get_parent_cells(dtb_virt, node, &addr_cells, &size_cells);
	if (err)
		return err;

	err = dtb_for_each_reg(dtb_virt, node, addr_cells, size_cells,
			       memblock_add);
	if (err)
		return err;

	return 0;
}

int dtb_early_init_scan_reserved_mem(void)
{
	int err;
	int node, subnode;
	int addr_cells, size_cells;
	void *dtb_virt = (void *)dtb_phys;

	node = fdt_path_offset(dtb_virt, "/reserved-memory");
	if (node < 0)
		return 0;

	err = dtb_get_cells(dtb_virt, node, &addr_cells, &size_cells);
	if (err)
		return err;

	fdt_for_each_subnode(subnode, dtb_virt, node) {
		err = dtb_for_each_reg(dtb_virt, subnode, addr_cells,
				       size_cells, memblock_reserve);
		if (err)
			return err;
	}

	return 0;
}

int dtb_init_scan_cpu(void)
{
	int node;
	uint32_t timebase_freq = 0;
	const uint32_t *freq;
	int len;
	void *dtb_virt = (void *)phys_to_virt(dtb_phys);

	node = fdt_path_offset(dtb_virt, "/cpus");
	if (node < 0)
		return -EINVAL;

	freq = fdt_getprop(dtb_virt, node, "timebase-frequency", &len);
	if (!freq || len != sizeof(uint32_t))
		return -EINVAL;

	timebase_freq = fdt32_to_cpu(*freq);
	cpu_set_timebase_freq(timebase_freq);

	return 0;
}

static int dtb_get_one_reg(int node, uint64_t *addr, uint64_t *size)
{
	int len;
	uint32_t const *reg;
	int addr_cells, size_cells;
	void *dtb_virt = (void *)phys_to_virt(dtb_phys);

	if (dtb_get_parent_cells(dtb_virt, node, &addr_cells, &size_cells))
		return -EINVAL;

	int cell_bytes = (addr_cells + size_cells) * sizeof(uint32_t);

	reg = fdt_getprop(dtb_virt, node, "reg", &len);
	if (!reg || len < cell_bytes || len % cell_bytes != 0)
		return -EINVAL;

	for (int i = 0; i < addr_cells; ++i)
		*addr = (*addr << 32) | fdt32_to_cpu(*reg++);
	for (int i = 0; i < size_cells; ++i)
		*size = (*size << 32) | fdt32_to_cpu(*reg++);

	return 0;
}

int dtb_parse_plic(struct plic_device *plic)
{
	int node;
	const uint32_t *ndev;
	int len;
	uint64_t addr = 0;
	uint64_t size = 0;
	void *dtb_virt = (void *)phys_to_virt(dtb_phys);

	node = fdt_path_offset(dtb_virt, "/soc/plic");
	if (node < 0)
		return -EINVAL;

	if (dtb_get_one_reg(node, &addr, &size))
		return -EINVAL;

	plic->phys_base = addr;
	plic->size = size;

	ndev = fdt_getprop(dtb_virt, node, "riscv,ndev", &len);
	if (!ndev || len != sizeof(uint32_t))
		return -EINVAL;

	plic->ndev = fdt32_to_cpu(*ndev);

	return 0;
}

static int dtb_get_irq(int node, uint32_t *irq)
{
	int len;
	const uint32_t *prop;
	void *dtb_virt = (void *)phys_to_virt(dtb_phys);

	prop = fdt_getprop(dtb_virt, node, "interrupts", &len);
	if (!prop || len != sizeof(uint32_t))
		return -EINVAL;

	*irq = fdt32_to_cpu(*prop);
	return 0;
}

int dtb_parse_uart(struct uart_device *uart)
{
	int node;
	const uint32_t *clock_freq;
	int len;
	uint64_t addr = 0;
	uint64_t size = 0;
	uint32_t irq = 0;
	void *dtb_virt = (void *)phys_to_virt(dtb_phys);

	node = fdt_path_offset(dtb_virt, "/soc/serial");
	if (node < 0)
		return -EINVAL;

	if (dtb_get_one_reg(node, &addr, &size))
		return -EINVAL;

	uart->phys_base = addr;
	uart->size = size;

	if (dtb_get_irq(node, &irq))
		return -EINVAL;

	uart->irq = irq;

	clock_freq = fdt_getprop(dtb_virt, node, "clock-frequency", &len);
	if (!clock_freq || len != sizeof(uint32_t))
		return -EINVAL;

	uart->clock_freq = fdt32_to_cpu(*clock_freq);

	return 0;
}

int dtb_init_scan_virtio_dev(void)
{
	int node;
	uint64_t addr = 0;
	uint64_t size = 0;
	uint32_t irq = 0;
	struct virtio_device *vdev;
	void *dtb_virt = (void *)phys_to_virt(dtb_phys);

	for (node = fdt_node_offset_by_compatible(dtb_virt, -1, "virtio,mmio");
	     node >= 0; node = fdt_node_offset_by_compatible(dtb_virt, node,
							     "virtio,mmio")) {
		addr = 0;
		size = 0;
		if (dtb_get_one_reg(node, &addr, &size))
			continue;

		if (dtb_get_irq(node, &irq))
			continue;

		vdev = virtio_dev_create(addr, size, irq);
		if (!vdev)

			continue;

		virtio_dev_add(vdev);
	}
	return 0;
}
