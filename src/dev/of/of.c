#include <arch/hart.h>
#include <arch/mm.h>
#include <brk/align.h>
#include <brk/irqdomain.h>
#include <brk/list.h>
#include <brk/memblock.h>
#include <brk/of.h>
#include <brk/printk.h>
#include <brk/string.h>
#include <brk/types.h>
#include <libfdt.h>
#include <stddef.h>
#include <uapi/errno.h>

#include "of_internal.h"

#define OF_PATH_MAX 512
#define OF_MAX_NCELLS 4
#define OF_MEMBLOCK_ALIGN 8
#define OF_VALUE_ALIGN 4

static struct device_node *of_root_node;
static bool of_initialized;

struct of_build_info {
	size_t nr_nodes;
	size_t nr_props;
	size_t text_size;
	size_t value_size;
};

struct of_build_cursor {
	struct device_node *node;
	struct property *prop;
	char *text;
	char *value;
};

static const struct property *
of_find_property_raw(const struct device_node *node, const char *name)
{
	if (!node || !name)
		return NULL;

	for (int i = 0; i < node->nr_properties; ++i) {
		struct property *prop = &node->properties[i];

		if (!strcmp(prop->name, name))
			return prop;
	}
	return NULL;
}

static void *of_memblock_alloc(size_t size)
{
	u64 pa;

	if (!size)
		return NULL;

	pa = memblock_phys_alloc(size, OF_MEMBLOCK_ALIGN);
	if (!pa)
		return NULL;

	return (void *)phys_to_virt(pa);
}

static int of_count_node(const void *fdt, int offset,
			 struct of_build_info *info)
{
	const char *name;
	const char *prop_name;
	const void *value;
	char path[OF_PATH_MAX];
	int name_len;
	int path_len;
	int prop_offset;
	int value_len;
	int child;

	name = fdt_get_name(fdt, offset, &name_len);
	if (!name)
		return name_len;

	info->nr_nodes += 1;
	info->text_size += (size_t)name_len + 1;

	path_len = fdt_get_path(fdt, offset, path, sizeof(path));
	if (path_len < 0)
		info->text_size += (size_t)name_len + 1;
	else
		info->text_size += (size_t)path_len + 1;

	for (prop_offset = fdt_first_property_offset(fdt, offset);
	     prop_offset >= 0;
	     prop_offset = fdt_next_property_offset(fdt, prop_offset)) {
		value = fdt_getprop_by_offset(fdt, prop_offset, &prop_name,
					      &value_len);
		if (!value)
			return value_len;
		if (!prop_name)
			return -FDT_ERR_BADSTRUCTURE;

		info->nr_props += 1;
		info->text_size += strlen(prop_name) + 1;
		info->value_size += align_up((size_t)value_len, OF_VALUE_ALIGN);
	}

	if (prop_offset != -FDT_ERR_NOTFOUND)
		return prop_offset;

	fdt_for_each_subnode(child, fdt, offset) {
		int err = of_count_node(fdt, child, info);

		if (err)
			return err;
	}
	if (child != -FDT_ERR_NOTFOUND)
		return child;

	return 0;
}

static void of_copy_string(char **cursor, const char *src, const char **out)
{
	size_t len = strlen(src) + 1;

	*out = *cursor;
	memcpy(*cursor, src, len);
	*cursor += len;
}

static int of_build_node(const void *fdt, int offset,
			 struct device_node *parent,
			 struct of_build_cursor *cur)
{
	const char *name;
	const char *prop_name;
	const void *value;
	char path[OF_PATH_MAX];
	struct device_node *node = cur->node++;
	int name_len;
	int path_len;
	int prop_offset;
	int value_len;
	int child;

	name = fdt_get_name(fdt, offset, &name_len);
	if (!name)
		return name_len;

	of_copy_string(&cur->text, name, &node->name);

	path_len = fdt_get_path(fdt, offset, path, sizeof(path));
	if (path_len < 0)
		node->full_name = node->name;
	else
		of_copy_string(&cur->text, path, &node->full_name);

	node->phandle = fdt_get_phandle(fdt, offset);
	node->parent = parent;
	node->properties = cur->prop;
	list_init(&node->children);
	list_init(&node->sibling);

