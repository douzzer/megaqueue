/* MegaQueue
 *
 * Copyright 2015 Daniel Pouzzner
 *
 * mq_time.c  2015mar20  daniel.pouzzner@megaqueue.com
 * 
 * time processing routines
 */

#include "mq.h"

__wur int64_t MQ_Time_Now(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME,&ts)<0) {
    struct timeval tv;
    gettimeofday(&tv,0);
    return (int64_t)tv.tv_sec * 1000000000L + (int64_t)tv.tv_usec * 1000L;
  } else
    return (int64_t)ts.tv_sec * 1000000000L + (int64_t)ts.tv_nsec;
}

__wur int64_t MQ_Time_Now_Monotonic(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC,&ts)<0) {
    struct timeval tv;
    gettimeofday(&tv,0);
    return (int64_t)tv.tv_sec * 1000000000L + (int64_t)tv.tv_usec * 1000L;
  } else
    return (int64_t)ts.tv_sec * 1000000000L + (int64_t)ts.tv_nsec;
}
