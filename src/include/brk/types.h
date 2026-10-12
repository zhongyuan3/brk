#ifndef BRK_TYPES_H
#define BRK_TYPES_H

#include <brk/static_assert.h>
#include <uapi/types.h>

typedef unsigned int fmode_t;
typedef int cpuid_t;
typedef unsigned int umode_t;
typedef unsigned int kuid_t;
typedef unsigned int kgid_t;

struct list_head {
	struct list_head *prev, *next;
};

struct hlist_head {
	struct hlist_node *first;
};

struct hlist_node {
	struct hlist_node *next, **pprev;
};

typedef unsigned long long paddr_t;

/*
 * printf() format specifiers for the fixed-width integer types, mirroring
 * the PRI* macros from <inttypes.h>.  The prefix length modifiers follow
 * the types picked by the compiler's <stdint.h> for the current ABI: on
 * the RV32 ILP32 toolchain int32_t/int_least32_t are 'long' and int64_t is
 * 'long long', while on RV64 LP64 they are 'int' and 'long' respectively.
 * The 8- and 16-bit types are promoted to 'int' when passed to printf(), so
 * their macros keep the 'hh'/'h' modifiers to truncate back to size.
 */
#if __riscv_xlen == 32
#define __PRIL32 "l"
#define __PRIL64 "ll"
#define __PRILPTR ""
#define __PRILMAX "ll"

typedef signed char i8;
typedef signed short i16;
typedef signed long i32;
typedef signed long long i64;

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;
typedef unsigned long long u64;
#else
#define __PRIL32 ""
#define __PRIL64 "l"
#define __PRILPTR "l"
#define __PRILMAX "l"

typedef signed char i8;
typedef signed short i16;
typedef signed int i32;
typedef signed long i64;

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
#endif

static_assert(sizeof(i8) == 1, "size of i8 is not equal to 1 byte");
static_assert(sizeof(i16) == 2, "size of i16 is not equal to 2 bytes");
static_assert(sizeof(i32) == 4, "size of i32 is not equal to 4 bytes");
static_assert(sizeof(i64) == 8, "size of i64 is not equal to 8 bytes");

#define PRId8 "hhd"
#define PRIi8 "hhi"
#define PRIo8 "hho"
#define PRIu8 "hhu"
#define PRIx8 "hhx"
#define PRIX8 "hhX"

#define PRId16 "hd"
#define PRIi16 "hi"
#define PRIo16 "ho"
#define PRIu16 "hu"
#define PRIx16 "hx"
#define PRIX16 "hX"

#define PRId32 __PRIL32 "d"
#define PRIi32 __PRIL32 "i"
#define PRIo32 __PRIL32 "o"
#define PRIu32 __PRIL32 "u"
#define PRIx32 __PRIL32 "x"
#define PRIX32 __PRIL32 "X"

#define PRId64 __PRIL64 "d"
#define PRIi64 __PRIL64 "i"
#define PRIo64 __PRIL64 "o"
#define PRIu64 __PRIL64 "u"
#define PRIx64 __PRIL64 "x"
#define PRIX64 __PRIL64 "X"

#define PRIdLEAST8 "hhd"
#define PRIiLEAST8 "hhi"
#define PRIoLEAST8 "hho"
#define PRIuLEAST8 "hhu"
#define PRIxLEAST8 "hhx"
#define PRIXLEAST8 "hhX"

#define PRIdLEAST16 "hd"
#define PRIiLEAST16 "hi"
#define PRIoLEAST16 "ho"
#define PRIuLEAST16 "hu"
#define PRIxLEAST16 "hx"
#define PRIXLEAST16 "hX"

#define PRIdLEAST32 __PRIL32 "d"
#define PRIiLEAST32 __PRIL32 "i"
#define PRIoLEAST32 __PRIL32 "o"
#define PRIuLEAST32 __PRIL32 "u"
#define PRIxLEAST32 __PRIL32 "x"
#define PRIXLEAST32 __PRIL32 "X"

#define PRIdLEAST64 __PRIL64 "d"
#define PRIiLEAST64 __PRIL64 "i"
#define PRIoLEAST64 __PRIL64 "o"
#define PRIuLEAST64 __PRIL64 "u"
#define PRIxLEAST64 __PRIL64 "x"
#define PRIXLEAST64 __PRIL64 "X"

#define PRIdFAST8 "d"
#define PRIiFAST8 "i"
#define PRIoFAST8 "o"
#define PRIuFAST8 "u"
#define PRIxFAST8 "x"
#define PRIXFAST8 "X"

#define PRIdFAST16 "d"
#define PRIiFAST16 "i"
#define PRIoFAST16 "o"
#define PRIuFAST16 "u"
#define PRIxFAST16 "x"
#define PRIXFAST16 "X"

#define PRIdFAST32 "d"
#define PRIiFAST32 "i"
#define PRIoFAST32 "o"
#define PRIuFAST32 "u"
#define PRIxFAST32 "x"
#define PRIXFAST32 "X"

#define PRIdFAST64 __PRIL64 "d"
#define PRIiFAST64 __PRIL64 "i"
#define PRIoFAST64 __PRIL64 "o"
#define PRIuFAST64 __PRIL64 "u"
#define PRIxFAST64 __PRIL64 "x"
#define PRIXFAST64 __PRIL64 "X"

#define PRIdPTR __PRILPTR "d"
#define PRIiPTR __PRILPTR "i"
#define PRIoPTR __PRILPTR "o"
#define PRIuPTR __PRILPTR "u"
#define PRIxPTR __PRILPTR "x"
#define PRIXPTR __PRILPTR "X"

#define PRIdMAX __PRILMAX "d"
#define PRIiMAX __PRILMAX "i"
#define PRIoMAX __PRILMAX "o"
#define PRIuMAX __PRILMAX "u"
#define PRIxMAX __PRILMAX "x"
#define PRIXMAX __PRILMAX "X"

#endif
