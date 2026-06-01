#include <brk/cpu.h>
#include <brk/riscv.h>
#include <brk/sbi.h>
#include <brk/task.h>
#include <brk/timekeeper.h>
#include <brk/timer.h>
static u64 timer_interval;
static u64 xorshift_state;

void timer_init(void)
{
	u32 timebase_freq = cpu_get_timebase_freq();
	timer_interval = timebase_freq / 1000;
}

u64 timer_get_time(void)
{
	return read_time();
}

void timer_set_next(void)
{
	sbi_set_timer(timer_get_time() + timer_interval);
}

void timer_handle_int(void)
{
	if (current_cpuid() == boot_cpuid)
		timekeeper_tick();

	task_wake_all(timekeeper_wait_chan());

	timer_set_next();
}

static void xorshift_srand(void)
{
	xorshift_state = read_time();
}

static u32 xorshift_rand(void)
{
	xorshift_state ^= xorshift_state >> 12;
	xorshift_state ^= xorshift_state << 25;
	xorshift_state ^= xorshift_state >> 27;
	return (u32)(xorshift_state * 0x2545F4914F6CDD1DULL >> 32);
}

void timer_srand(void)
{
	xorshift_srand();
}

u32 timer_rand(void)
{
	return xorshift_rand();
}
