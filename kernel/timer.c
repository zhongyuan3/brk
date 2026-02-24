#include <aosd/cpu.h>
#include <aosd/lock.h>
#include <aosd/riscv.h>
#include <aosd/sbi.h>
#include <aosd/spinlock.h>
#include <aosd/timer.h>

static uint64_t timer_interval;
static uint64_t jiffies;
static spinlock_define(jiffies_lock);

void timer_init(void)
{
	uint32_t timebase_freq = cpu_get_timebase_freq();
	timer_interval = timebase_freq / 1000;
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
	if (current_cpu()->hart_id == 0) {
		spinlock_acquire(&jiffies_lock);
		++jiffies;
		spinlock_release(&jiffies_lock);
	}

	timer_set_next();
}
