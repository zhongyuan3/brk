#ifndef BRK_TIMECONST_H
#define BRK_TIMECONST_H

#define NS_PER_SEC 1000000000ULL
#define NS_PER_MS 1000000ULL
#define NS_PER_US 1000ULL
#define US_PER_SEC 1000000ULL
#define US_PER_MS 1000ULL
#define MS_PER_SEC 1000ULL

/* Kernel tick rate and derived user-visible clock tick rate. */
#define HZ 1000
#define USER_HZ HZ
#define TICK_NS (NS_PER_SEC / HZ)

#define SEC_PER_MIN 60
#define MIN_PER_HOUR 60
#define HOUR_PER_DAY 24

#endif
