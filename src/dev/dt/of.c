#include <brk/dt.h>
#include <brk/initcall.h>
#include <brk/irqdomain.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/of.h>
#include <brk/printk.h>
#include <brk/string.h>
#include <libfdt.h>
#include <uapi/errno.h>

static struct device_node *of_root_node;
static bool of_initialized;

static int of_fdt_errno(int err)
{
	if (err >= 0)
		return err;

	switch (-err) {
	case FDT_ERR_NOTFOUND:
		return -ENOENT;
	case FDT_ERR_NOSPACE:
		return -ENOSPC;
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
	default:
		return -EINVAL;
	}
}

static char *of_strdup(const char *s)
{
	size_t len = strlen(s) + 1;
	char *copy = kmalloc(len);

	if (copy)
		memcpy(copy, s, len);
	return copy;
}

static struct device_node *of_alloc_node(int offset, struct device_node *parent)
{
	struct device_node *node;
	const void *prop;
	const char *name;
	char path[512];
	int len;

	node = kzalloc(sizeof(*node));
	if (!node)
		return NULL;

	name = fdt_get_name(dt_get_fdt(), offset, &len);
	node->name = name ? name : "";
	node->fdt_offset = offset;
	node->phandle = fdt_get_phandle(dt_get_fdt(), offset);
	node->parent = parent;
	list_init(&node->children);
	list_init(&node->sibling);

	len = fdt_get_path(dt_get_fdt(), offset, path, sizeof(path));
	if (len >= 0)
		node->full_name = of_strdup(path);
	else
		node->full_name = of_strdup(node->name);
	if (!node->full_name)
		goto fail;

	prop = fdt_getprop(dt_get_fdt(), offset, "device_type", &len);
	if (prop && len > 0)
		node->type = prop;

	return node;

fail:
	kfree((void *)node->full_name);
	kfree(node);
	return NULL;
}

static void of_free_node(struct device_node *node)
{
	struct device_node *child, *tmp;

	if (!node)
		return;

	list_for_each_entry_safe(child, tmp, &node->children, sibling) {
		list_del(&child->sibling);
		of_free_node(child);
	}
	kfree((void *)node->full_name);
	kfree(node);
}

static int of_build_children(struct device_node *parent)
{
	int offset;

	fdt_for_each_subnode(offset, dt_get_fdt(), parent->fdt_offset) {
		struct device_node *child = of_alloc_node(offset, parent);
		int err;

		if (!child)
			return -ENOMEM;
		list_add_tail(&child->sibling, &parent->children);

		err = of_build_children(child);
		if (err)
			return err;
	}
	return 0;
}

int of_init(void)
{
	int offset;
	int err;

	if (of_initialized)
		return 0;
	if (!dt_get_fdt())
		return -ENODEV;

	offset = fdt_path_offset(dt_get_fdt(), "/");
	if (offset < 0)
		return of_fdt_errno(offset);

	of_root_node = of_alloc_node(offset, NULL);
	if (!of_root_node)
		return -ENOMEM;

	err = of_build_children(of_root_node);
	if (err) {
		of_free_node(of_root_node);
		of_root_node = NULL;
		return err;
	}

	of_initialized = true;
	pr_debug("of: root=%s\n", of_root_node->full_name);
	return 0;
}

static void of_initcall(void)
{
	int err = of_init();

	if (err)
		pr_warn("of: failed to build device tree: %d\n", err);
}
core_initcall(of_initcall);

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

static struct device_node *of_find_by_offset(struct device_node *node, int off)
{
	struct device_node *child;

	if (node->fdt_offset == off)
		return node;

	of_for_each_child(child, node)
	{
		struct device_node *found = of_find_by_offset(child, off);

		if (found)
			return found;
	}
	return NULL;
}

struct device_node *of_find_node_by_path(const char *path)
{
	int offset;

	if (!path || of_ensure_init())
		return NULL;

