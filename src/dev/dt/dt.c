#define pr_fmt(fmt) "dt: " fmt
#include <arch/hart.h>
#include <arch/mm.h>
#include <brk/assert.h>
#include <brk/clocksource.h>
#include <brk/dt.h>
#include <brk/initcall.h>
#include <brk/ktime.h>
#include <brk/memblock.h>
#include <brk/mm.h>
#include <brk/panic.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/string.h>
#include <brk/types.h>
#include <libfdt.h>
#include <stddef.h>
#include <uapi/errno.h>

static void *fdt;

static int __fdt_err_to_errno(int err)
{
	int code;

	if (err >= 0)
		return err;
	if (err < -FDT_ERR_MAX)
		return -EINVAL;

	code = -err;

	switch (code) {
	case FDT_ERR_NOTFOUND:
		return -ENOENT;
	case FDT_ERR_EXISTS:
		return -EEXIST;
	case FDT_ERR_NOSPACE:
	case FDT_ERR_NOPHANDLES:
		return -ENOSPC;
	case FDT_ERR_INTERNAL:
		return -EIO;
	case FDT_ERR_BADOFFSET:
	case FDT_ERR_BADPATH:
	case FDT_ERR_BADPHANDLE:
	case FDT_ERR_BADSTATE:
	case FDT_ERR_TRUNCATED:
	case FDT_ERR_BADMAGIC:
	case FDT_ERR_BADVERSION:
	case FDT_ERR_BADSTRUCTURE:
	case FDT_ERR_BADLAYOUT:
	case FDT_ERR_BADNCELLS:
	case FDT_ERR_BADVALUE:
	case FDT_ERR_BADOVERLAY:
	case FDT_ERR_BADFLAGS:
	case FDT_ERR_ALIGNMENT:
	default:
		return -EINVAL;
	}
}

static int get_addr_cells(int node, int *addr_cells, int *size_cells)
{
	int ret;

	ret = fdt_address_cells(fdt, node);
	if (ret < 0)
		return __fdt_err_to_errno(ret);
	*addr_cells = ret;

	ret = fdt_size_cells(fdt, node);
	if (ret < 0)
		return __fdt_err_to_errno(ret);
	*size_cells = ret;

	return 0;
}

static int get_parent_addr_cells(int node, int *addr_cells, int *size_cells)
{
	int ret = fdt_parent_offset(fdt, node);
	if (ret < 0)
		return __fdt_err_to_errno(ret);
	return get_addr_cells(ret, addr_cells, size_cells);
}

static int for_each_reg(int node, int addr_cells, int size_cells,
			int (*fn)(u64 addr, size_t size, void *arg), void *arg)
{
	const u32 *reg;
	u64 addr, size;
	int len = 0;
	int reg_bytes = (addr_cells + size_cells) * sizeof(u32);
	int err;

	reg = fdt_getprop(fdt, node, "reg", &len);
	if (!reg)
		return __fdt_err_to_errno(len);
	if (len < reg_bytes || len % reg_bytes != 0)
		return -EINVAL;

	for (int i = 0, nr = len / reg_bytes; i < nr; ++i) {
		addr = 0;
		size = 0;

		for (int j = 0; j < addr_cells; ++j)
			addr = (addr << 32) | fdt32_to_cpu(*reg++);

		for (int j = 0; j < size_cells; ++j)
			size = (size << 32) | fdt32_to_cpu(*reg++);

		err = fn(addr, size, arg);
		if (err)
			return err;
	}

	return 0;
}

void dt_set_fdt(unsigned long addr)
{
	fdt = (void *)addr;
}

static int __memblock_add_or_reserve(u64 addr, size_t size, void *add)
{
	if (!add)
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
		return __fdt_err_to_errno(node);

	err = get_parent_addr_cells(node, &addr_cells, &size_cells);
	if (err)
		return err;

	err = for_each_reg(node, addr_cells, size_cells,
			   __memblock_add_or_reserve, (void *)1);
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
	if (node < 0) {
		pr_info("no reserved-memory node in the DT\n");
		return 0;
	}

	err = get_addr_cells(node, &addr_cells, &size_cells);
	if (err)
		return err;

	fdt_for_each_subnode(subnode, fdt, node) {
		err = for_each_reg(subnode, addr_cells, size_cells,
				   __memblock_add_or_reserve, (void *)0);
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
	if (node < 0) {
		pr_err("no cpus node in the DT\n");
		return __fdt_err_to_errno(node);
	}

	freq = fdt_getprop(fdt, node, "timebase-frequency", &len);
	if (!freq)
		return __fdt_err_to_errno(len);
	if (len != sizeof(u32))
		return -EINVAL;

	timebase_freq = fdt32_to_cpu(*freq);
	clocksource_set_timebase_freq(timebase_freq);

	return 0;
}

int dt_init_scan_cpu(void)
{
	int node, subnode;
	int len;
	const char *type;

	node = fdt_path_offset(fdt, "/cpus");
	if (node < 0)
		return __fdt_err_to_errno(node);

	fdt_for_each_subnode(subnode, fdt, node) {
		len = 0;
		type = fdt_getprop(fdt, subnode, "device_type", &len);
		if (type && !strncmp(type, "cpu", len))
			nr_harts += 1;
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
	int err;

	if (!addr || !size || index < 0)
		return -EINVAL;

	err = get_parent_addr_cells(node, &addr_cells, &size_cells);
	if (err)
		return err;

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
