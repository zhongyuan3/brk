#include <aosd/console.h>
#include <aosd/lock.h>
#include <aosd/sched.h>
#include <aosd/spinlock.h>
#include <aosd/uart.h>

#define BACKSPACE 0x100
#define CTRL(x) ((x) - '@')

static spinlock_define(cons_lock);
static char cons_buf[CONSOLE_BUF_SIZE];
static size_t cons_r;
static size_t cons_w;
static size_t cons_e;

void console_init(void)
{
	spinlock_init(&cons_lock, "cons_lock");
}

void console_putc(int c)
{
	if (c == BACKSPACE) {
		uart_putc('\b');
		uart_putc(' ');
		uart_putc('\b');
	} else {
		uart_putc(c);
	}
}

int console_write(const char *buf, size_t n, size_t *written)
{
	for (size_t i = 0; i < n; ++i)
		console_putc(buf[i]);
	if (written)
		*written = n;
	return 0;
}

int console_read(char *buf, size_t n, size_t *read)
{
	size_t target;
	int c;

	target = n;
	spinlock_acquire(&cons_lock);
	while (n > 0) {
		while (cons_r == cons_w) {
			if (task_is_killed(current_task())) {
				spinlock_release(&cons_lock);
				return -1;
			}
			sched_sleep(&cons_r, &cons_lock);
		}

		c = cons_buf[cons_r++ % CONSOLE_BUF_SIZE];

		if (c == CTRL('D')) {
			if (n < target)
				cons_r--;

			break;
		}

		*buf++ = c;
		--n;

		if (c == '\n')
			break;
	}
	spinlock_release(&cons_lock);

	if (read)
		*read = target - n;

	return 0;
}

void console_intr(int c)
{
	spinlock_acquire(&cons_lock);

	switch (c) {
	case CTRL('U'): /* Kill line. */
		while (cons_e != cons_w &&
		       cons_buf[(cons_e - 1) % CONSOLE_BUF_SIZE] != '\n') {
			cons_e--;
			console_putc(BACKSPACE);
		}
		break;
	case CTRL('H'): /* Backspace */
	case '\x7f': /* Delete key */
		if (cons_e != cons_w) {
			cons_e--;
			console_putc(BACKSPACE);
		}
		break;
	case CTRL('P'):
		show_all_tasks();
		break;
	default:
		if (c != 0 && cons_e - cons_r < CONSOLE_BUF_SIZE) {
			c = (c == '\r') ? '\n' : c;

			console_putc(c);

			cons_buf[cons_e++ % CONSOLE_BUF_SIZE] = c;

			if (c == '\n' || c == CTRL('D') ||
			    cons_e - cons_r == CONSOLE_BUF_SIZE) {
				cons_w = cons_e;
				sched_wake_up(&cons_r);
			}
		}
		break;
	}

	spinlock_release(&cons_lock);
}