	for (prop_offset = fdt_first_property_offset(fdt, offset);
	     prop_offset >= 0;
	     prop_offset = fdt_next_property_offset(fdt, prop_offset)) {
		struct property *prop = cur->prop++;

		value = fdt_getprop_by_offset(fdt, prop_offset, &prop_name,
					      &value_len);
		if (!value)
			return value_len;
		if (!prop_name)
			return -FDT_ERR_BADSTRUCTURE;

		of_copy_string(&cur->text, prop_name, &prop->name);
		prop->length = value_len;

		cur->value = (char *)align_up((unsigned long)cur->value,
					      OF_VALUE_ALIGN);
		prop->value = cur->value;
		if (value_len > 0) {
			memcpy(cur->value, value, (size_t)value_len);
			cur->value += value_len;
		}

		node->nr_properties += 1;
		if (prop->length > 0 && !strcmp(prop->name, "device_type"))
			node->type = prop->value;
	}

	if (prop_offset != -FDT_ERR_NOTFOUND)
		return prop_offset;

	fdt_for_each_subnode(child, fdt, offset) {
		struct device_node *child_node = cur->node;
		int err = of_build_node(fdt, child, node, cur);

		if (err)
			return err;
		list_add_tail(&child_node->sibling, &node->children);
	}
	if (child != -FDT_ERR_NOTFOUND)
		return child;

	return 0;
}

int of_init(void)
{
	struct of_build_info info = { 0 };
	struct of_build_cursor cur = { 0 };
	struct device_node *root;
	void *fdt;
	void *block;
	size_t nodes_size;
	size_t props_size;
	size_t props_offset;
	size_t text_offset;
	size_t value_offset;
	size_t total_size;
	int root_offset;
	int err;

	if (of_initialized)
		return 0;

	fdt = of_early_fdt();
	if (!fdt)
		return -ENODEV;

	root_offset = fdt_path_offset(fdt, "/");
	if (root_offset < 0)
		return of_fdt_errno(root_offset);

	err = of_count_node(fdt, root_offset, &info);
	if (err)
		return of_fdt_errno(err);
	if (!info.nr_nodes)
		return -EINVAL;

	nodes_size = info.nr_nodes * sizeof(struct device_node);
	props_size = info.nr_props * sizeof(struct property);
	props_offset = align_up(nodes_size, OF_MEMBLOCK_ALIGN);
	text_offset = align_up(props_offset + props_size, OF_MEMBLOCK_ALIGN);
	value_offset = align_up(text_offset + info.text_size, OF_VALUE_ALIGN);
	total_size = value_offset + info.value_size;

	block = of_memblock_alloc(total_size);
	if (!block)
		return -ENOMEM;
	memset(block, 0, total_size);

	cur.node = block;
	cur.prop = (struct property *)((char *)block + props_offset);
	cur.text = (char *)block + text_offset;
	cur.value = (char *)block + value_offset;

	err = of_build_node(fdt, root_offset, NULL, &cur);
	if (err)
		return of_fdt_errno(err);

	root = block;
	of_root_node = root;
	of_initialized = true;

	pr_debug("of: %zu nodes, %zu properties\n", info.nr_nodes,
		 info.nr_props);
	return 0;
}

static int of_ensure_init(void)
{
	if (of_initialized)
		return 0;
	return of_init();
}

struct device_node *of_root(void)
{
	if (of_ensure_init())
		return NULL;
	return of_root_node;
}

static bool of_node_name_is(const struct device_node *node, const char *name,
			    size_t len)
{
	return strlen(node->name) == len && !memcmp(node->name, name, len);
}

struct device_node *of_find_node_by_path(const char *path)
{
	struct device_node *node;
	const char *p;

	if (!path || path[0] != '/' || of_ensure_init())
		return NULL;

	node = of_root_node;
	p = path;
	while (*p) {
		const char *start;
		size_t len;
		struct device_node *child;
		bool found = false;

		while (*p == '/')
			p++;
		if (!*p)
			break;

		start = p;
		while (*p && *p != '/')
			p++;
		len = (size_t)(p - start);

		of_for_each_child(child, node)
		{
			if (of_node_name_is(child, start, len)) {
				node = child;
				found = true;
				break;
			}
		}
		if (!found)
			return NULL;
	}

	return node;
}

static struct device_node *of_find_compatible(struct device_node *node,
					      const char *compatible)
{
	struct device_node *child;

	if (of_device_is_compatible(node, compatible))
		return node;

	of_for_each_child(child, node)
	{
		struct device_node *found =
			of_find_compatible(child, compatible);

		if (found)
			return found;
	}
	return NULL;
}

