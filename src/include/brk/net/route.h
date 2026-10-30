#ifndef BRK_NET_ROUTE_H
#define BRK_NET_ROUTE_H

#include <brk/net/netdevice.h>
#include <brk/types.h>

struct rt_entry {
	u32 dst;
	u32 mask;
	u32 gw;
	struct net_device *dev;
};

int route_add(u32 dst, u32 mask, u32 gw, struct net_device *dev);

/*
 * Resolve @daddr to an output device and next hop.  @nexthop is the gateway
 * when one is configured, otherwise @daddr itself.
 */
int ip_route_output(u32 daddr, u32 *nexthop, struct net_device **dev);

#endif
