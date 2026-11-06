#ifndef UAPI_MMAN_H
#define UAPI_MMAN_H

/* Protection bits */
#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

/* Mapping flags */
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20

#define MAP_FAILED ((void *)-1)

/* msync flags */
#define MS_ASYNC 0x1
#define MS_INVALIDATE 0x2
#define MS_SYNC 0x4

#endif
