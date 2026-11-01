#ifndef BRK_TTY_H
#define BRK_TTY_H

#include <brk/refcnt_types.h>
#include <brk/sleeplock_types.h>
#include <brk/spinlock_types.h>
#include <brk/types.h>
#include <uapi/ioctl.h>
#include <uapi/termios.h>
#include <uapi/types.h>

#define TTY_RX_BUF_SIZE 1024

/* Passed to tty_port::put_char for erase rendering (not on the wire). */
#define TTY_VIS_BACKSPACE 0x100

struct fs_file;
struct char_dev;
struct device;
struct task;
struct tty;
struct tty_port;
struct tty_ops;
struct tty_driver;

struct tty_ops {
	int (*put_char)(struct tty *tty, int c);
};

struct tty_ldisc_ops {
	const char *name;
	int (*open)(struct tty *tty);
	void (*close)(struct tty *tty);
	ssize_t (*read)(struct tty *tty, char *buf, size_t size);
	ssize_t (*write)(struct tty *tty, const char *buf, size_t size);
	void (*receive_buf)(struct tty *tty, const u8 *buf, size_t size);
	long (*ioctl)(struct tty *tty, unsigned int cmd, unsigned long arg);
};

struct tty_port {
	struct tty *tty;
	struct tty_driver *driver;
	spinlock_t lock;
	void *client_data;
	/* Foreground process group and owning session (job control). */
	pid_t pgrp;
	pid_t sid;

	unsigned int open_count;
	struct termios termios;
	bool dead;
};

struct tty_driver {
	const char *name;
	const struct tty_ops *ops;
	struct tty_port **ports;
	struct char_dev **cds;
	struct device **devs;
	int num_ports;
	unsigned major;
	unsigned minor_start;
	struct hlist_node driver_list;
	spinlock_t lock;
	void *driver_data;
};

struct tty_driver *tty_alloc_driver(int num_ports);
void tty_free_driver(struct tty_driver *driver);
int tty_register_driver(struct tty_driver *driver);
int tty_unregister_driver(struct tty_driver *driver);
struct tty_port *tty_lookup_port(dev_t dev);

int tty_driver_add_port(struct tty_driver *driver, struct tty_port *port);
int tty_driver_remove_port(struct tty_driver *driver, struct tty_port *port);

struct tty_port *tty_port_alloc(void);
void tty_port_free(struct tty_port *port);

struct tty {
	struct tty_port *port;
	u8 *rx_buf;
	size_t rx_size;
	size_t rx_r;
	size_t rx_w;
	size_t rx_e;
	bool eof_pending;
	bool lnext;
	bool stopped;
	bool dead;
	struct winsize winsize;
	refcnt_t refcnt;
	const struct tty_ldisc_ops *ldisc;
	sleeplock_t read_lock;
	sleeplock_t write_lock;
};

extern const struct tty_ldisc_ops n_tty_ldisc_ops;

void tty_termios_defaults(struct termios *termios);

struct tty *tty_alloc(void);
void tty_free(struct tty *tty);
void tty_init(struct tty *tty, struct tty_port *port);
struct tty *tty_attach_port(struct tty_port *port);
void tty_detach_port(struct tty_port *port);
struct tty *tty_open(struct tty_port *port);
void tty_close(struct tty *tty);
ssize_t tty_read(struct tty *tty, char *buf, size_t n);
ssize_t tty_write(struct tty *tty, const void *buf, size_t n);
void tty_receive(struct tty *tty, int c);
long tty_ioctl(struct tty *tty, unsigned int cmd, unsigned long arg);

#endif
