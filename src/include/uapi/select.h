#ifndef UAPI_SELECT_H
#define UAPI_SELECT_H

#define FD_SETSIZE 1024

typedef unsigned long fd_mask;

#define FD_NFDBITS (8 * sizeof(fd_mask))

typedef struct {
	fd_mask fds_bits[FD_SETSIZE / FD_NFDBITS];
} fd_set;

#define FD_ELT(d) ((d) / FD_NFDBITS)
#define FD_MASK(d) ((fd_mask)1 << ((d) % FD_NFDBITS))

#define FD_ZERO(set) memset((set), 0, sizeof(fd_set))
#define FD_SET(d, set) ((set)->fds_bits[FD_ELT(d)] |= FD_MASK(d))
#define FD_CLR(d, set) ((set)->fds_bits[FD_ELT(d)] &= ~FD_MASK(d))
#define FD_ISSET(d, set) (((set)->fds_bits[FD_ELT(d)] & FD_MASK(d)) != 0)

#endif
