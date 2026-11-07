#ifndef BRK_OF_H
#define BRK_OF_H

#include <brk/list.h>
#include <brk/resource.h>
#include <brk/types.h>
#include <stdbool.h>

/*
 * Device-tree node tree.
 *
 * The FDT itself stays the backing store for properties; device_node stores
 * the tree shape and the FDT offset.  Drivers should use the of_* accessors
 * instead of libfdt directly.
 */
struct device_node {
	const char *name;
	const char *full_name;
	const char *type;

	int fdt_offset;
	u32 phandle;

	struct device_node *parent;
	struct list_head children;
	struct list_head sibling;
};

#define of_for_each_child(child, parent) \
	list_for_each_entry(child, &(parent)->children, sibling)

int of_init(void);
struct device_node *of_root(void);

struct device_node *of_find_node_by_path(const char *path);
struct device_node *of_find_compatible_node(const char *compatible);
struct device_node *of_find_node_by_phandle(u32 phandle);

struct device_node *of_get_parent(const struct device_node *node);
struct device_node *of_get_child_by_name(const struct device_node *node,
					 const char *name);

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

#endif
