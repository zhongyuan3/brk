#ifndef BRK_NET_ROUTE_H
#define BRK_NET_ROUTE_H

#include <brk/net/netdevice.h>
#include <brk/types.h>

struct rt_entry {
	uint32_t dst;
	uint32_t mask;
	uint32_t gw;
	struct net_device *dev;
};

int route_add(uint32_t dst, uint32_t mask, uint32_t gw, struct net_device *dev);

/*
 * Resolve @daddr to an output device and next hop.  @nexthop is the gateway
 * when one is configured, otherwise @daddr itself.
 */
int ip_route_output(uint32_t daddr, uint32_t *nexthop, struct net_device **dev);

#endif
