#ifndef BRK_COMPILER_ATTR_H
#define BRK_COMPILER_ATTR_H

#define __must_check __attribute__((warn_unused_result))

#define __printf(fmt, arg) __attribute__((format(printf, fmt, arg)))

#define __packed __attribute__((packed))

#define __used __attribute__((__used__))

#define __section(section) __attribute__((__section__(section)))

#define __result(type) type __must_check

#endif
