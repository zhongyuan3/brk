#ifndef BRK_TERMIOS_H
#define BRK_TERMIOS_H

/*
 * Linux-compatible termios layout and ioctl numbers (asm-generic / glibc).
 * sizeof(struct termios) must match Linux uapi for TCGETS/TCSETS.
 */

#include <brk/types.h>

typedef unsigned char cc_t;
typedef unsigned int speed_t;
typedef unsigned int tcflag_t;

#define NCCS 19

struct termios {
	tcflag_t c_iflag;
	tcflag_t c_oflag;
	tcflag_t c_cflag;
	tcflag_t c_lflag;
	cc_t c_line;
	cc_t c_cc[NCCS];
};

#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSWTC 7
#define VSTART 8
#define VSTOP 9
#define VSUSP 10
#define VEOL 11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE 14
#define VLNEXT 15
#define VEOL2 16

#define CSIZE 0x00000030u
#define CS8 0x00000030u
#define CREAD 0x00000080u
#define HUPCL 0x00000400u

#define ISIG 0x00000001u
#define ICANON 0x00000002u
#define ECHO 0x00000008u
#define ECHOE 0x00000010u
#define ECHOK 0x00000020u
#define ECHONL 0x00000040u
#define NOFLSH 0x00000080u

#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#define TCGETS 0x5401u
#define TCSETS 0x5402u
#define TCSETSW 0x5403u
#define TCSETSF 0x5404u

/*
 * Window size (Linux asm-generic/ioctls.h). Used with TIOCGWINSZ / TIOCSWINSZ.
 * Default is a conventional serial/QEMU nographic size until a real VT exists.
 */
struct winsize {
	unsigned short ws_row;
	unsigned short ws_col;
	unsigned short ws_xpixel;
	unsigned short ws_ypixel;
};

#define TIOCGWINSZ 0x5413u
#define TIOCSWINSZ 0x5414u

#endif
