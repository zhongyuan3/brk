#ifndef BRK_NET_NET_H
#define BRK_NET_NET_H

#include <brk/types.h>

/*
 * IPv4 addresses use the network byte order numeric convention: NET_IP4()
 * produces the same 32-bit value that a load of the four wire bytes yields,
 * so addresses are stored into headers and compared without byte swapping.
 * Ports, lengths and protocol fields still go through htons()/ntohs().
 */
#define NET_IP4(a, b, c, d) \
	((u32)(a) | ((u32)(b) << 8) | ((u32)(c) << 16) | ((u32)(d) << 24))

#define NET_IP4_FMT "%u.%u.%u.%u"
#define NET_IP4_ARG(a)                                                     \
	((unsigned int)((a) & 0xff)), ((unsigned int)(((a) >> 8) & 0xff)), \
		((unsigned int)(((a) >> 16) & 0xff)),                      \
		((unsigned int)(((a) >> 24) & 0xff))

/* QEMU user-mode networking defaults. */
#define NET_DEFAULT_IP NET_IP4(10, 0, 2, 15)
#define NET_DEFAULT_NETMASK NET_IP4(255, 255, 255, 0)
#define NET_DEFAULT_GW NET_IP4(10, 0, 2, 2)

static inline u16 htons(u16 x)
{
	return __builtin_bswap16(x);
}

static inline u16 ntohs(u16 x)
{
	return __builtin_bswap16(x);
}

static inline u32 htonl(u32 x)
{
	return __builtin_bswap32(x);
}

static inline u32 ntohl(u32 x)
{
	return __builtin_bswap32(x);
}

/*
 * Internet checksum helpers.  The accumulator works on big-endian 16-bit
 * words, so inet_csum() returns a value in network byte order that can be
 * stored directly into a header field.  Verifying a header that already
 * contains its checksum therefore reduces to inet_csum(hdr, len) == 0.
 */
u32 inet_csum_add(u32 sum, const void *data, size_t len);
u16 inet_csum_fold(u32 sum);
u16 inet_csum(const void *data, size_t len);

/* Parse a dotted-quad string; returns 0 on success. */
int net_parse_ipv4(const char *buf, size_t len, u32 *addr);

/* Send one ICMP echo request to @daddr (host byte order). */
int net_ping(u32 daddr);

#endif
