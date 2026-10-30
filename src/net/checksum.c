#include <brk/net/net.h>
#include <brk/types.h>

u32 inet_csum_add(u32 sum, const void *data, size_t len)
{
	const u8 *p = data;

	while (len > 1) {
		sum += ((u32)p[0] << 8) | p[1];
		p += 2;
		len -= 2;
	}

	if (len)
		sum += (u32)p[0] << 8;

	return sum;
}

u16 inet_csum_fold(u32 sum)
{
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return htons((u16)~sum);
}

u16 inet_csum(const void *data, size_t len)
{
	return inet_csum_fold(inet_csum_add(0, data, len));
}
