#include <aosd/dtb.h>
#include <aosd/errno.h>
#include <aosd/libfdt.h>
#include <aosd/memblock.h>
#include <aosd/panic.h>
#include <aosd/printk.h>

uint64_t dtb_phys;

static int addr_and_size_cells(void const *fdt, int offset, int *addr_cells,
			       int *size_cells)
{
	int ret;

	if ((ret = fdt_address_cells(fdt, offset)) < 0)
		return ret;
	*addr_cells = ret;

	if ((ret = fdt_size_cells(fdt, offset)) < 0)
		return ret;
	*size_cells = ret;

	return 0;
}

static int parent_addr_and_size_cells(void const *fdt, int offset,
				      int *addr_cells, int *size_cells)
{
	int ret;
	int parent;

	ret = fdt_parent_offset(fdt, offset);
	if (ret < 0)
		return ret;
	parent = ret;

	return addr_and_size_cells(fdt, parent, addr_cells, size_cells);
}

int dtb_early_init_scan_mem(void)
{
	int offset;
	int len;
	int addr_cells;
	int size_cells;
	uint32_t const *reg;
	void const *fdt = (void *)dtb_phys;
	int err;

	offset = fdt_path_offset(fdt, "/memory");
	if (offset < 0)
		return -EINVAL;

	err = parent_addr_and_size_cells(fdt, offset, &addr_cells, &size_cells);
	if (err)
		return err;

	reg = fdt_getprop(fdt, offset, "reg", &len);
	if (!reg)
		return -EINVAL;

	int reg_bytes = (addr_cells + size_cells) * sizeof(uint32_t);
	if (len < reg_bytes || len % reg_bytes != 0)
		return -EINVAL;

	int nr_regs = len / reg_bytes;
	for (int i = 0; i < nr_regs; ++i) {
		uint64_t addr = 0;
		uint64_t size = 0;

		for (int j = 0; j < addr_cells; ++j)
			addr = (addr << 32) | fdt32_to_cpu(*reg++);

		for (int j = 0; j < size_cells; ++j)
			size = (size << 32) | fdt32_to_cpu(*reg++);

		err = memblock_add(addr, size);
		if (err)
			return err;
	}

	return 0;
}

int dtb_early_init_scan_reserved_mem(void)
{
	int offset;
	int len;
	int addr_cells;
	int size_cells;
	uint32_t const *reg;
	void const *fdt = (void *)dtb_phys;
	int err;

	offset = fdt_path_offset(fdt, "/reserved-memory");
	if (offset < 0)
		return 0;

	err = addr_and_size_cells(fdt, offset, &addr_cells, &size_cells);
	if (err)
		return err;

	int suboffset;
	int reg_bytes = (addr_cells + size_cells) * sizeof(uint32_t);
	fdt_for_each_subnode(suboffset, fdt, offset) {
		reg = fdt_getprop(fdt, suboffset, "reg", &len);
		if (!reg)
			return -EINVAL;
		if (len < reg_bytes || len % reg_bytes != 0)
			return -EINVAL;

		int nr_regs = len / reg_bytes;
		for (int i = 0; i < nr_regs; ++i) {
			uint64_t addr = 0;
			uint64_t size = 0;

			for (int j = 0; j < addr_cells; ++j)
				addr = (addr << 32) | fdt32_to_cpu(*reg++);

			for (int j = 0; j < size_cells; ++j)
				size = (size << 32) | fdt32_to_cpu(*reg++);

			err = memblock_reserve(addr, size);
			if (err)
				return err;
		}
	}

	return 0;
}
