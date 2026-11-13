#ifndef BRK_OF_H
#define BRK_OF_H

#include <brk/list.h>
#include <brk/resource.h>
#include <brk/types.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Device-tree node tree.
 *
 * The FDT is unflattened during arch_init(), before the page allocator is
 * available.  Nodes, properties, names and property values are copied into
 * one memblock allocation and stay valid for the lifetime of the kernel.
 * After of_init() no code outside dev/of should touch the raw FDT.
 */
struct property {
	const char *name;
	const void *value;
	int length;
};

struct device_node {
	const char *name;
	const char *full_name;
	const char *type;

	struct property *properties;
	int nr_properties;

	u32 phandle;

	struct device_node *parent;
	struct list_head children;
	struct list_head sibling;
};

#define of_for_each_child(child, parent) \
	list_for_each_entry(child, &(parent)->children, sibling)

/* Early boot: raw FDT backing store and physical-memory scans. */
void of_set_fdt(unsigned long addr);
size_t of_get_fdt_size(unsigned long addr);
int of_scan_memory(void);
int of_scan_reserved_memory(void);

/* Build the device_node tree from the FDT. */
int of_init(void);
struct device_node *of_root(void);

int of_scan_cpu(void);
int of_get_timebase_freq(u32 *freq_out);

struct device_node *of_find_node_by_path(const char *path);
struct device_node *of_find_compatible_node(const char *compatible);
struct device_node *of_find_node_by_phandle(u32 phandle);

struct device_node *of_get_parent(const struct device_node *node);
struct device_node *of_get_child_by_name(const struct device_node *node,
					 const char *name);

const struct property *of_find_property(const struct device_node *node,
					const char *name, int *lenp);
const void *of_get_property(const struct device_node *node, const char *name,
			    int *lenp);
int of_property_read_u32(const struct device_node *node, const char *name,
			 u32 *out);
int of_property_read_string(const struct device_node *node, const char *name,
			    const char **out);

bool of_device_is_available(const struct device_node *node);
bool of_device_is_compatible(const struct device_node *node,
			     const char *compatible);

const char *of_node_name(const struct device_node *node);
const char *of_node_full_name(const struct device_node *node);

int of_address_to_resource(const struct device_node *node, int index,
			   struct resource *res);
int of_irq_get(const struct device_node *node, int index);

int of_platform_populate(void);

/* Read one big-endian cell from an OF property value. */
static inline u32 of_read_be32(const void *p)
{
	const u8 *b = p;

	return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) |
	       (u32)b[3];
}

#endif
