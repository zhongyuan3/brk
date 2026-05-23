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

#endif
