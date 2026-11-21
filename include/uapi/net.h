#ifndef UAPI_NET_H
#define UAPI_NET_H

#include <uapi/socket.h>
#include <uapi/types.h>

#define IFNAMSIZ 16

#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK 0x8

#define ARPHRD_ETHER 1

#define SIOCADDRT 0x890B
#define SIOCDELRT 0x890C
#define SIOCGIFCONF 0x8912
#define SIOCGIFFLAGS 0x8913
#define SIOCSIFFLAGS 0x8914
#define SIOCGIFADDR 0x8915
#define SIOCSIFADDR 0x8916
#define SIOCGIFBRDADDR 0x8919
#define SIOCSIFBRDADDR 0x891a
#define SIOCGIFNETMASK 0x891b
#define SIOCSIFNETMASK 0x891c
#define SIOCGIFMTU 0x8921
#define SIOCSIFMTU 0x8922
#define SIOCGIFHWADDR 0x8927
#define SIOCGIFSTATS 0x89f0

#define RTF_UP 0x0001
#define RTF_GATEWAY 0x0002

struct __brk_ifreq {
	char ifr_name[IFNAMSIZ];
	union {
		struct __brk_sockaddr ifru_addr;
		struct __brk_sockaddr ifru_netmask;
		struct __brk_sockaddr ifru_broadaddr;
		struct __brk_sockaddr ifru_hwaddr;
		short ifru_flags;
		int ifru_mtu;
	} ifr_ifru;
};

#define ifr_addr ifr_ifru.ifru_addr
#define ifr_netmask ifr_ifru.ifru_netmask
#define ifr_broadaddr ifr_ifru.ifru_broadaddr
#define ifr_hwaddr ifr_ifru.ifru_hwaddr
#define ifr_flags ifr_ifru.ifru_flags
#define ifr_mtu ifr_ifru.ifru_mtu

struct __brk_ifconf {
	int ifc_len;
	union {
		char *ifcu_buf;
		struct __brk_ifreq *ifcu_req;
	} ifc_ifcu;
};

#define ifc_buf ifc_ifcu.ifcu_buf
#define ifc_req ifc_ifcu.ifcu_req

struct __brk_ifstats {
	char ifs_name[IFNAMSIZ];
	__brk_u64 rx_packets;
	__brk_u64 tx_packets;
	__brk_u64 rx_bytes;
	__brk_u64 tx_bytes;
	__brk_u64 rx_errors;
	__brk_u64 tx_errors;
	__brk_u64 rx_dropped;
	__brk_u64 tx_dropped;
};

struct __brk_rtentry {
	struct __brk_sockaddr rt_dst;
	struct __brk_sockaddr rt_gateway;
	struct __brk_sockaddr rt_genmask;
	unsigned short rt_flags;
	unsigned short rt_metric;
	char rt_dev[IFNAMSIZ];
};

#endif
