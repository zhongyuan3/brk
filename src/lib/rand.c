#include <brk/base/types.h>
#include <brk/lib/rand.h>

static uint64_t xorshift_state;

void rand_seed(uint64_t seed)
{
	xorshift_state = seed;
}

uint32_t rand_u32(void)
{
	xorshift_state ^= xorshift_state >> 12;
	xorshift_state ^= xorshift_state << 25;
	xorshift_state ^= xorshift_state >> 27;
	return (uint32_t)(xorshift_state * 0x2545F4914F6CDD1DULL >> 32);
}