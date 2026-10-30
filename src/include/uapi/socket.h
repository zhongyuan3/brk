#ifndef UAPI_SOCKET_H
#define UAPI_SOCKET_H

typedef unsigned short sa_family_t;
typedef unsigned int socklen_t;

#define AF_UNSPEC 0
#define AF_INET 2

#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3

#define SOCK_NONBLOCK 00004000
#define SOCK_CLOEXEC 02000000

#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_ERROR 4
#define SO_SNDBUF 7
#define SO_RCVBUF 8

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

struct sockaddr {
	sa_family_t sa_family;
	char sa_data[14];
};

struct in_addr {
	unsigned int s_addr;
};

struct sockaddr_in {
	sa_family_t sin_family;
	unsigned short sin_port;
	struct in_addr sin_addr;
	unsigned char sin_zero[8];
};

#endif
