#include <aosd/cpu.h>
#include <aosd/printk.h>
#include <aosd/sched_types.h>
#include <aosd/syscall.h>

void syscall(void)
{
	struct task *task = current_cpu()->current;
	printk("syscall: %c\n", (int)task->tf.a0);
}
