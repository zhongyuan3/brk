#define pr_fmt(fmt) "of: " fmt
#include <brk/memblock.h>
#include <brk/printk.h>
#include <brk/types.h>
#include <libfdt.h>
#include <stddef.h>
#include <uapi/errno.h>

#include "of_internal.h"

/*
 * Raw FDT access for the early boot phase.
 *
 * The device tree is unflattened into device_node before the slab allocator
 * comes up.  The two physical-memory scans below run before that tree exists;
 * after of_init(), all device-tree access goes through the of_* node helpers.
 */
static void *fdt;

void of_set_fdt(unsigned long addr)
{
	fdt = (void *)addr;
}

size_t of_get_fdt_size(unsigned long addr)
{
	return fdt_totalsize((const void *)addr);
}

void *of_early_fdt(void)
{
	return fdt;
}

int of_fdt_errno(int err)
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
		return of_fdt_errno(ret);
	*addr_cells = ret;

	ret = fdt_size_cells(fdt, node);
	if (ret < 0)
		return of_fdt_errno(ret);
	*size_cells = ret;

	return 0;
}

static int get_parent_addr_cells(int node, int *addr_cells, int *size_cells)
{
	int ret = fdt_parent_offset(fdt, node);

	if (ret < 0)
		return of_fdt_errno(ret);
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
		return of_fdt_errno(len);
	if (reg_bytes <= 0 || len < reg_bytes || len % reg_bytes != 0)
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

static int of_memblock_add_or_reserve(u64 addr, size_t size, void *add)
{
	if (!add)
		return memblock_reserve(addr, size);
	else
		return memblock_add(addr, size);
}

int of_scan_memory(void)
{
	int err;
	int node;
	int addr_cells, size_cells;

	node = fdt_path_offset(fdt, "/memory");
	if (node < 0)
		return of_fdt_errno(node);

	err = get_parent_addr_cells(node, &addr_cells, &size_cells);
	if (err)
		return err;

	return for_each_reg(node, addr_cells, size_cells,
			    of_memblock_add_or_reserve, (void *)1);
}

int of_scan_reserved_memory(void)
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
				   of_memblock_add_or_reserve, (void *)0);
		if (err)
			return err;
	}
	if (subnode != -FDT_ERR_NOTFOUND)
		return of_fdt_errno(subnode);

	return 0;
}
