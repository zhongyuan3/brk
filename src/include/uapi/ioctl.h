#ifndef UAPI_IOCTL_H
#define UAPI_IOCTL_H

#include <uapi/types.h>

struct winsize {
	__u16 ws_row;
	__u16 ws_col;
	__u16 ws_xpixel;
	__u16 ws_ypixel;
};

#define TIOCGWINSZ 0x5413u
#define TIOCSWINSZ 0x5414u

/* Terminal job control (Linux generic values). */
#define TIOCSCTTY 0x540Eu
#define TIOCGPGRP 0x540Fu
#define TIOCSPGRP 0x5410u
#define TIOCNOTTY 0x5422u

/* Block device ioctls (Linux generic values). */
#define BLKGETSIZE64 0x80081272u
#define BLKFLSBUF 0x1261u

#endif
