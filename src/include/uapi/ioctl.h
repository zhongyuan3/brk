#ifndef UAPI_IOCTL_H
#define UAPI_IOCTL_H

struct winsize {
	unsigned short ws_row;
	unsigned short ws_col;
	unsigned short ws_xpixel;
	unsigned short ws_ypixel;
};

#define TIOCGWINSZ 0x5413u
#define TIOCSWINSZ 0x5414u

/* Block device ioctls (Linux generic values). */
#define BLKGETSIZE64 0x80081272u
#define BLKFLSBUF 0x1261u

#endif
