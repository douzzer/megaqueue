/* MegaQueue
 *
 * Copyright 2015 Daniel Pouzzner
 *
 * mq_time.h  2015mar20  daniel.pouzzner@megaqueue.com
 * 
 * time processing routines
 */

#ifndef MQ_TIME_H
#define MQ_TIME_H

#include <sys/time.h>
#include <time.h>

typedef int64_t MQ_Time_t;
#define MQ_ONE_SECOND 1000000000L
#define MQ_ONE_MINUTE 60000000000L
#define MQ_NOWAITING 0L
#define MQ_DEFAULTTIME 0x7fffffffffffffffL
#define MQ_FOREVER 0x7ffffffffffffffeL

extern __wur int64_t MQ_Time_Now(void);
static __wur __always_inline int64_t MQ_Time_Now_inline(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME,&ts)<0) {
    struct timeval tv;
    gettimeofday(&tv,0);
    return (int64_t)tv.tv_sec * 1000000000L + (int64_t)tv.tv_usec * 1000L;
  } else
    return (int64_t)ts.tv_sec * 1000000000L + (int64_t)ts.tv_nsec;
}

extern __wur int64_t MQ_Time_Now_Monotonic(void);
static __wur __always_inline int64_t MQ_Time_Now_Monotonic_inline(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC,&ts)<0) {
    struct timeval tv;
    gettimeofday(&tv,0);
    return (int64_t)tv.tv_sec * 1000000000L + (int64_t)tv.tv_usec * 1000L;
  } else
    return (int64_t)ts.tv_sec * 1000000000L + (int64_t)ts.tv_nsec;
}

#if 0

/* glibc-2.21/nptl/pthread_cond_timedwait.c */


# ifdef __NR_clock_gettime
        INTERNAL_SYSCALL_DECL (err);
        (void) INTERNAL_VSYSCALL (clock_gettime, err, 2,
                                  (cond->__data.__nwaiters
                                   & ((1 << COND_NWAITERS_SHIFT) - 1)),
                                  &rt);
        /* Convert the absolute timeout value to a relative timeout.  */
        rt.tv_sec = abstime->tv_sec - rt.tv_sec;
        rt.tv_nsec = abstime->tv_nsec - rt.tv_nsec;
# else
        /* Get the current time.  So far we support only one clock.  */
        struct timeval tv;
        (void) __gettimeofday (&tv, NULL);

        /* Convert the absolute timeout value to a relative timeout.  */
        rt.tv_sec = abstime->tv_sec - tv.tv_sec;
        rt.tv_nsec = abstime->tv_nsec - tv.tv_usec * 1000;
# endif





#include <bits/libc-vdso.h>

static __always_inline MQ_clock_gettime(
        INTERNAL_SYSCALL_DECL (err);
        (void) INTERNAL_VSYSCALL (clock_gettime, err, 2,
                                  (cond->__data.__nwaiters
                                   & ((1 << COND_NWAITERS_SHIFT) - 1)),
                                  &rt);


static __wur __always_inline int64_t MQ_Time_Now() {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME,&ts)<0) {
    struct timeval tv;
    gettimeofday(&tv,0);
    return (int64_t)tv.tv_sec * 1000000000L + (int64_t)tv.tv_usec * 1000L;
  } else
    return (int64_t)ts.tv_sec * 1000000000L + (int64_t)ts.tv_nsec;
}
#endif

#endif
