#include <aosd/cpu.h>
#include <aosd/lock.h>
#include <aosd/process.h>
#include <aosd/riscv.h>
#include <aosd/sbi.h>
#include <aosd/timer.h>

static uint64_t timer_interval;
static uint64_t jiffies;
static SPINLOCK_DEFINE(jiffies_lock);
static uint64_t xorshift_state;
static struct timeval walltime;

void timer_init(void)
{
	uint32_t timebase_freq = cpu_get_timebase_freq();
	timer_interval = timebase_freq / 1000;
	walltime.tv_sec = 0;
	walltime.tv_usec = 0;
}

uint64_t timer_get_time(void)
{
	return read_time();
}

void timer_set_next(void)
{
	sbi_set_timer(timer_get_time() + timer_interval);
}

void timer_handle_int(void)
{
	if (current_cpuid() == init_cpuid) {
		spinlock_acquire(&jiffies_lock);
		++jiffies;
		/* 1 ms = 1000 us */
		walltime.tv_usec += 1000;
		/* 1 s = 1000000 us */
		if (walltime.tv_usec >= 1000000) {
			walltime.tv_sec += 1;
			walltime.tv_usec = 0;
		}
		spinlock_release(&jiffies_lock);
	}

	timer_set_next();
}

void walltime_get(struct timeval *tv)
{
	spinlock_acquire(&jiffies_lock);
	tv->tv_sec = walltime.tv_sec;
	tv->tv_usec = walltime.tv_usec;
	spinlock_release(&jiffies_lock);
}

void walltime_set(const struct timeval *tv)
{
	spinlock_acquire(&jiffies_lock);
	walltime.tv_sec = tv->tv_sec;
	walltime.tv_usec = tv->tv_usec;
	spinlock_release(&jiffies_lock);
}

uint64_t do_nanosleep(const struct timeval *dur, struct timeval *rem)
{
	struct timeval start, curr;

	walltime_get(&start);
	spinlock_acquire(&jiffies_lock);
	for (;;) {
		if (proc_is_killed(current_process())) {
			spinlock_release(&jiffies_lock);
			return -1;
		}
		walltime_get(&curr);
		if ((curr.tv_sec - start.tv_sec >= dur->tv_sec) &&
		    (curr.tv_usec - start.tv_usec >= dur->tv_usec))
			break;
		proc_sleep(&jiffies, &jiffies_lock);
	}
	spinlock_release(&jiffies_lock);

	rem->tv_sec = 0;
	rem->tv_usec = 0;
	return 0;
}

static void xorshift_srand(void)
{
	xorshift_state = read_time();
}

static uint32_t xorshift_rand(void)
{
	xorshift_state ^= xorshift_state >> 12;
	xorshift_state ^= xorshift_state << 25;
	xorshift_state ^= xorshift_state >> 27;
	return (uint32_t)(xorshift_state * 0x2545F4914F6CDD1DULL >> 32);
}

void timer_srand(void)
{
	xorshift_srand();
}

uint32_t timer_rand(void)
{
	return xorshift_rand();
}

uint64_t jiffies_get(void)
{
	uint64_t j;
	spinlock_acquire(&jiffies_lock);
	j = jiffies;
	spinlock_release(&jiffies_lock);
	return j;
}
