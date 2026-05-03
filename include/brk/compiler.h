#ifndef BRK_COMPILER_H
#define BRK_COMPILER_H

#define __must_check __attribute__((warn_unused_result))
#define __maybe_unused __attribute__((unused))

#define __printf_format(fmt, args) __attribute__((format(printf, fmt, args)))

#endif