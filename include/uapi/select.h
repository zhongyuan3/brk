#ifndef UAPI_SELECT_H
#define UAPI_SELECT_H

#include <uapi/types.h>

#define FD_SETSIZE 1024

#define FD_NFDBITS (8 * sizeof(__brk_fd_mask_t))

typedef struct {
	__brk_fd_mask_t fds_bits[FD_SETSIZE / FD_NFDBITS];
} fd_set;

#define FD_ELT(d) ((d) / FD_NFDBITS)
#define FD_MASK(d) ((__brk_fd_mask_t)1 << ((d) % FD_NFDBITS))

#define FD_ZERO(set) __builtin_memset((set), 0, sizeof(fd_set))
#define FD_SET(d, set) ((set)->fds_bits[FD_ELT(d)] |= FD_MASK(d))
#define FD_CLR(d, set) ((set)->fds_bits[FD_ELT(d)] &= ~FD_MASK(d))
#define FD_ISSET(d, set) (((set)->fds_bits[FD_ELT(d)] & FD_MASK(d)) != 0)

#endif
