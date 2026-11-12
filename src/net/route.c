#include <brk/net.h>
#include <brk/netdevice.h>
#include <brk/route.h>
#include <brk/spinlock.h>
#include <brk/types.h>
#include <uapi/errno.h>

#define RT_TABLE_SIZE 8

static struct rt_entry rt_table[RT_TABLE_SIZE];
static SPINLOCK_DEFINE(rt_lock);

int route_add(u32 dst, u32 mask, u32 gw, struct net_device *dev)
{
	irqstate_t flags;
	int ret = -ENOSPC;

	if (!dev)
		return -EINVAL;

	flags = spin_lock_irqsave(&rt_lock);
	for (unsigned int i = 0; i < RT_TABLE_SIZE; i++) {
		if (!rt_table[i].dev)
			continue;
		if (rt_table[i].dst == dst && rt_table[i].mask == mask &&
		    rt_table[i].gw == gw && rt_table[i].dev == dev) {
			spin_unlock_irqrestore(&rt_lock, flags);
			return 0;
		}
	}

	for (unsigned int i = 0; i < RT_TABLE_SIZE; i++) {
		if (rt_table[i].dev)
			continue;
		rt_table[i].dst = dst;
		rt_table[i].mask = mask;
		rt_table[i].gw = gw;
		rt_table[i].dev = dev;
		ret = 0;
		break;
	}
	spin_unlock_irqrestore(&rt_lock, flags);

	return ret;
}

int route_del(u32 dst, u32 mask, u32 gw)
{
	irqstate_t flags;
	int ret = -ENOENT;

	flags = spin_lock_irqsave(&rt_lock);
	for (unsigned int i = 0; i < RT_TABLE_SIZE; i++) {
		if (!rt_table[i].dev)
			continue;
		if (rt_table[i].dst != dst || rt_table[i].mask != mask ||
		    rt_table[i].gw != gw)
			continue;
		rt_table[i].dev = NULL;
		ret = 0;
		break;
	}
	spin_unlock_irqrestore(&rt_lock, flags);

	return ret;
}

int ip_route_output(u32 daddr, u32 *nexthop, struct net_device **dev)
{
	struct rt_entry *best = NULL;
	irqstate_t flags;

	if (daddr == 0xffffffffU) {
		struct net_device *bdev = netdev_get_broadcast_dev();

		if (!bdev)
			return -ENETUNREACH;
		*nexthop = daddr;
		*dev = bdev;
		return 0;
	}

	flags = spin_lock_irqsave(&rt_lock);
	for (unsigned int i = 0; i < RT_TABLE_SIZE; i++) {
		struct rt_entry *e = &rt_table[i];

		if (!e->dev)
			continue;
		if ((daddr & e->mask) != (e->dst & e->mask))
			continue;
		if (!best || e->mask > best->mask)
			best = e;
	}

	if (best) {
		*nexthop = best->gw ? best->gw : daddr;
		*dev = best->dev;
	}
	spin_unlock_irqrestore(&rt_lock, flags);

	return best ? 0 : -ENETUNREACH;
}
