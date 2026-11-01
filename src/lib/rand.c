#include <brk/rand.h>
#include <brk/types.h>

static u64 xorshift_state;

void rand_seed(u64 seed)
{
	xorshift_state = seed;
}

u32 rand_u32(void)
{
	xorshift_state ^= xorshift_state >> 12;
	xorshift_state ^= xorshift_state << 25;
	xorshift_state ^= xorshift_state >> 27;
	return (u32)(xorshift_state * 0x2545F4914F6CDD1DULL >> 32);
}