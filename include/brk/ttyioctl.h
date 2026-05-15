#ifndef BRK_TTYIOCTL_H
#define BRK_TTYIOCTL_H

#include <brk/termios.h>
#include <brk/types.h>

/* Aliases to Linux c_lflag bits (for legacy brk ioctls). */
#define TTY_LFLAG_ICANON ICANON
#define TTY_LFLAG_ECHO ECHO

struct brk_tty_attr {
	u32 lflags;
	u8 vmin;
	u8 vtime;
	u8 _pad[2];
};

#define BRK_TIOCGLFLAGS 0xBF01u
#define BRK_TIOCSLFLAGS 0xBF02u
#define BRK_TIOCGETATTR 0xBF03u
#define BRK_TIOCSETATTR 0xBF04u

#endif