struct device_node *of_find_compatible_node(const char *compatible)
{
	if (!compatible || of_ensure_init())
		return NULL;
	return of_find_compatible(of_root_node, compatible);
}

static struct device_node *of_find_phandle(struct device_node *node,
					   u32 phandle)
{
	struct device_node *child;

	if (node->phandle == phandle)
		return node;

	of_for_each_child(child, node)
	{
		struct device_node *found = of_find_phandle(child, phandle);

		if (found)
			return found;
	}
	return NULL;
}

struct device_node *of_find_node_by_phandle(u32 phandle)
{
	if (!phandle || of_ensure_init())
		return NULL;
	return of_find_phandle(of_root_node, phandle);
}

struct device_node *of_get_parent(const struct device_node *node)
{
	return node ? node->parent : NULL;
}

struct device_node *of_get_child_by_name(const struct device_node *node,
					 const char *name)
{
	struct device_node *child;

	if (!node || !name)
		return NULL;

	of_for_each_child(child, node)
	{
		if (!strcmp(child->name, name))
			return child;
	}
	return NULL;
}

const struct property *of_find_property(const struct device_node *node,
					const char *name, int *lenp)
{
	const struct property *prop;
	int err;

	if (!node || !name) {
		if (lenp)
			*lenp = -EINVAL;
		return NULL;
	}

	err = of_ensure_init();
	if (err) {
		if (lenp)
			*lenp = err;
		return NULL;
	}

	prop = of_find_property_raw(node, name);
	if (!prop) {
		if (lenp)
			*lenp = -ENOENT;
		return NULL;
	}

	if (lenp)
		*lenp = prop->length;
	return prop;
}

const void *of_get_property(const struct device_node *node, const char *name,
			    int *lenp)
{
	const struct property *prop = of_find_property(node, name, lenp);

	return prop ? prop->value : NULL;
}

int of_property_read_u32(const struct device_node *node, const char *name,
			 u32 *out)
{
	const struct property *prop;

	if (!out)
		return -EINVAL;

	prop = of_find_property(node, name, NULL);
	if (!prop || prop->length < (int)sizeof(u32))
		return -EINVAL;

	*out = of_read_be32(prop->value);
	return 0;
}

int of_property_read_string(const struct device_node *node, const char *name,
			    const char **out)
{
	const void *prop;
	int len;

	if (!out)
		return -EINVAL;

	prop = of_get_property(node, name, &len);
	if (!prop || len <= 0)
		return -EINVAL;
	if (!memchr(prop, '\0', (size_t)len))
		return -EINVAL;

	*out = prop;
	return 0;
}

bool of_device_is_available(const struct device_node *node)
{
	const char *status;
	int len;

	status = of_get_property(node, "status", &len);
	if (!status)
		return true;
	if (len <= 0)
		return false;

	return !strcmp(status, "okay") || !strcmp(status, "ok");
}

bool of_device_is_compatible(const struct device_node *node,
			     const char *compatible)
{
	const char *prop;
	int len;

	if (!node || !compatible)
		return false;

	prop = of_get_property(node, "compatible", &len);
	if (!prop || len <= 0)
		return false;

	for (const char *p = prop; p < prop + len;) {
		size_t remaining = (size_t)(prop + len - p);
		size_t str_len = strnlen(p, remaining);

		if (str_len == remaining)
			break;
		if (!strcmp(p, compatible))
			return true;
		p += str_len + 1;
	}
	return false;
}

const char *of_node_name(const struct device_node *node)
{
	return node ? node->name : NULL;
}

const char *of_node_full_name(const struct device_node *node)
{
	return node ? node->full_name : NULL;
}

static int of_n_cells(const struct device_node *node, const char *name,
		      int default_cells)
{
	const struct property *prop;
	u32 cells;

	prop = of_find_property_raw(node, name);
	if (!prop)
		return default_cells;
	if (prop->length != (int)sizeof(u32))
		return -EINVAL;

	cells = of_read_be32(prop->value);
	if (cells > OF_MAX_NCELLS)
		return -EINVAL;
	return (int)cells;
}

static int of_n_addr_cells(const struct device_node *node)
{
	int cells = of_n_cells(node, "#address-cells", 2);

	if (cells == 0)
		return -EINVAL;
	return cells;
}

