#include <brk/lock.h>
#include <brk/slab.h>
#include <brk/tty.h>
#include <brk/types.h>

struct tty_port *tty_port_alloc(void)
{
	struct tty_port *port = kzalloc(sizeof(struct tty_port));
	if (!port)
		return NULL;
	spinlock_init(&port->lock, "tty_port");
	return port;
}

void tty_port_free(struct tty_port *port)
{
	kfree(port);
}
