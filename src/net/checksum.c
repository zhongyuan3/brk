#include <brk/net/net.h>
#include <brk/types.h>

uint32_t inet_csum_add(uint32_t sum, const void *data, size_t len)
{
	const uint8_t *p = data;

	while (len > 1) {
		sum += ((uint32_t)p[0] << 8) | p[1];
		p += 2;
		len -= 2;
	}

	if (len)
		sum += (uint32_t)p[0] << 8;

	return sum;
}

uint16_t inet_csum_fold(uint32_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return htons((uint16_t)~sum);
}

uint16_t inet_csum(const void *data, size_t len)
{
	return inet_csum_fold(inet_csum_add(0, data, len));
}
