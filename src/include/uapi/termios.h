#ifndef UAPI_TERMIOS_H
#define UAPI_TERMIOS_H

#define NCCS 32

typedef unsigned char cc_t;
typedef unsigned int speed_t;
typedef unsigned int tcflag_t;

struct termios {
	tcflag_t c_iflag;
	tcflag_t c_oflag;
	tcflag_t c_cflag;
	tcflag_t c_lflag;
	cc_t c_line;
	cc_t c_cc[NCCS];
	speed_t c_ispeed;
	speed_t c_ospeed;
};

/* c_cc indices */
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

/* c_iflag */
#define IGNBRK 0000001u
#define BRKINT 0000002u
#define IGNPAR 0000004u
#define PARMRK 0000010u
#define INPCK 0000020u
#define ISTRIP 0000040u
#define INLCR 0000100u
#define IGNCR 0000200u
#define ICRNL 0000400u
#define IUCLC 0001000u
#define IXON 0002000u
#define IXANY 0004000u
#define IXOFF 0010000u
#define IMAXBEL 0020000u
#define IUTF8 0040000u

/* c_oflag */
#define OPOST 0000001u
#define OLCUC 0000002u
#define ONLCR 0000004u
#define OCRNL 0000010u
#define ONOCR 0000020u
#define ONLRET 0000040u
#define OFILL 0000100u
#define OFDEL 0000200u
#define NLDLY 0000400u
#define CRDLY 0003000u
#define TABDLY 00014000u
#define BSDLY 0020000u
#define VTDLY 0040000u
#define FFDLY 0100000u

/* c_cflag */
#define CBAUD 0010017u
#define B0 0000000u
#define B50 0000001u
#define B75 0000002u
#define B110 0000003u
#define B134 0000004u
#define B150 0000005u
#define B200 0000006u
#define B300 0000007u
#define B600 0000010u
#define B1200 0000011u
#define B1800 0000012u
#define B2400 0000013u
#define B4800 0000014u
#define B9600 0000015u
#define B19200 0000016u
#define B38400 0000017u
#define CSIZE 0000060u
#define CS5 0000000u
#define CS6 0000020u
#define CS7 0000040u
#define CS8 0000060u
#define CSTOPB 0000100u
#define CREAD 0000200u
#define PARENB 0000400u
#define PARODD 0001000u
#define HUPCL 0002000u
#define CLOCAL 0004000u
#define CBAUDEX 0010000u

/* c_lflag */
#define ISIG 0000001u
#define ICANON 0000002u
#define XCASE 0000004u
#define ECHO 0000010u
#define ECHOE 0000020u
#define ECHOK 0000040u
#define ECHONL 0000100u
#define NOFLSH 0000200u
#define TOSTOP 0000400u
#define ECHOCTL 0001000u
#define ECHOPRT 0002000u
#define ECHOKE 0004000u
#define FLUSHO 0010000u
#define PENDIN 0040000u
#define IEXTEN 0100000u

/* ioctls */
#define TCGETS 0x5401u
#define TCSETS 0x5402u
#define TCSETSW 0x5403u
#define TCSETSF 0x5404u
#define TCSBRK 0x5409u
#define TCXONC 0x540Au
#define TCFLSH 0x540Bu
#define TIOCINQ 0x541Bu
#define FIONREAD TIOCINQ

#define TCIFLUSH 0
#define TCOFLUSH 1
#define TCIOFLUSH 2

#endif
