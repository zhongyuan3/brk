#ifndef BRK_TTY_H
#define BRK_TTY_H

#include <brk/list.h>
#include <brk/lock.h>
#include <brk/termios.h>
#include <brk/ttyioctl.h>
#include <brk/types.h>

#define TTY_RX_BUF_SIZE 1024

/* Passed to tty_port::put_char for erase rendering (not on the wire). */
#define TTY_VIS_BACKSPACE 0x100

struct file;
struct tty;

/*
 * Per-struct file wait state (VTIME sleepers only). termios lives on struct tty
 * so dup/fork/extra opens of the same device share Linux-like attributes.
 */
struct tty_file_priv {
	struct list_head vtime_link;
	u64 raw_deadline_jiffies;
	bool raw_timer_armed;
};

struct tty_port {
	struct tty *tty;
	void (*put_char)(struct tty_port *port, int c);
};

struct tty {
	spinlock_t lock;
	struct tty_port *port;
	struct termios termios;
	struct winsize winsize;
	struct list_head vtime_waiters;
	bool null_raw_armed;
	u64 null_raw_deadline_jiffies;
	char rx_buf[TTY_RX_BUF_SIZE];
	usize_t rx_r;
	usize_t rx_w;
	usize_t rx_e;
};

void tty_boot_init(struct tty_port *port);
struct tty *tty_boot(void);

int tty_chrdev_register(struct tty *tty, dev_t dev);

struct tty_file_priv *tty_file_priv_create(void);
void tty_file_priv_destroy(struct tty_file_priv *priv);

void tty_init(struct tty *tty, struct tty_port *port);
void tty_timer_tick(void);
int tty_read(struct tty *tty, struct file *file, char *buf, usize_t n,
	     usize_t *read);
int tty_write(struct tty *tty, const char *buf, usize_t n, usize_t *written);
void tty_receive(struct tty *tty, int c);
long tty_ioctl(struct tty *tty, struct file *file, unsigned int cmd,
	       unsigned long arg);

#endif
