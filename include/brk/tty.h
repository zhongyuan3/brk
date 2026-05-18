#ifndef BRK_TTY_H
#define BRK_TTY_H

#include <brk/lock.h>
#include <brk/types.h>

#define TTY_RX_BUF_SIZE 1024

/* Passed to tty_port::put_char for erase rendering (not on the wire). */
#define TTY_VIS_BACKSPACE 0x100

struct file;
struct tty;

struct tty_port {
	struct tty *tty;
	void (*put_char)(struct tty_port *port, int c);
};

struct tty {
	spinlock_t lock;
	struct tty_port *port;
	char rx_buf[TTY_RX_BUF_SIZE];
	usize_t rx_r;
	usize_t rx_w;
	usize_t rx_e;
};

void tty_boot_init(struct tty_port *port);
struct tty *tty_boot(void);

int tty_chrdev_register(struct tty *tty, dev_t dev);

void tty_init(struct tty *tty, struct tty_port *port);
int tty_read(struct tty *tty, char *buf, usize_t n, usize_t *read);
int tty_write(struct tty *tty, const char *buf, usize_t n, usize_t *written);
void tty_receive(struct tty *tty, int c);

#endif