static int of_n_size_cells(const struct device_node *node)
{
	return of_n_cells(node, "#size-cells", 1);
}

static u64 of_read_cells(const u32 **pp, int cells)
{
	u64 value = 0;

	while (cells-- > 0)
		value = (value << 32) | of_read_be32((*pp)++);
	return value;
}

static int of_translate_address(const struct device_node *node, u64 address,
				u64 *out)
{
	const struct device_node *bus;
	const struct device_node *parent;
	const struct property *ranges;
	int child_cells, parent_cells, size_cells;
	int entry_cells;
	int entry_bytes;

	bus = node ? node->parent : NULL;
	if (!bus) {
		*out = address;
		return 0;
	}

	parent = bus->parent;
	ranges = of_find_property_raw(bus, "ranges");
	if (!ranges || ranges->length <= 0) {
		*out = address;
		return 0;
	}

	child_cells = of_n_addr_cells(bus);
	parent_cells = parent ? of_n_addr_cells(parent) : 2;
	size_cells = of_n_size_cells(bus);
	if (child_cells < 0 || parent_cells < 0 || size_cells < 0)
		return -EINVAL;

	entry_cells = child_cells + parent_cells + size_cells;
	entry_bytes = entry_cells * (int)sizeof(u32);
	if (entry_cells <= 0 || ranges->length % entry_bytes)
		return -EINVAL;

	for (int i = 0; i < ranges->length / entry_bytes; i++) {
		const u32 *entry = (const u32 *)ranges->value + i * entry_cells;
		u64 child_base = of_read_cells(&entry, child_cells);
		u64 parent_base = of_read_cells(&entry, parent_cells);
		u64 size = of_read_cells(&entry, size_cells);

		if (address >= child_base && address < child_base + size)
			return of_translate_address(
				bus, parent_base + (address - child_base), out);
	}
	return -EINVAL;
}

int of_address_to_resource(const struct device_node *node, int index,
			   struct resource *res)
{
	const struct property *reg;
	const struct device_node *parent;
	const u32 *p;
	u64 address;
	u64 size;
	u64 translated;
	int addr_cells, size_cells;
	int cell_count;
	size_t needed;

	if (!node || !res || index < 0 || of_ensure_init())
		return -EINVAL;

	reg = of_find_property_raw(node, "reg");
	if (!reg)
		return -ENOENT;

	parent = node->parent;
	if (parent) {
		addr_cells = of_n_addr_cells(parent);
		size_cells = of_n_size_cells(parent);
	} else {
		addr_cells = 2;
		size_cells = 1;
	}
	if (addr_cells < 0 || size_cells < 0)
		return -EINVAL;

	cell_count = addr_cells + size_cells;
	needed = (size_t)(index + 1) * (size_t)cell_count * sizeof(u32);
	if (cell_count <= 0 || reg->length < 0 || (size_t)reg->length < needed)
		return -EINVAL;

	p = (const u32 *)reg->value + index * cell_count;
	address = of_read_cells(&p, addr_cells);
	size = of_read_cells(&p, size_cells);
	if (!size)
		return -EINVAL;

	if (of_translate_address(node, address, &translated))
		return -EINVAL;

	res->start = translated;
	res->end = translated + size - 1;
	res->flags = IORESOURCE_MEM;
	res->name = node->name;
	return 0;
}

int of_scan_cpu(void)
{
	struct device_node *node;
	struct device_node *cpu;
	const char *type;
	int len;

	node = of_find_node_by_path("/cpus");
	if (!node)
		return -ENOENT;

	of_for_each_child(cpu, node)
	{
		type = of_get_property(cpu, "device_type", &len);
		if (type && len > 0 && !strcmp(type, "cpu"))
			nr_harts += 1;
	}

	return 0;
}

int of_get_timebase_freq(u32 *freq_out)
{
	struct device_node *node;

	if (!freq_out)
		return -EINVAL;

	node = of_find_node_by_path("/cpus");
	if (!node) {
		pr_err("no cpus node in the DT\n");
		return -ENOENT;
	}

	return of_property_read_u32(node, "timebase-frequency", freq_out);
}

int of_irq_get(const struct device_node *node, int index)
{
	unsigned int virq;

	if (!node || index < 0 || of_ensure_init())
		return -EINVAL;

	virq = irq_of_parse_and_map(node, index);
	return virq ? (int)virq : -ENOENT;
}