	offset = fdt_path_offset(dt_get_fdt(), path);
	if (offset < 0)
		return NULL;
	return of_find_by_offset(of_root_node, offset);
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

const void *of_get_property(const struct device_node *node, const char *name,
			    int *lenp)
{
	if (!node || !name || of_ensure_init())
		return NULL;
	return fdt_getprop(dt_get_fdt(), node->fdt_offset, name, lenp);
}

int of_property_read_u32(const struct device_node *node, const char *name,
			 u32 *out)
{
	const u32 *prop;
	int len;

	if (!out)
		return -EINVAL;

	prop = of_get_property(node, name, &len);
	if (!prop || len < (int)sizeof(u32))
		return -EINVAL;

	*out = fdt32_to_cpu(*prop);
	return 0;
}

int of_property_read_string(const struct device_node *node, const char *name,
			    const char **out)
{
	const char *prop;
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

	for (const char *p = prop; p < prop + len; p += strlen(p) + 1) {
		if (!strcmp(p, compatible))
			return true;
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

static u64 of_read_cells(const u32 **pp, int cells)
{
	u64 value = 0;

	while (cells-- > 0)
		value = (value << 32) | fdt32_to_cpu(*(*pp)++);
	return value;
}

static int of_translate_address(const struct device_node *node, u64 address,
				u64 *out)
{
	const struct device_node *bus;
	const struct device_node *parent;
	const u32 *ranges;
	int len;
	int child_cells, parent_cells, size_cells;
	int entry_cells;
	int entry_bytes;

	bus = node ? node->parent : NULL;
	if (!bus) {
		*out = address;
		return 0;
	}

	parent = bus->parent;
	ranges = fdt_getprop(dt_get_fdt(), bus->fdt_offset, "ranges", &len);
	if (!ranges || len <= 0) {
		*out = address;
		return 0;
	}

	child_cells = fdt_address_cells(dt_get_fdt(), bus->fdt_offset);
	parent_cells =
		parent ? fdt_address_cells(dt_get_fdt(), parent->fdt_offset) :
			 2;
	size_cells = fdt_size_cells(dt_get_fdt(), bus->fdt_offset);
	if (child_cells < 0 || parent_cells < 0 || size_cells < 0)
		return -EINVAL;

	entry_cells = child_cells + parent_cells + size_cells;
	entry_bytes = entry_cells * (int)sizeof(u32);
	if (entry_cells <= 0 || len % entry_bytes)
		return -EINVAL;

	for (int i = 0; i < len / entry_bytes; i++) {
		const u32 *entry = ranges + i * entry_cells;
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
	const u32 *reg;
	const u32 *p;
	u64 address = 0;
	u64 size = 0;
	u64 translated;
	int len;
	int addr_cells, size_cells;
	int cell_count;
	int parent;

	if (!node || !res || index < 0 || of_ensure_init())
		return -EINVAL;

	reg = fdt_getprop(dt_get_fdt(), node->fdt_offset, "reg", &len);
	if (!reg)
		return of_fdt_errno(len);

	parent = fdt_parent_offset(dt_get_fdt(), node->fdt_offset);
	if (parent >= 0) {
		addr_cells = fdt_address_cells(dt_get_fdt(), parent);
		size_cells = fdt_size_cells(dt_get_fdt(), parent);
	} else {
		addr_cells = 2;
		size_cells = 1;
	}
	if (addr_cells < 0 || size_cells < 0)
		return -EINVAL;

	cell_count = addr_cells + size_cells;
	if (cell_count <= 0 ||
	    len < (index + 1) * cell_count * (int)sizeof(u32))
		return -EINVAL;

	p = reg + index * cell_count;
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

int of_irq_get(const struct device_node *node, int index)
{
	unsigned int virq;

	if (!node || index < 0 || of_ensure_init())
		return -EINVAL;

	virq = irq_of_parse_and_map(node->fdt_offset, index);
	return virq ? (int)virq : -ENOENT;
}
