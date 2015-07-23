/* MegaQueue
 *
 * Copyright 2015 Daniel Pouzzner
 *
 * tests/test_rwlock.c  daniel.pouzzner@megaqueue.com
 */

#define MQ_RWLOCK_INLINE
#include "../mq.h"
#ifdef MQ_RWLOCK_INLINE
#include "../mq_rwlock.c"
#else
struct MQ_Lock_Wait_Ent {
  MQ_RWLock_t *l;
  MQ_Wait_Type what;
  volatile int granted;
  volatile int * volatile granted_ptr;
  void * volatile arg; /* passed by MQ_cond_signal() to MQ_cond_wait() */
  struct MQ_Lock_Wait_Ent * volatile _prev, * volatile _next;
  volatile MQ_TSA_t W_Locker_Saved;
  int64_t LFList_ID;
#ifndef MQ_NO_PTHREADS
  int async_origtype;
#endif
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
  const char *file;
  int line;
  const char *function;
#endif
};
#endif

#include <sys/signal.h>

#ifndef dprintf
#define dprintf(fd,format,args...) do { START_NOCANCELS; dprintf(fd,format,##args); END_NOCANCELS } while (0)
#endif

static volatile int testtot, testtot2, failtot;
static volatile int launchcount;

__thread_scoped const char *launch_file = 0;
__thread_scoped int launch_line = 0;
__thread_scoped const char *launch_function = 0;

#define set_launchpoint() ({ launch_file = __FILE__; launch_line = __LINE__; launch_function = __FUNCTION__; })

#define usleep(usec) do { struct timespec ts = { .tv_sec = 0, .tv_nsec = (usec)*1000 }; nanosleep(&ts,0); } while(0)

#ifdef MQ_RWLOCK_STATISTICS_SUPPORT
#define Decrement_Futex_Trips(lock,n) MQ_SyncInt_Decrement(lock.futex_trips,n)
#else
#define Decrement_Futex_Trips(lock,n) {}
#endif

static void *testroutine_mutex(void *arg) {
  set_launchpoint();
  int64_t ret;
  if ((ret=MQ_Mutex_Lock(arg,1000000L))<0) {
    if (ret != -ETIMEDOUT) {
      MQ_dprintf3("testroutine_mutex MQ_Mutex_Lock: %s\n",strerror((int)(-ret)));
      MQ_SyncInt_Increment(launchcount,1000);
abort();
    }
    goto out;
  }
  {
  int testtot_before_sleep = testtot;
  usleep(10);
  testtot = testtot_before_sleep + 1;
  if ((ret=MQ_Mutex_Unlock(arg))<0) {
    MQ_dprintf3("testroutine_mutex MQ_Mutex_Unlock: %s (locker=%d)\n",strerror((int)(-ret)),MQ_TSA_To_TID(((MQ_RWLock_t *)arg)->MutEx.Owner));
    MQ_SyncInt_Increment(launchcount,1000);
    goto out;
  }
  MQ_SyncInt_Increment(testtot2,1);
  }
 out:
  MQ_SyncInt_Increment(launchcount,1);
  return 0;
}

static void *testroutine_w(void *arg) {
  set_launchpoint();
  MQ_SyncInt_Increment(launchcount,1);
  int ret;
  if ((ret=MQ_W_Lock(arg,2L<<30))<0) {
    MQ_dprintf3("MQ_W_Lock in testroutine_w: %s\n",strerror((int)(-ret)));
    return 0;
  }
  int testtot_before_yield = testtot;
/* usleep(1); */
  sched_yield();
  testtot = testtot_before_yield + 1;
  if ((ret=MQ_W_Unlock(arg))<0) {
    MQ_dprintf3("MQ_W_Unlock in testroutine_w: %s\nR_Count=%u, W_Count=%u, MutEx=%d, LFList_Head=%p, Wait_List_Head=%p, n_waiters=%d\n",strerror((int)(-ret)),MQ_RWLOCK_R_COUNT((MQ_RWLock_t *)arg),MQ_RWLOCK_W_COUNT((MQ_RWLock_t *)arg),((MQ_RWLock_t *)arg)->MutEx.Count,
	    ((MQ_RWLock_t *)arg)->LFList_Head,/*((MQ_RWLock_t *)arg)->Wait_LL_futex,*/((MQ_RWLock_t *)arg)->Wait_List_Head,
	    MQ_RWLock_Cur_Waiter_Count_Unlocked((MQ_RWLock_t *)arg));
    return 0;
  }
  return 0;
}

static void *testroutine_r(void *arg) {
  set_launchpoint();
  int ret;

  MQ_SyncInt_Increment(testtot,1);

  if ((ret=MQ_R_InheritLock((MQ_RWLock_t *)arg))<0)
    MQ_dprintf3("testroutine_r MQ_R_InheritLock: %s (RW.Count=%x)\n",strerror((int)(-ret)),((MQ_RWLock_t *)arg)->RW.Count);

  if ((ret=MQ_R_Unlock(arg))<0) {
    MQ_dprintf3("MQ_R_Unlock in testroutine_r: %s\n",strerror((int)(-ret)));
    return 0;
  }
  return 0;
}

static void *testroutine_r2(void *arg) {
  set_launchpoint();
  int ret;
  MQ_SyncInt_Increment(launchcount,1);
  if ((ret=MQ_R_Lock(arg,1L<<30))<0) {
    MQ_dprintf3("MQ_R_Lock in testroutine_r2: %s\n",strerror((int)(-ret)));
    return 0;
  }
  MQ_SyncInt_Increment(testtot,1);
  for (int i=0; i<10; ++i) {
    if ((ret=MQ_R_Lock(arg,1L<<30))<0) {
      MQ_dprintf3("inner MQ_R_Lock in testroutine_r2: %s\n",strerror((int)(-ret)));
      return 0;
    }
    if ((ret=MQ_R_Unlock(arg))<0) {
      MQ_dprintf3("inner MQ_R_Unlock in testroutine_r2: %s\n",strerror((int)(-ret)));
      return 0;
    }
  }
  if ((ret=MQ_R_Unlock(arg))<0) {
    MQ_dprintf3("MQ_R_Unlock in testroutine_r2: %s\n",strerror((int)(-ret)));
    return 0;
  }
  return 0;
}

static void testroutine_cond_cleanup(MQ_RWLock_t *l) {
  int ret;
  if ((ret=MQ_R_Unlock(l))<0)
    MQ_dprintf3("MQ_R_Unlock in testroutine_cond: %s\n",strerror((int)(-ret)));
}

static void *testroutine_cond(void *arg) {
  set_launchpoint();
  void *retarg;
  int ret;

  if ((ret=MQ_R_InheritLock((MQ_RWLock_t *)arg))<0)
    MQ_dprintf3("testroutine_cond MQ_R_InheritLock: %s\n",strerror((int)(-ret)));

  pthread_cleanup_push(testroutine_cond_cleanup,arg);

  if ((ret=MQ_Cond_Wait(arg, MQ_FOREVER, &retarg, MQ_NOFLAGS)) < 0)
    MQ_dprintf3("testroutine_cond MQ_Cond_Wait: %s\n",strerror((int)(-ret)));

  pthread_cleanup_pop(1);

#ifdef MQ_DEBUG_MORE
  dprintf(STDERR_FILENO,"testroutine_cond done, retarg = %p\n",retarg);
#endif

  return retarg;
}

static void *testroutine_cond_w(void *arg) {
  set_launchpoint();
  void *retarg;
  int ret;

  if ((ret=MQ_W_InheritLock((MQ_RWLock_t *)arg))<0)
    MQ_dprintf3("testroutine_cond_w MQ_W_InheritLock: %s\n",strerror((int)(-ret)));

  if ((ret=MQ_Cond_Wait(arg, MQ_FOREVER, &retarg, 0UL)) < 0)
    MQ_dprintf3("testroutine_cond_w MQ_Cond_Wait: %s\n",strerror((int)(-ret)));

  if ((ret=MQ_W_Unlock(arg))<0) {
    MQ_dprintf3("MQ_W_Unlock in testroutine_cond_w: %s\n",strerror((int)(-ret)));
    return 0;
  }

#ifdef MQ_DEBUG_MORE
  dprintf(STDERR_FILENO,"testroutine_cond_w done, retarg = %p\n",retarg);
#endif

  return retarg;
}

static void *testroutine_signal_w(void *arg) {
  set_launchpoint();
  int ret;

  if ((ret=MQ_W_Lock(arg,10L*MQ_ONE_SECOND))<0) {
    MQ_dprintf3("testroutine_signal_w MQ_W_Lock got %s\n",strerror((int)(-ret)));
    return (void *)(intptr_t)ret;
  }

  ret = MQ_Cond_Signal(arg, 1, (void *)0xdead6005eUL, MQ_COND_RETURN_UNLOCKED);
  if (ret<0) {
    MQ_dprintf3("testroutine_signal MQ_Cond_Signal got %s\n",strerror((int)(-ret)));
    return (void *)(intptr_t)ret;
  }

  return 0;
}

static void testroutine_cond_mutex_cleanup(MQ_RWLock_t *l) {
  int ret;
  if ((ret=MQ_Mutex_Unlock(l))<0)
    MQ_dprintf3("MQ_Mutex_Unlock in testroutine_cond_mutex: %s\n",strerror((int)(-ret)));
}

static void *testroutine_cond_mutex(void *arg) {
  set_launchpoint();
  void *retarg;
  int ret;

  if ((ret=MQ_Mutex_InheritLock((MQ_RWLock_t *)arg))<0)
    MQ_dprintf3("testroutine_cond_mutex MQ_Mutex_InheritLock: %s\n",strerror((int)(-ret)));

  pthread_cleanup_push(testroutine_cond_mutex_cleanup,arg);

  if ((ret=MQ_Cond_Wait(arg, MQ_FOREVER, &retarg, 0UL)) < 0)
    MQ_dprintf3("testroutine_cond_mutex MQ_Cond_Wait: %s\n",strerror((int)(-ret)));

  pthread_cleanup_pop(1);

#ifdef MQ_DEBUG_MORE
  dprintf(STDERR_FILENO,"testroutine_cond_mutex done, retarg = %p\n",retarg);
#endif

  return retarg;
}

static void *testroutine_cond_mutex_returnunlocked(void *arg) {
  set_launchpoint();
  void *retarg;
  int ret;

  if ((ret=MQ_Mutex_InheritLock((MQ_RWLock_t *)arg))<0)
    MQ_dprintf3("testroutine_cond_mutex MQ_Mutex_InheritLock: %s\n",strerror((int)(-ret)));

  if ((ret=MQ_Cond_Wait(arg, MQ_FOREVER, &retarg, MQ_COND_RETURN_UNLOCKED)) < 0)
    MQ_dprintf3("testroutine_cond_mutex MQ_Cond_Wait: %s\n",strerror((int)(-ret)));

#ifdef MQ_DEBUG_MORE
  dprintf(STDERR_FILENO,"testroutine_cond_mutex done, retarg = %p\n",retarg);
#endif

  return retarg;
}

static void testroutine_cond2or3_cleanup(MQ_RWLock_t *l) {
  int ret;
  if ((ret=MQ_W_Unlock(l))<0)
    MQ_dprintf3("MQ_W_Unlock in testroutine_cond2or3: %s\n",strerror((int)(-ret)));
}

static void *testroutine_cond2(void *arg) {
  set_launchpoint();
  void *retarg;
  int ret;

  if ((ret=MQ_W_InheritLock((MQ_RWLock_t *)arg))<0)
    MQ_dprintf3("MQ_W_InheritLock: %s\n",strerror((int)(-ret)));

  pthread_cleanup_push(testroutine_cond2or3_cleanup,arg);

  if ((ret=MQ_Cond_Wait(arg, MQ_FOREVER, &retarg, 0UL)) < 0)
    MQ_dprintf3("testroutine_cond2 MQ_Cond_Wait: %s\n",strerror((int)(-ret)));

  pthread_cleanup_pop(1);

#ifdef MQ_DEBUG_MORE
  dprintf(STDERR_FILENO,"testroutine_cond2 done, retarg = %p\n",retarg);
#endif

  return retarg;
}

static void *testroutine_cond3(void *arg) {
  set_launchpoint();
  void *retarg;
  int ret;

  if ((ret=MQ_W_Lock(arg,1L<<30))<0) {
    MQ_dprintf3("MQ_W_Lock in testroutine_cond3: %s\n",strerror((int)(-ret)));
    return 0;
  }

  pthread_cleanup_push(testroutine_cond2or3_cleanup,arg);

  if ((ret=MQ_Cond_Wait(arg, MQ_FOREVER, &retarg, 0UL)) < 0)
    MQ_dprintf3("testroutine_cond3 MQ_Cond_Wait: %s\n",strerror((int)(-ret)));

  pthread_cleanup_pop(1);

#ifdef MQ_DEBUG_MORE
  dprintf(STDERR_FILENO,"testroutine_cond3 done, retarg = %p\n",retarg);
#endif

  return retarg;
}

static int exitflag = 0;
static void handle_sigint(__unused int sig) {
  exitflag = 1;
}

int main(int argc, char **argv) {
  int times = 0, times_max;
  int64_t ret;

  if ((argc != 2) || ((times_max = atoi(argv[1])) < 0)) {
    dprintf(STDERR_FILENO,"usage: %s <iterations> (zero for benchmarks)\n",argv[0]);
    exit(1);
  }

  MQ_RWLock_t testlock = MQ_RWLOCK_INITIALIZER(MQ_RWLOCK_DEFAULTFLAGS);
#if 0
  if ((ret=MQ_RWLock_Init(&testlock,MQ_RWLOCK_DEFAULTFLAGS)) < 0) {
    dprintf(STDERR_FILENO,"MQ_RWLock_Init: %s\n",strerror((int)(-ret)));
    exit(1);
  }
#endif
  dprintf(STDERR_FILENO,"compiled-in rwlock flags: 0x%lx\n",testlock.Flags&((MQ_RWLOCK_MAXUSERFLAG<<1UL)-1UL));

  {
    struct sigaction act = { { handle_sigint }, { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } }, (int)(SA_SIGINFO|SA_RESETHAND), 0 };
    if (sigaction(SIGINT,&act,(struct sigaction *)0)<0) {
      dprintf(STDERR_FILENO,"sigaction: %s",strerror(errno));
      exit(1);
    }
    if (sigaction(SIGTERM,&act,(struct sigaction *)0)<0) {
      dprintf(STDERR_FILENO,"sigaction: %s",strerror(errno));
      exit(1);
    }
  }

  int i;
  if (times_max == 0) {
#define BENCHMARKCYCLES 10000000
/*#define DO_MIDDLE_FENCE*/
    MQ_Time_t start, end;

    /* for benchmarks, disable the lock flags (still some overhead to check the flags, unless we get lucky and the compiler optimizes out the checks) */
    if (__MQ_RWLock_LocalBuildFlags) {
#ifdef BENCHOPTIONS
      dprintf(STDERR_FILENO,"warning, leaving local build flags (0x%lx) in place for benchmarks\n",testlock.Flags&((MQ_RWLOCK_MAXUSERFLAG<<1UL)-1UL));
#else
      dprintf(STDERR_FILENO,"warning, disabling local build flags (0x%lx) for benchmarks, but compiled-in option-testing overhead will remain\n",__MQ_RWLock_LocalBuildFlags);

      if ((ret=MQ_RWLock_Init(&testlock,0)) < 0) {
	MQ_dprintf3("MQ_RWLock_Init: %s\n",strerror((int)(-ret)));
	exit(1);
      }
#endif
    }

    MQ_Time_t overhead;
    volatile int testint = 0;
    end = MQ_Time_Now();
    start = MQ_Time_Now();
    overhead = start-end;
    for (i=BENCHMARKCYCLES*10;i;--i)
      if (testint)
	break;
    end = MQ_Time_Now();
    dprintf(STDERR_FILENO,"%.5fns per round for minimal unlocked spin iteration\n",((double)((end-start)-overhead)) / ((double)BENCHMARKCYCLES*10));

    start = MQ_Time_Now();
    end = MQ_Time_Now();
    overhead = end-start;
    start = MQ_Time_Now();
    for (i=0;i<BENCHMARKCYCLES/10;++i)
      sched_yield();
    end = MQ_Time_Now();
    overhead += end-start;
    dprintf(STDERR_FILENO,"%.3fns per round for sched_yield()\n",((double)(end-start)) / (double)(BENCHMARKCYCLES/10));

    volatile MQ_Time_t ret2;

    ret2 = start = MQ_Time_Now();
    for (i=BENCHMARKCYCLES;i;--i) {
      if ((--ret2)<0)
	break;
#ifdef DO_MIDDLE_FENCE
      MQ_SyncInt_Fence_Full();
#endif
      if ((--ret2)<0)
	break;
      if ((--ret2)<0)
	break;
      MQ_SyncInt_Fence_Full();
    }
    end = MQ_Time_Now();
    overhead = end-start;
    if (ret2<0) {
      /* this can't actually happen but tricks the compiler into using ret instead of optimizing out the overhead loop entirely */
      dprintf(STDERR_FILENO,"ret2 went negative: %ld\n",ret2);
      exit(1);
    }
    dprintf(STDERR_FILENO,"%.3fns per round benchmarking overhead\n",((double)(end-start)) / (double)BENCHMARKCYCLES);

    pthread_mutex_t pmutex = PTHREAD_MUTEX_INITIALIZER;
    ret2 = start = MQ_Time_Now();
    for (i=BENCHMARKCYCLES;i;--i) {
      if ((--ret2)<0)
	break;
      if ((ret=pthread_mutex_lock(&pmutex))!=0)
	break;
      if ((--ret2)<0)
	break;
#ifdef DO_MIDDLE_FENCE
      MQ_SyncInt_Fence_Full();
#endif
      if ((ret=pthread_mutex_unlock(&pmutex))!=0)
	break;
      if ((--ret2)<0)
	break;
      MQ_SyncInt_Fence_Full();
    }
    if (ret) {
      dprintf(STDERR_FILENO,"pthread_mutex_{lock,unlock}: %s\n",strerror((int)ret));
      exit(1);
    }
    end = MQ_Time_Now() - overhead;
    if (ret2<0) {
      /* this can't actually happen but tricks the compiler into using ret instead of optimizing out the overhead loop entirely */
      dprintf(STDERR_FILENO,"ret2 went negative: %ld\n",ret2);
      exit(1);
    }
    dprintf(STDERR_FILENO,"%.3fns per pthread_mutex_{lock,unlock}\n",((double)(end-start)) / (double)BENCHMARKCYCLES);

    ret2 = start = MQ_Time_Now();
    for (i=BENCHMARKCYCLES;i;--i) {
      if ((--ret2)<0)
	break;
      if ((ret=MQ_Mutex_Lock(&testlock,MQ_FOREVER))<0)
	break;
      if ((--ret2)<0)
	break;
#ifdef DO_MIDDLE_FENCE
      MQ_SyncInt_Fence_Full();
#endif
      if ((ret=MQ_Mutex_Unlock(&testlock))<0)
	break;
      if ((--ret2)<0)
	break;
      MQ_SyncInt_Fence_Full();
    }
    if (ret) {
      MQ_dprintf3("MQ_Mutex_{Lock,Unlock}: %s\n",strerror((int)(-ret)));
      exit(1);
    }
    end = MQ_Time_Now() - overhead;
    if (ret2<0) {
      /* this can't actually happen but tricks the compiler into using ret instead of optimizing out the overhead loop entirely */
      dprintf(STDERR_FILENO,"ret2 went negative: %ld\n",ret2);
      exit(1);
    }
    dprintf(STDERR_FILENO,"%.3fns per MQ_Mutex_{Lock,Unlock}\n",((double)(end-start)) / (double)BENCHMARKCYCLES);

    ret2 = start = MQ_Time_Now();
    for (i=BENCHMARKCYCLES;i;--i) {
      if ((--ret2)<0)
	break;
      if ((ret=MQ_Mutex_Lock_Fast(&testlock,MQ_FOREVER))<0)
	break;
      if ((--ret2)<0)
	break;
#ifdef DO_MIDDLE_FENCE
      MQ_SyncInt_Fence_Full();
#endif
      if ((ret=MQ_Mutex_Unlock_Fast(&testlock))<0)
	break;
      if ((--ret2)<0)
	break;
      MQ_SyncInt_Fence_Full();
    }
    if (ret) {
      MQ_dprintf3("MQ_Mutex_{Lock,Unlock}_Fast: %s\n",strerror((int)(-ret)));
      exit(1);
    }
    end = MQ_Time_Now() - overhead;
    if (ret2<0) {
      /* this can't actually happen but tricks the compiler into using ret instead of optimizing out the overhead loop entirely */
      dprintf(STDERR_FILENO,"ret2 went negative: %ld\n",ret2);
      exit(1);
    }
    dprintf(STDERR_FILENO,"%.3fns per MQ_Mutex_{Lock,Unlock}_Fast\n",((double)(end-start)) / (double)BENCHMARKCYCLES);

    ret2 = start = MQ_Time_Now();
    for (i=BENCHMARKCYCLES;i;--i) {
      if ((--ret2)<0)
	break;
      if ((ret=pthread_mutex_lock(&pmutex))!=0)
	break;
      if ((--ret2)<0)
	break;
#ifdef DO_MIDDLE_FENCE
      MQ_SyncInt_Fence_Full();
#endif
      if ((ret=pthread_mutex_unlock(&pmutex))!=0)
	break;
      if ((--ret2)<0)
	break;
      MQ_SyncInt_Fence_Full();
    }
    if (ret) {
      dprintf(STDERR_FILENO,"pthread_mutex_{lock,unlock}: %s\n",strerror((int)ret));
      exit(1);
    }
    end = MQ_Time_Now() - overhead;
    if (ret2<0) {
      /* this can't actually happen but tricks the compiler into using ret instead of optimizing out the overhead loop entirely */
      dprintf(STDERR_FILENO,"ret2 went negative: %ld\n",ret2);
      exit(1);
    }
    dprintf(STDERR_FILENO,"%.3fns per pthread_mutex_{lock,unlock}, second run\n",((double)(end-start)) / (double)BENCHMARKCYCLES);


    pthread_mutexattr_t attr;
    if ((ret=pthread_mutexattr_init(&attr)) != 0) {
      MQ_dprintf3("pthread_mutexattr_init: %s\n",strerror((int)ret));
      exit(1);
    }
    if ((ret=pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT)) != 0) {
      MQ_dprintf3("pthread_mutexattr_setprotocol(PTHREAD_PRIO_INHERIT): %s\n",strerror((int)ret));
      exit(1);
    }
    pthread_mutex_destroy(&pmutex);
    if ((ret=pthread_mutex_init(&pmutex, &attr)) != 0) {
      MQ_dprintf3("pthread_mutex_init: %s\n",strerror((int)ret));
      exit(1);
    }

    ret2 = start = MQ_Time_Now();
    for (i=BENCHMARKCYCLES;i;--i) {
      if ((--ret2)<0)
	break;
      if ((ret=pthread_mutex_lock(&pmutex))!=0)
	break;
      if ((--ret2)<0)
	break;
#ifdef DO_MIDDLE_FENCE
      MQ_SyncInt_Fence_Full();
#endif
      if ((ret=pthread_mutex_unlock(&pmutex))!=0)
	break;
      if ((--ret2)<0)
	break;
      MQ_SyncInt_Fence_Full();
    }
    if (ret) {
      MQ_dprintf3("pthread_mutex_{lock,unlock}: %s\n",strerror((int)ret));
      exit(1);
    }
    end = MQ_Time_Now() - overhead;
    if (ret2<0) {
      /* this can't actually happen but tricks the compiler into using ret instead of optimizing out the overhead loop entirely */
      MQ_dprintf3("ret2 went negative: %ld\n",ret2);
      exit(1);
    }
    dprintf(STDERR_FILENO,"%.3fns per pthread_mutex_{lock,unlock}, PTHREAD_PRIO_INHERIT\n",((double)(end-start)) / (double)BENCHMARKCYCLES);


    if ((ret=MQ_SchedState_Set(0, SCHED_RR, 1))<0) {
      MQ_dprintf3("MQ_SchedState_Set(0, SCHED_RR, 1): %s (skipping PTHREAD_PRIO_PROTECT benchmark)\n",strerror((int)-ret));
      goto skip_prio_protect;
    }

    if ((ret=pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_PROTECT)) != 0) {
      MQ_dprintf3("pthread_mutexattr_setprotocol(PTHREAD_PRIO_PROTECT): %s\n",strerror((int)ret));
      exit(1);
    }
    if ((ret=pthread_mutexattr_setprioceiling(&attr, 1)) != 0) {
      MQ_dprintf3("pthread_mutexattr_setprioceiling(0): %s\n",strerror((int)ret));
      exit(1);
    }
    pthread_mutex_destroy(&pmutex);
    if ((ret=pthread_mutex_init(&pmutex, &attr)) != 0) {
      MQ_dprintf3("pthread_mutex_init: %s\n",strerror((int)ret));
      exit(1);
    }

    ret2 = start = MQ_Time_Now();
    for (i=BENCHMARKCYCLES;i;--i) {
      if ((--ret2)<0)
	break;
      if ((ret=pthread_mutex_lock(&pmutex))!=0)
	break;
      if ((--ret2)<0)
	break;
#ifdef DO_MIDDLE_FENCE
      MQ_SyncInt_Fence_Full();
#endif
      if ((ret=pthread_mutex_unlock(&pmutex))!=0)
	break;
      if ((--ret2)<0)
	break;
      MQ_SyncInt_Fence_Full();
    }
    if (ret) {
      MQ_dprintf3("pthread_mutex_{lock,unlock}: %s\n",strerror((int)ret));
      exit(1);
    }
    end = MQ_Time_Now() - overhead;
    if (ret2<0) {
      /* this can't actually happen but tricks the compiler into using ret instead of optimizing out the overhead loop entirely */
      MQ_dprintf3("ret2 went negative: %ld\n",ret2);
      exit(1);
    }
    dprintf(STDERR_FILENO,"%.3fns per pthread_mutex_{lock,unlock}, PTHREAD_PRIO_PROTECT\n",((double)(end-start)) / (double)BENCHMARKCYCLES);


    if ((ret=MQ_SchedState_Set(0, SCHED_OTHER, 20))<0) {
      MQ_dprintf3("MQ_SchedState_Set(0, SCHED_OTHER, 20): %s\n",strerror((int)-ret));
      exit(1);
    }

  skip_prio_protect:

    if ((ret=pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_NONE)) != 0) {
      MQ_dprintf3("pthread_mutexattr_setprotocol(PTHREAD_PRIO_NONE): %s\n",strerror((int)ret));
      exit(1);
    }
    if ((ret=pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK)) != 0) {
      MQ_dprintf3("pthread_mutexattr_settype(PTHREAD_MUTEX_ERRORCHECK): %s\n",strerror((int)ret));
      exit(1);
    }
    pthread_mutex_destroy(&pmutex);
    if ((ret=pthread_mutex_init(&pmutex, &attr)) != 0) {
      MQ_dprintf3("pthread_mutex_init: %s\n",strerror((int)ret));
      exit(1);
    }

    ret2 = start = MQ_Time_Now();
    for (i=BENCHMARKCYCLES;i;--i) {
      if ((--ret2)<0)
	break;
      if ((ret=pthread_mutex_lock(&pmutex))!=0)
	break;
      if ((--ret2)<0)
	break;
#ifdef DO_MIDDLE_FENCE
      MQ_SyncInt_Fence_Full();
#endif
      if ((ret=pthread_mutex_unlock(&pmutex))!=0)
	break;
      if ((--ret2)<0)
	break;
      MQ_SyncInt_Fence_Full();
    }
    if (ret) {
      dprintf(STDERR_FILENO,"pthread_mutex_{lock,unlock}: %s\n",strerror((int)ret));
      exit(1);
    }
    end = MQ_Time_Now() - overhead;
    if (ret2<0) {
      /* this can't actually happen but tricks the compiler into using ret instead of optimizing out the overhead loop entirely */
      dprintf(STDERR_FILENO,"ret2 went negative: %ld\n",ret2);
      exit(1);
    }
    dprintf(STDERR_FILENO,"%.3fns per pthread_mutex_{lock,unlock}, PTHREAD_MUTEX_ERRORCHECK\n",((double)(end-start)) / (double)BENCHMARKCYCLES);

    ret2 = start = MQ_Time_Now();
    for (i=BENCHMARKCYCLES;i;--i) {
      if ((--ret2)<0)
	break;
      if ((ret=MQ_Mutex_Lock(&testlock,MQ_FOREVER))<0)
	break;
      if ((--ret2)<0)
	break;
#ifdef DO_MIDDLE_FENCE
      MQ_SyncInt_Fence_Full();
#endif
      if ((ret=MQ_Unlock(&testlock))<0)
	break;
      if ((--ret2)<0)
	break;
      MQ_SyncInt_Fence_Full();
    }
    if (ret) {
      MQ_dprintf3("MQ_Mutex_Lock, MQ_Unlock: %s\n",strerror((int)(-ret)));
      exit(1);
    }
    end = MQ_Time_Now() - overhead;
    if (ret2<0) {
      /* this can't actually happen but tricks the compiler into using ret instead of optimizing out the overhead loop entirely */
      dprintf(STDERR_FILENO,"ret2 went negative: %ld\n",ret2);
      exit(1);
    }
    dprintf(STDERR_FILENO,"%.3fns per MQ_Mutex_Lock, MQ_Unlock\n",((double)(end-start)) / (double)BENCHMARKCYCLES);

    ret2 = start = MQ_Time_Now();
    for (i=BENCHMARKCYCLES;i;--i) {
      if ((--ret2)<0)
	break;
      if ((ret=MQ_W_Lock(&testlock,MQ_FOREVER))<0)
	break;
      if ((--ret2)<0)
	break;
#ifdef DO_MIDDLE_FENCE
      MQ_SyncInt_Fence_Full();
#endif
      if ((ret=MQ_W_Unlock(&testlock))<0)
	break;
      if ((--ret2)<0)
	break;
      MQ_SyncInt_Fence_Full();
    }
    if (ret) {
      MQ_dprintf3("MQ_W_{Lock,Unlock}: %s\n",strerror((int)(-ret)));
      exit(1);
    }
    end = MQ_Time_Now() - overhead;
    if (ret2<0) {
      /* this can't actually happen but tricks the compiler into using ret instead of optimizing out the overhead loop entirely */
      dprintf(STDERR_FILENO,"ret2 went negative: %ld\n",ret2);
      exit(1);
    }
    dprintf(STDERR_FILENO,"%.3fns per MQ_W_{Lock,Unlock}\n",((double)(end-start)) / (double)BENCHMARKCYCLES);

    ret2 = start = MQ_Time_Now();
    for (i=BENCHMARKCYCLES;i;--i) {
      if ((--ret2)<0)
	break;
      if ((ret=MQ_W_Lock(&testlock,MQ_FOREVER))<0)
	break;
      if ((--ret2)<0)
	break;
#ifdef DO_MIDDLE_FENCE
      MQ_SyncInt_Fence_Full();
#endif
      if ((ret=MQ_W2R_Lock(&testlock))<0)
	break;
      if ((--ret2)<0)
	break;
      if ((ret=MQ_R_Unlock(&testlock))<0)
	break;
      MQ_SyncInt_Fence_Full();
    }
    if (ret) {
      MQ_dprintf3("MQ_W_Lock, MQ_W2R_Lock, MQ_R_Unlock: %s\n",strerror((int)(-ret)));
      exit(1);
    }
    end = MQ_Time_Now() - overhead;
    if (ret2<0) {
      /* this can't actually happen but tricks the compiler into using ret instead of optimizing out the overhead loop entirely */
      dprintf(STDERR_FILENO,"ret2 went negative: %ld\n",ret2);
      exit(1);
    }
    dprintf(STDERR_FILENO,"%.3fns per MQ_W_Lock, MQ_W2R_Lock, MQ_R_Unlock\n",((double)(end-start)) / (double)BENCHMARKCYCLES);

    ret2 = start = MQ_Time_Now();
    for (i=BENCHMARKCYCLES;i;--i) {
      if ((--ret2)<0)
	break;
      if ((ret=MQ_W_Lock(&testlock,MQ_FOREVER))<0)
	break;
      if ((--ret2)<0)
	break;
#ifdef DO_MIDDLE_FENCE
      MQ_SyncInt_Fence_Full();
#endif
      if ((ret=MQ_Unlock(&testlock))<0)
	break;
      if ((--ret2)<0)
	break;
      MQ_SyncInt_Fence_Full();
    }
    if (ret) {
      MQ_dprintf3("MQ_W_Lock, MQ_Unlock: %s\n",strerror((int)(-ret)));
      exit(1);
    }
    end = MQ_Time_Now() - overhead;
    if (ret2<0) {
      /* this can't actually happen but tricks the compiler into using ret instead of optimizing out the overhead loop entirely */
      dprintf(STDERR_FILENO,"ret2 went negative: %ld\n",ret2);
      exit(1);
    }
    dprintf(STDERR_FILENO,"%.3fns per MQ_W_Lock, MQ_Unlock\n",((double)(end-start)) / (double)BENCHMARKCYCLES);

    ret2 = start = MQ_Time_Now();
    for (i=BENCHMARKCYCLES;i;--i) {
      if ((--ret2)<0)
	break;
      if ((ret=MQ_R_Lock(&testlock,MQ_FOREVER))<0)
	break;
      if ((--ret2)<0)
	break;
#ifdef DO_MIDDLE_FENCE
      MQ_SyncInt_Fence_Full();
#endif
      if ((ret=MQ_R_Unlock(&testlock))<0)
	break;
      if ((--ret2)<0)
	break;
      MQ_SyncInt_Fence_Full();
    }
    if (ret) {
      MQ_dprintf3("MQ_R_{Lock,Unlock}: %s\n",strerror((int)(-ret)));
      exit(1);
    }
    end = MQ_Time_Now() - overhead;
    if (ret2<0) {
      /* this can't actually happen but tricks the compiler into using ret instead of optimizing out the overhead loop entirely */
      dprintf(STDERR_FILENO,"ret2 went negative: %ld\n",ret2);
      exit(1);
    }
    dprintf(STDERR_FILENO,"%.3fns per MQ_R_{Lock,Unlock}\n",((double)(end-start)) / (double)BENCHMARKCYCLES);

    ret2 = start = MQ_Time_Now();
    for (i=BENCHMARKCYCLES;i;--i) {
      if ((--ret2)<0)
	break;
      if ((ret=MQ_R_Lock(&testlock,MQ_FOREVER))<0)
	break;
      if ((--ret2)<0)
	break;
#ifdef DO_MIDDLE_FENCE
      MQ_SyncInt_Fence_Full();
#endif
      if ((ret=MQ_R2W_Lock(&testlock,MQ_FOREVER))<0)
	break;
      if ((--ret2)<0)
	break;
      if ((ret=MQ_W_Unlock(&testlock))<0)
	break;
      MQ_SyncInt_Fence_Full();
    }
    if (ret) {
      MQ_dprintf3("MQ_R_Lock, MQ_R2W_Lock, MQ_W_Unlock: %s\n",strerror((int)(-ret)));
      exit(1);
    }
    end = MQ_Time_Now() - overhead;
    if (ret2<0) {
      /* this can't actually happen but tricks the compiler into using ret instead of optimizing out the overhead loop entirely */
      dprintf(STDERR_FILENO,"ret2 went negative: %ld\n",ret2);
      exit(1);
    }
    dprintf(STDERR_FILENO,"%.3fns per MQ_R_Lock, MQ_R2W_Lock, MQ_W_Unlock\n",((double)(end-start)) / (double)BENCHMARKCYCLES);

    ret2 = start = MQ_Time_Now();
    for (i=BENCHMARKCYCLES;i;--i) {
      if ((--ret2)<0)
	break;
      if ((ret=MQ_R_Lock(&testlock,MQ_FOREVER))<0)
	break;
      if ((--ret2)<0)
	break;
#ifdef DO_MIDDLE_FENCE
      MQ_SyncInt_Fence_Full();
#endif
      if ((ret=MQ_Unlock(&testlock))<0)
	break;
      if ((--ret2)<0)
	break;
      MQ_SyncInt_Fence_Full();
    }
    if (ret) {
      MQ_dprintf3("MQ_R_Lock, MQ_Unlock: %s\n",strerror((int)(-ret)));
      exit(1);
    }
    end = MQ_Time_Now() - overhead;
    if (ret2<0) {
      /* this can't actually happen but tricks the compiler into using ret instead of optimizing out the overhead loop entirely */
      dprintf(STDERR_FILENO,"ret2 went negative: %ld\n",ret2);
      exit(1);
    }
    dprintf(STDERR_FILENO,"%.3fns per MQ_R_Lock, MQ_Unlock\n",((double)(end-start)) / (double)BENCHMARKCYCLES);

    pthread_rwlock_t prwlock = PTHREAD_RWLOCK_INITIALIZER;
    ret2 = start = MQ_Time_Now();
    for (i=BENCHMARKCYCLES;i;--i) {
      if ((--ret2)<0)
	break;
      if ((ret=pthread_rwlock_rdlock(&prwlock))!=0)
	break;
      if ((--ret2)<0)
	break;
#ifdef DO_MIDDLE_FENCE
      MQ_SyncInt_Fence_Full();
#endif
      if ((ret=pthread_rwlock_unlock(&prwlock))!=0)
	break;
      if ((--ret2)<0)
	break;
      MQ_SyncInt_Fence_Full();
    }
    if (ret) {
      dprintf(STDERR_FILENO,"pthread_rwlock_{rdlock,unlock}: %s\n",strerror((int)ret));
      exit(1);
    }
    end = MQ_Time_Now() - overhead;
    if (ret2<0) {
      /* this can't actually happen but tricks the compiler into using ret instead of optimizing out the overhead loop entirely */
      dprintf(STDERR_FILENO,"ret2 went negative: %ld\n",ret2);
      exit(1);
    }
    dprintf(STDERR_FILENO,"%.3fns per pthread_rwlock_{rdlock,unlock}\n",((double)(end-start)) / (double)BENCHMARKCYCLES);

    ret2 = start = MQ_Time_Now();
    for (i=BENCHMARKCYCLES;i;--i) {
      if ((--ret2)<0)
	break;
      if ((ret=pthread_rwlock_wrlock(&prwlock))!=0)
	break;
      if ((--ret2)<0)
	break;
#ifdef DO_MIDDLE_FENCE
      MQ_SyncInt_Fence_Full();
#endif
      if ((ret=pthread_rwlock_unlock(&prwlock))!=0)
	break;
      if ((--ret2)<0)
	break;
      MQ_SyncInt_Fence_Full();
    }
    if (ret) {
      dprintf(STDERR_FILENO,"pthread_rwlock_{wrlock,unlock}: %s\n",strerror((int)ret));
      exit(1);
    }
    end = MQ_Time_Now() - overhead;
    if (ret2<0) {
      /* this can't actually happen but tricks the compiler into using ret instead of optimizing out the overhead loop entirely */
      dprintf(STDERR_FILENO,"ret2 went negative: %ld\n",ret2);
      exit(1);
    }
    dprintf(STDERR_FILENO,"%.3fns per pthread_rwlock_{wrlock,unlock}\n",((double)(end-start)) / (double)BENCHMARKCYCLES);

    exit(0);
  }

  int nspins;
  uint64_t futex_trips_possible = 0;
  void *threadret;
  pthread_t testthread, testthread2;
  pthread_attr_t attr;
  pthread_attr_t attr2;

  if ((ret=pthread_attr_init(&attr))!=0)
    dprintf(STDERR_FILENO,"pthread_attr_init: %s\n",strerror((int)ret));
  if ((ret=pthread_attr_init(&attr2))!=0)
    dprintf(STDERR_FILENO,"pthread_attr_init: %s\n",strerror((int)ret));

  if ((ret=pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED))!=0)
    dprintf(STDERR_FILENO,"pthread_attr_setdetachstate: %s\n",strerror((int)ret));
  if ((ret=pthread_attr_setdetachstate(&attr2, PTHREAD_CREATE_JOINABLE))!=0)
    dprintf(STDERR_FILENO,"pthread_attr_setdetachstate: %s\n",strerror((int)ret));

 again:

  if ((times == times_max) || exitflag) {
    dprintf(STDERR_FILENO,"\nCompleted %d run%s successfully; no defects detected\n",times,times==1?"":"s");
    goto out;
  }

  if (((MQ_SyncInt_Get(testlock.MutEx.Count) != 0) && (testlock.MutEx.EnBloc != MQ_MUTEX_CORE_INITIALIZER)) ||
      (((MQ_SyncInt_Get(testlock.RW.Count) & ~MQ_RWLOCK_COUNT_CONTENDED) != 0) && (testlock.RW.EnBloc != MQ_RWLOCK_CORE_INITIALIZER)) ||
      (MQ_TSA_ValidP(MQ_SyncInt_Get(testlock.MutEx.Owner)) && (testlock.MutEx.Owner != MQ_TSA_INVALIDOWNER)) ||
      (MQ_SyncInt_Get(testlock.Wait_List_Head) != 0)
      || ((MQ_RWLock_Cur_Waiter_Count_Unlocked(&testlock) != 0) && (testlock.MutEx.Owner != MQ_TSA_INVALIDOWNER))
#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
      || ((__MQ_RWLock_ThreadState.HeldLocks_Index != 0) && __MQ_ThreadState_Pointer)
#endif
      || (testlock.Shared_Futex_RefCount != 0)
      || (MQ_TSA_ValidP(testlock.RW.Owner) && (testlock.RW.Owner != MQ_TSA_INVALIDOWNER))) {
    dprintf(STDERR_FILENO,"\nat restart, __MQ_RWLock_ThreadState.HeldLocks_Index=%d, Shared_Futex_Refcount=%d, Cur_Waiter_Count=%d, R_Count=%u, W_Count=%u, Contended_bit=%d, MutEx=%u, MutEx_Locker=%d, RW.Owner=%d, LFList_Head=%p, Wait_List_Head=%p"
#if defined(MQ_RWLOCK_DEBUGGING_SUPPORT) || defined(MQ_RWLOCK_STATISTICS_SUPPORT)
", nonintrinsic_futex_trips=%lu/%lu, M=%lu/%lu/%lu/%lu, R=%lu/%lu/%lu, W=%lu/%lu/%lu, R2W=%lu/%lu/%lu, C=%lu/%lu, S=%lu/%lu/%lu\n\tinit=%s@%d/%s();lastWL=[%d]%s@%d/%s();lastWU=[%d]%s@%d/%s();lastFRL=[%d]%s@%d/%s();lastRL=[%d]%s@%d/%s();lastRU=[%d]%s@%d/%s()\n"
#else
"\n"
#endif
,
#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
	    __MQ_RWLock_ThreadState.HeldLocks_Index,
#else
	    -1,
#endif
	    testlock.Shared_Futex_RefCount,
	    MQ_RWLock_Cur_Waiter_Count_Unlocked(&testlock),MQ_RWLOCK_R_COUNT(&testlock),MQ_RWLOCK_W_COUNT(&testlock),(MQ_RWLOCK_RW_RAWCOUNT(&testlock)&MQ_RWLOCK_COUNT_CONTENDED) != 0, testlock.MutEx.Count, MQ_TSA_To_TID(testlock.MutEx.Owner), MQ_TSA_To_TID(testlock.RW.Owner),
	    testlock.LFList_Head,testlock.Wait_List_Head

#if defined(MQ_RWLOCK_DEBUGGING_SUPPORT) || defined(MQ_RWLOCK_STATISTICS_SUPPORT)
	    ,testlock.futex_trips, 
	    futex_trips_possible,
	    testlock.M_Reqs,testlock.M_Adaptives,testlock.M_Waits,testlock.M_Fails,
	    testlock.R_Reqs,testlock.R_Waits,testlock.R_Fails,
	    testlock.W_Reqs,testlock.W_Waits,testlock.W_Fails,
	    testlock.R2W_Reqs,testlock.R2W_Waits,testlock.R2W_Fails,
	    testlock.C_Reqs,testlock.C_Fails,
	    testlock.S_Reqs,testlock.S_Misses,testlock.S_Fails,
	    testlock.init_file,testlock.init_line,testlock.init_function,
	    testlock.last_W_Locker_tid,testlock.last_W_Locker_file,testlock.last_W_Locker_line,testlock.last_W_Locker_function,
	    testlock.last_W_Unlocker_tid,testlock.last_W_Unlocker_file,testlock.last_W_Unlocker_line,testlock.last_W_Unlocker_function,
	    testlock.last_fresh_R_Locker_tid,testlock.last_fresh_R_Locker_file,testlock.last_fresh_R_Locker_line,testlock.last_fresh_R_Locker_function,
	    testlock.last_R_Locker_tid,testlock.last_R_Locker_file,testlock.last_R_Locker_line,testlock.last_R_Locker_function,
	    testlock.last_R_Unlocker_tid,testlock.last_R_Unlocker_file,testlock.last_R_Unlocker_line,testlock.last_R_Unlocker_function
#endif	    
	    );
    exit(1);
  }

#ifdef MQ_DEBUG_MORE
  dprintf(STDERR_FILENO,"\nat restart, R_Count=%u, W_Count=%u, MutEx=%u, LFList_Head=%p, Wait_List_Head=%p, n_waiters=%d, nonintrinsic_futex_trips=%lu/%lu, M=%lu/%lu/%lu/%lu, R=%lu/%lu/%lu, W=%lu/%lu/%lu, R2W=%lu/%lu/%lu, C=%lu/%lu, S=%lu/%lu/%lu\n\tinit=%s@%d/%s();lastWL=[%d]%s@%d/%s();lastWU=[%d]%s@%d/%s();lastFRL=[%d]%s@%d/%s();lastRL=[%d]%s@%d/%s();lastRU=[%d]%s@%d/%s()\n",MQ_RWLOCK_R_COUNT(&testlock),MQ_RWLOCK_W_COUNT(&testlock),testlock.MutEx,
	    testlock.LFList_Head,/*testlock.Wait_LL_futex,*/testlock.Wait_List_Head,
	    MQ_RWLock_Cur_Waiter_Count_Unlocked(&testlock), testlock.futex_trips,
	    futex_trips_possible,
	    testlock.M_Reqs,testlock.M_Adaptives,testlock.M_Waits,testlock.M_Fails,
	    testlock.R_Reqs,testlock.R_Waits,testlock.R_Fails,
	    testlock.W_Reqs,testlock.W_Waits,testlock.W_Fails,
	    testlock.R2W_Reqs,testlock.R2W_Waits,testlock.R2W_Fails,
	    testlock.C_Reqs,testlock.C_Fails,
	    testlock.S_Reqs,testlock.S_Misses,testlock.S_Fails,
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
	    testlock.init_file,testlock.init_line,testlock.init_function,
	    testlock.last_W_Locker_tid,testlock.last_W_Locker_file,testlock.last_W_Locker_line,testlock.last_W_Locker_function,
	    testlock.last_W_Unlocker_tid,testlock.last_W_Unlocker_file,testlock.last_W_Unlocker_line,testlock.last_W_Unlocker_function,
	    testlock.last_fresh_R_Locker_tid,testlock.last_fresh_R_Locker_file,testlock.last_fresh_R_Locker_line,testlock.last_fresh_R_Locker_function,
	    testlock.last_R_Locker_tid,testlock.last_R_Locker_file,testlock.last_R_Locker_line,testlock.last_R_Locker_function,
	    testlock.last_R_Unlocker_tid,testlock.last_R_Unlocker_file,testlock.last_R_Unlocker_line,testlock.last_R_Unlocker_function
#else
	    "",0,"",
	    0,"",0,"",
	    0,"",0,"",
	    0,"",0,"",
	    0,"",0,"",
	    0,"",0,""
#endif	    
	    );
#else
  dprintf(STDERR_FILENO,".");
#endif

  if ((ret=MQ_Mutex_Lock(&testlock,MQ_ONE_SECOND))<0) {
    MQ_dprintf3("MQ_Mutex_Lock: %s\n",strerror((int)(-ret)));
    goto out;
  }
  ret=MQ_Mutex_Lock(&testlock,MQ_NOWAITING);
  if ((ret != -EDEADLK) && (ret != -EBUSY)) {
    MQ_dprintf3("MQ_Mutex_Lock should have EBUSYd or EDEADLKed but instead %s\n",strerror((int)(-ret)));
    goto out;
  }
  ret=MQ_Mutex_Lock(&testlock,1000L);
  if ((ret != -EDEADLK) && (ret != -ETIMEDOUT)) {
    MQ_dprintf3("MQ_Mutex_Lock should haved timed out or EDEADLKed but instead %s\n",strerror((int)(-ret)));
    goto out;
  } else if (ret == -ETIMEDOUT)
    Decrement_Futex_Trips(testlock,1); /* don't count this one -- nothing to do with performance */

  if ((ret=MQ_Mutex_Unlock(&testlock))<0) {
    MQ_dprintf3("MQ_Mutex_Unlock: %s\n",strerror((int)(-ret)));
    goto out;
  }

  MQ_SyncInt_Put(launchcount,0);
  MQ_SyncInt_Put(testtot,0);
  MQ_SyncInt_Put(testtot2,0);
  failtot = 0;

  for (i=0;i<100;++i)
    if ((ret=pthread_create(&testthread,&attr,testroutine_mutex,&testlock))!=0) {
      dprintf(STDERR_FILENO,"pthread_create: %s\n",strerror((int)ret));
      goto out;
    }

  futex_trips_possible += (uint64_t)i-1;

  for (nspins=0;launchcount<i;++nspins)
    usleep(1000);
#ifndef MQ_DEBUG_MORE
  if (nspins>100)
#endif
    dprintf(STDERR_FILENO,"%d spins, with %d timeouts, for %d mutex threads to complete\n",nspins,failtot,i);

  if (launchcount > i)
    goto out;

  if (MQ_SyncInt_Get(testtot) != MQ_SyncInt_Get(testtot2)) {
    dprintf(STDERR_FILENO,"testroutine_mutex loop: testtot=%d but testtot2=%d (launchcount=%d)\n",testtot,testtot2,launchcount);
    goto out;
  }

#ifdef MQ_DEBUG_MORE
  dprintf(STDERR_FILENO,"after mutex contention test, MutEx=%u, LFList_Head=%p, LFList_Tail=%p, LFList_Head_Waiter=%d, n_waiters=%d, nonintrinsic_futex_trips=%lu/%lu, M=%lu/%lu/%lu\n",MQ_SyncInt_Get(testlock.MutEx.Count),
	  testlock.LFList_Head,testlock.LFList_Tail,testlock.LFList_Head_Waiter,
	  MQ_RWLock_Cur_Waiter_Count_Unlocked(&testlock), testlock.futex_trips,
	  futex_trips_possible,
	  testlock.M_Reqs,testlock.M_Adaptives,testlock.M_Waits,testlock.M_Fails);
#endif


  if ((ret=MQ_W_Lock(&testlock, 1L << 30))<0) {
    MQ_dprintf3("MQ_W_Lock at %d: %s\n",__LINE__,strerror((int)(-ret)));
    goto out;
  }

  if ((i=MQ_Cond_Signal(&testlock, 1, (void *)0x987UL, MQ_COND_RETURN_UNLOCKED)) != 0) {
    if (i>1)
      dprintf(STDERR_FILENO,"MQ_Cond_Signal returned %d but should have returned zero\n",i);
    else
      MQ_dprintf3("MQ_Cond_Signal returned %d/%s\n",i,i<0 ? strerror(-i) : "");
    goto out;
  }

  if ((ret=MQ_W_Lock(&testlock, 1L << 30))<0) {
    MQ_dprintf3("MQ_W_Lock at %d: %s\n",__LINE__,strerror((int)(-ret)));
    goto out;
  }

  threadret = 0;
  if ((ret=MQ_Cond_Wait(&testlock, 1000L, &threadret, 0UL)) != -ETIMEDOUT) {
    MQ_dprintf3("MQ_Cond_Wait should have timed out, but instead %s\n",strerror((int)(-ret)));
    goto out;
  }
  if (threadret != 0) {
    dprintf(STDERR_FILENO,"MQ_Cond_Wait should not have returned an arg, but did: %p\n",threadret);
    goto out;
  }
  if (! MQ_W_HaveLock_p(&testlock)) {
    dprintf(STDERR_FILENO,"don't have write lock anymore after return from MQ_Cond_Wait\n");
    goto out;
  }

  if ((ret=MQ_R_Lock(&testlock, 1L << 24))<0) {
    if ((ret != -ETIMEDOUT) && (ret != -EDEADLK)) {
      MQ_dprintf3("MQ_R_Lock returned unexpected error, R=%u W=%u, that should have timed out: %s\n",MQ_RWLOCK_R_COUNT(&testlock),MQ_RWLOCK_W_COUNT(&testlock), strerror((int)(-ret)));
      goto out;
    }      
#ifdef MQ_DEBUG_MORE
    dprintf(STDERR_FILENO,"MQ_R_Lock (should time out): %s, file=%s, line=%d, function=%s\n",strerror(MQ_errno),MQ_errno_file,MQ_errno_line,MQ_errno_function);
#endif
    Decrement_Futex_Trips(testlock,1); /* don't count this one -- nothing to do with performance */
  } else {
    dprintf(STDERR_FILENO,"MQ_R_Lock succeeded, R=%u W=%u, that should have timed out\n",MQ_RWLOCK_R_COUNT(&testlock),MQ_RWLOCK_W_COUNT(&testlock));
    goto out;
  }

  if ((ret=MQ_W2R_Lock(&testlock))<0) {
    MQ_dprintf3("MQ_W2R_Lock: %s\n",strerror((int)(-ret)));
    goto out;
  }

#if 0
  if ((ret=MQ_R_Lock(&testlock,1L<<30))<0) {
    MQ_dprintf3("MQ_R_Lock before testroutine_cond 1: %s\n",strerror((int)(-ret)));
    goto out;
  }
#endif


  if ((ret=MQ_R_BequeathLock(&testlock))<0) {
    MQ_dprintf3("MQ_R_BequeathLock before testroutine_cond 1: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=pthread_create(&testthread,&attr2,testroutine_cond,&testlock))!=0) {
    dprintf(STDERR_FILENO,"pthread_create: %s\n",strerror((int)ret));
    goto out;
  }
  /* don't count the inevitable futex() in MQ_Cond_Wait */
  Decrement_Futex_Trips(testlock,1);

  usleep(1000);

  /* and if the thread hasn't entered COND_WAIT yet, don't count the futex in Cond_Signal either */
  if (MQ_RWLOCK_R_COUNT(&testlock)>0)
    Decrement_Futex_Trips(testlock,1);

  if ((ret=MQ_W_Lock(&testlock, 1L << 30))<0) {
    MQ_dprintf3("MQ_W_Lock at %d: %s\n",__LINE__,strerror((int)(-ret)));
    goto out;
  }

  if ((i=MQ_Cond_Signal(&testlock, 1, (void *)0x123UL, MQ_COND_RETURN_UNLOCKED)) != 1) {
    MQ_dprintf3("MQ_Cond_Signal returned %d/%s\n",i,i<0 ? strerror(-i) : "");
    goto out;
  }
  ++futex_trips_possible; /* for the LFLL contention between the cond and signal */

  if ((ret=pthread_join(testthread,&threadret))!=0) {
    dprintf(STDERR_FILENO,"pthread_join: %s\n",strerror((int)ret));
    goto out;
  }

  if (threadret != (void *)0x123UL) {
    dprintf(STDERR_FILENO,"testroutine_cond returned %p, but should be %p\n",threadret,(void *)0x123UL);
    goto out;
  }

  if ((ret=MQ_R_Lock(&testlock,1L<<30))<0) {
    MQ_dprintf3("MQ_R_Lock before testroutine_cond 2: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=MQ_R_BequeathLock(&testlock))<0) {
    MQ_dprintf3("MQ_R_BequeathLock before testroutine_cond 2: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=pthread_create(&testthread,&attr2,testroutine_cond,&testlock))!=0) {
    MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
    goto out;
  }
  /* don't count the inevitable futex() in MQ_Cond_Wait */
  Decrement_Futex_Trips(testlock,1);

  /* and if the thread hasn't entered COND_WAIT yet, don't count the futex in Cond_Signal either */
  if (MQ_RWLOCK_R_COUNT(&testlock)>0)
    Decrement_Futex_Trips(testlock,1);

  if ((ret=MQ_W_Lock(&testlock, 1L << 30))<0) {
    MQ_dprintf3("MQ_W_Lock at %d: %s\n",__LINE__,strerror((int)(-ret)));
    goto out;
  }

  if ((i=MQ_Cond_Signal(&testlock, 1, (void *)0x456UL, MQ_COND_RETURN_UNLOCKED)) != 1) {
    MQ_dprintf3("MQ_Cond_Signal returned %d/%s\n\n",i,i<0 ? strerror(-i) : "");
    goto out;
  }
  ++futex_trips_possible; /* for the LFLL contention between the cond and signal */

  if ((ret=pthread_join(testthread,&threadret))!=0) {
    MQ_dprintf3("pthread_join: %s\n",strerror((int)ret));
    goto out;
  }

  if (threadret != (void *)0x456UL) {
    dprintf(STDERR_FILENO,"testroutine_cond returned %p, but should be %p\n",threadret,(void *)0x456UL);
    goto out;
  }


  if ((ret=MQ_R_Lock(&testlock,1L<<30))<0) {
    MQ_dprintf3("MQ_R_Lock before testroutine_cond 3: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=MQ_R_BequeathLock(&testlock))<0) {
    MQ_dprintf3("MQ_R_BequeathLock before testroutine_cond 3: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=pthread_create(&testthread,&attr2,testroutine_cond,&testlock))!=0) {
    MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
    goto out;
  }
  /* don't count the inevitable futex() in MQ_Cond_Wait */
  Decrement_Futex_Trips(testlock,1);

  if ((ret=MQ_R_Lock(&testlock,1L<<30))<0) {
    MQ_dprintf3("MQ_R_Lock before testroutine_cond 4: %s\n",strerror((int)(-ret)));
    goto out;
  }
  ++futex_trips_possible; /* for the LFLL contention between the cond and the lock */

  if ((ret=MQ_R_BequeathLock(&testlock))<0) {
    MQ_dprintf3("MQ_R_BequeathLock before testroutine_cond 4: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=pthread_create(&testthread2,&attr2,testroutine_cond,&testlock))!=0) {
    MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
    goto out;
  }
  /* don't count the inevitable futex() in MQ_Cond_Wait */
  Decrement_Futex_Trips(testlock,1);

  /* and if the thread hasn't entered COND_WAIT yet, don't count the futex in Cond_Signal either */
  if (MQ_RWLOCK_R_COUNT(&testlock)>0)
    Decrement_Futex_Trips(testlock,1);

  if ((ret=MQ_W_Lock(&testlock, 1L << 30))<0) {
    MQ_dprintf3("MQ_W_Lock at %d: %s\n",__LINE__,strerror((int)(-ret)));
    goto out;
  }

  if ((i=MQ_Cond_Signal(&testlock, 2, (void *)0x789UL, MQ_COND_RETURN_UNLOCKED)) != 2) {
    MQ_dprintf3("MQ_Cond_Signal returned %d/%s\n\n",i,i<0 ? strerror(-i) : "");
    goto out;
  }
  ++futex_trips_possible; /* for the LFLL contention between the second cond and the signal */

  if ((ret=pthread_join(testthread,&threadret))!=0) {
    MQ_dprintf3("pthread_join: %s\n",strerror((int)ret));
    goto out;
  }

  if (threadret != (void *)0x789UL) {
    dprintf(STDERR_FILENO,"testroutine_cond returned %p, but should be %p\n",threadret,(void *)0x789UL);
    goto out;
  }

  if ((ret=pthread_join(testthread2,&threadret))!=0) {
    MQ_dprintf3("pthread_join: %s\n",strerror((int)ret));
    goto out;
  }

  if (threadret != (void *)0x789UL) {
    dprintf(STDERR_FILENO,"testroutine_cond returned %p, but should be %p\n",threadret,(void *)0x789UL);
    goto out;
  }


  /* same as above, but without MQ_COND_RETURN_UNLOCKED on the signal */

  if ((ret=MQ_R_Lock(&testlock,1L<<30))<0) {
    MQ_dprintf3("MQ_R_Lock before testroutine_cond 4: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=MQ_R_BequeathLock(&testlock))<0) {
    MQ_dprintf3("MQ_R_BequeathLock before testroutine_cond 4: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=pthread_create(&testthread,&attr2,testroutine_cond,&testlock))!=0) {
    MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
    goto out;
  }
  /* don't count the inevitable futex() in MQ_Cond_Wait */
  Decrement_Futex_Trips(testlock,1);

  if ((ret=MQ_R_Lock(&testlock,1L<<30))<0) {
    MQ_dprintf3("MQ_R_Lock before testroutine_cond 4: %s\n",strerror((int)(-ret)));
    goto out;
  }
  ++futex_trips_possible; /* for the LFLL contention between the cond and the lock */

  if ((ret=MQ_R_BequeathLock(&testlock))<0) {
    MQ_dprintf3("MQ_R_BequeathLock before testroutine_cond 4: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=pthread_create(&testthread2,&attr2,testroutine_cond,&testlock))!=0) {
    MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
    goto out;
  }
  /* don't count the inevitable futex() in MQ_Cond_Wait */
  Decrement_Futex_Trips(testlock,1);

  /* and if the thread hasn't entered COND_WAIT yet, don't count the futex in Cond_Signal either */
  if (MQ_RWLOCK_R_COUNT(&testlock)>0)
    Decrement_Futex_Trips(testlock,1);

  if ((ret=MQ_W_Lock(&testlock, 1L << 30))<0) {
    MQ_dprintf3("MQ_W_Lock at %d: %s\n",__LINE__,strerror((int)(-ret)));
    goto out;
  }

  if ((i=MQ_Cond_Signal(&testlock, 2, (void *)0x654UL, MQ_NOFLAGS /* MQ_COND_RETURN_UNLOCKED */ )) != 2) {
    MQ_dprintf3("MQ_Cond_Signal returned %d/%s\n\n",i,i<0 ? strerror(-i) : "");
    goto out;
  }
  ++futex_trips_possible; /* for the LFLL contention between the second cond and the signal */

  if ((ret=MQ_W_Unlock(&testlock))<0) {
    MQ_dprintf3("MQ_W_Unlock at %d: %s\n",__LINE__,strerror((int)(-ret)));
    goto out;
  }

  if ((ret=pthread_join(testthread,&threadret))!=0) {
    MQ_dprintf3("pthread_join: %s\n",strerror((int)ret));
    goto out;
  }

  if (threadret != (void *)0x654UL) {
    dprintf(STDERR_FILENO,"testroutine_cond returned %p, but should be %p\n",threadret,(void *)0x654UL);
    goto out;
  }

  if ((ret=pthread_join(testthread2,&threadret))!=0) {
    MQ_dprintf3("pthread_join: %s\n",strerror((int)ret));
    goto out;
  }

  if (threadret != (void *)0x654UL) {
    dprintf(STDERR_FILENO,"testroutine_cond returned %p, but should be %p\n",threadret,(void *)0x654UL);
    goto out;
  }


  /* same as above, but W locks for the cond_waits and R locks for the signals */

  if ((ret=MQ_W_Lock(&testlock,1L<<30))<0) {
    MQ_dprintf3("MQ_R_Lock before testroutine_cond 5: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=MQ_W_BequeathLock(&testlock))<0) {
    MQ_dprintf3("MQ_W_BequeathLock before testroutine_cond 5: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=pthread_create(&testthread,&attr2,testroutine_cond_w,&testlock))!=0) {
    MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
    goto out;
  }
  /* don't count the inevitable futex() in MQ_Cond_Wait */
  Decrement_Futex_Trips(testlock,1);

  if ((ret=MQ_W_Lock(&testlock,1L<<30))<0) {
    MQ_dprintf3("MQ_W_Lock before testroutine_cond 5: %s\n",strerror((int)(-ret)));
    goto out;
  }
  ++futex_trips_possible; /* for the LFLL contention between the cond and the lock */

  if ((ret=MQ_W_BequeathLock(&testlock))<0) {
    MQ_dprintf3("MQ_W_BequeathLock before testroutine_cond 4: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=pthread_create(&testthread2,&attr2,testroutine_cond_w,&testlock))!=0) {
    MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
    goto out;
  }
  /* don't count the inevitable futex() in MQ_Cond_Wait */
  Decrement_Futex_Trips(testlock,1);

  if ((ret=MQ_R_Lock(&testlock, 1L << 30))<0) {
    MQ_dprintf3("MQ_W_Lock at %d: %s\n",__LINE__,strerror((int)(-ret)));
    goto out;
  }

  if ((i=MQ_Cond_Signal(&testlock, 2, (void *)0x321UL, MQ_NOFLAGS /* MQ_COND_RETURN_UNLOCKED */ )) != 1) {
    MQ_dprintf3("MQ_Cond_Signal returned %d/%s\n\n",i,i<0 ? strerror(-i) : "");
    goto out;
  }

#ifdef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
  if ((i=MQ_Cond_Signal(&testlock, 2, (void *)0x210UL, MQ_NOFLAGS)) != 1) {
    MQ_dprintf3("MQ_Cond_Signal returned %d/%s\n\n",i,i<0 ? strerror(-i) : "");
    goto out;
  }
  if ((ret=MQ_R_Unlock(&testlock))<0) {
    MQ_dprintf3("MQ_W_Lock at %d: %s\n",__LINE__,strerror((int)(-ret)));
    goto out;
  }
#else
  if ((i=MQ_Cond_Signal(&testlock, 2, (void *)0x210UL, MQ_COND_RETURN_UNLOCKED)) != 1) {
    MQ_dprintf3("MQ_Cond_Signal returned %d/%s\n\n",i,i<0 ? strerror(-i) : "");
    goto out;
  }
#endif

  if ((ret=pthread_join(testthread,&threadret))!=0) {
    MQ_dprintf3("pthread_join: %s\n",strerror((int)ret));
    goto out;
  }

  if (threadret != (void *)0x321UL) {
    dprintf(STDERR_FILENO,"testroutine_cond returned %p, but should be %p\n",threadret,(void *)0x321UL);
    goto out;
  }

  if ((ret=pthread_join(testthread2,&threadret))!=0) {
    MQ_dprintf3("pthread_join: %s\n",strerror((int)ret));
    goto out;
  }

  if (threadret != (void *)0x210UL) {
    dprintf(STDERR_FILENO,"testroutine_cond returned %p, but should be %p\n",threadret,(void *)0x210UL);
    goto out;
  }


  /* same as above, but mutex locks for the cond_waits and signals */

  if ((ret=MQ_Mutex_Lock(&testlock,1L<<30))<0) {
    MQ_dprintf3("MQ_R_Lock before testroutine_cond 5: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=MQ_Mutex_BequeathLock(&testlock))<0) {
    MQ_dprintf3("MQ_Mutex_BequeathLock before testroutine_cond_mutex: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=pthread_create(&testthread,&attr2,testroutine_cond_mutex,&testlock))!=0) {
    MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
    goto out;
  }
  /* don't count the inevitable futex() in MQ_Cond_Wait */
  Decrement_Futex_Trips(testlock,1);

  if ((ret=MQ_Mutex_Lock(&testlock,1L<<30))<0) {
    MQ_dprintf3("MQ_Mutex_Lock before testroutine_cond_mutex: %s\n",strerror((int)(-ret)));
    goto out;
  }
  ++futex_trips_possible; /* for the LFLL contention between the cond and the lock */

  if ((ret=MQ_Mutex_BequeathLock(&testlock))<0) {
    MQ_dprintf3("MQ_Mutex_BequeathLock before testroutine_cond_mutex: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=pthread_create(&testthread2,&attr2,testroutine_cond_mutex,&testlock))!=0) {
    MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
    goto out;
  }
  /* don't count the inevitable futex() in MQ_Cond_Wait */
  Decrement_Futex_Trips(testlock,1);

  if ((ret=MQ_Mutex_Lock(&testlock, 1L << 30))<0) {
    MQ_dprintf3("MQ_Mutex_Lock at %d: %s\n",__LINE__,strerror((int)(-ret)));
    goto out;
  }

  if ((i=MQ_Cond_Signal(&testlock, 2, (void *)0x234UL, MQ_NOFLAGS /* MQ_COND_RETURN_UNLOCKED */ )) != 1) {
    MQ_dprintf3("MQ_Cond_Signal returned %d/%s\n\n",i,i<0 ? strerror(-i) : "");
    goto out;
  }

  if ((i=MQ_Cond_Signal(&testlock, 2, (void *)0x345UL, MQ_NOFLAGS /* MQ_COND_RETURN_UNLOCKED */)) != 1) {
    MQ_dprintf3("MQ_Cond_Signal returned %d/%s\n\n",i,i<0 ? strerror(-i) : "");
    goto out;
  }

  if ((ret=MQ_Mutex_Unlock(&testlock))<0) {
    MQ_dprintf3("MQ_Mutex_Unlock at %d: %s\n",__LINE__,strerror((int)(-ret)));
    goto out;
  }

  if ((ret=pthread_join(testthread,&threadret))!=0) {
    MQ_dprintf3("pthread_join: %s\n",strerror((int)ret));
    goto out;
  }

  if (threadret != (void *)0x234UL) {
    dprintf(STDERR_FILENO,"testroutine_cond returned %p, but should be %p\n",threadret,(void *)0x234UL);
    goto out;
  }

  if ((ret=pthread_join(testthread2,&threadret))!=0) {
    MQ_dprintf3("pthread_join: %s\n",strerror((int)ret));
    goto out;
  }

  if (threadret != (void *)0x345UL) {
    dprintf(STDERR_FILENO,"testroutine_cond returned %p, but should be %p\n",threadret,(void *)0x345UL);
    goto out;
  }



  /* same as above, but mutex locks with MQ_COND_RETURN_UNLOCKED for the cond_waits and signals */

  if ((ret=MQ_Mutex_Lock(&testlock,1L<<30))<0) {
    MQ_dprintf3("MQ_R_Lock before testroutine_cond 5: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=MQ_Mutex_BequeathLock(&testlock))<0) {
    MQ_dprintf3("MQ_Mutex_BequeathLock before testroutine_cond_mutex: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=pthread_create(&testthread,&attr2,testroutine_cond_mutex_returnunlocked,&testlock))!=0) {
    MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
    goto out;
  }
  /* don't count the inevitable futex() in MQ_Cond_Wait */
  Decrement_Futex_Trips(testlock,1);

  if ((ret=MQ_Mutex_Lock(&testlock,1L<<30))<0) {
    MQ_dprintf3("MQ_Mutex_Lock before testroutine_cond_mutex: %s\n",strerror((int)(-ret)));
    goto out;
  }
  ++futex_trips_possible; /* for the LFLL contention between the cond and the lock */

  if ((ret=MQ_Mutex_BequeathLock(&testlock))<0) {
    MQ_dprintf3("MQ_Mutex_BequeathLock before testroutine_cond_mutex: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=pthread_create(&testthread2,&attr2,testroutine_cond_mutex_returnunlocked,&testlock))!=0) {
    MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
    goto out;
  }
  /* don't count the inevitable futex() in MQ_Cond_Wait */
  Decrement_Futex_Trips(testlock,1);

  if ((ret=MQ_Mutex_Lock(&testlock, 1L << 30))<0) {
    MQ_dprintf3("MQ_Mutex_Lock at %d: %s\n",__LINE__,strerror((int)(-ret)));
    goto out;
  }

  if ((i=MQ_Cond_Signal(&testlock, 2, (void *)0x567UL, MQ_NOFLAGS)) != 1) {
    MQ_dprintf3("MQ_Cond_Signal returned %d/%s\n\n",i,i<0 ? strerror(-i) : "");
    goto out;
  }

  if ((i=MQ_Cond_Signal(&testlock, 2, (void *)0x678UL, MQ_COND_RETURN_UNLOCKED)) != 1) {
    MQ_dprintf3("MQ_Cond_Signal returned %d/%s\n\n",i,i<0 ? strerror(-i) : "");
    goto out;
  }

  if ((ret=pthread_join(testthread,&threadret))!=0) {
    MQ_dprintf3("pthread_join: %s\n",strerror((int)ret));
    goto out;
  }

  if (threadret != (void *)0x567UL) {
    dprintf(STDERR_FILENO,"testroutine_cond returned %p, but should be %p\n",threadret,(void *)0x234UL);
    goto out;
  }

  if ((ret=pthread_join(testthread2,&threadret))!=0) {
    MQ_dprintf3("pthread_join: %s\n",strerror((int)ret));
    goto out;
  }

  if (threadret != (void *)0x678UL) {
    dprintf(STDERR_FILENO,"testroutine_cond returned %p, but should be %p\n",threadret,(void *)0x345UL);
    goto out;
  }



  if ((ret=MQ_W_Lock(&testlock,1L<<30))<0) {
    MQ_dprintf3("MQ_W_Lock before testroutine_cond2 1: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=MQ_W_BequeathLock(&testlock))<0) {
    MQ_dprintf3("MQ_W_BequeathLock before testroutine_cond2 1: %s\n",strerror((int)(-ret)));
    goto out;
  }

#if 1
  if ((ret=pthread_create(&testthread2,&attr2,testroutine_cond3,&testlock))!=0) {
    MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
    goto out;
  }
  /* don't count the inevitable futex() in MQ_Cond_Wait */
  /* there's no practical way the W lock in testroutine_cond3 is succeeding without waiting, so don't count that either: */
  Decrement_Futex_Trips(testlock,2);
#endif

  if ((ret=pthread_create(&testthread,&attr2,testroutine_cond2,&testlock))!=0) {
    MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
    goto out;
  }
  /* don't count the inevitable futex() in MQ_Cond_Wait */
  Decrement_Futex_Trips(testlock,1);
  ++futex_trips_possible; /* for the LFLL contention between the two conds */

/* usleep(500000); */

  if (MQ_Time_Now() & 1) {
#ifdef MQ_DEBUG_MORE
    dprintf(STDERR_FILENO,"sleeping 10ms\n");
#endif
    usleep(10000);
  }

  /* and if the thread hasn't entered COND_WAIT yet, don't count the futex in Cond_Signal either */
  if (MQ_RWLOCK_W_COUNT(&testlock)>0)
    Decrement_Futex_Trips(testlock,1);

  nspins = 0;
  /* unlocked signalling */
  while ((ret=MQ_Cond_Signal(&testlock, 2, (void *)0xabcUL, MQ_NOFLAGS )) != 1) {
    if (ret<0) {
      MQ_dprintf3("MQ_Cond_Signal: %s\n",strerror((int)(-ret)));
      goto out;
    }
    ++nspins;
    sched_yield();
  }
#ifdef MQ_DEBUG_MORE
  dprintf(STDERR_FILENO,"\nMQ_Cond_Signal 0xabcUL, %d spins\n",nspins);
#endif
  ++futex_trips_possible; /* for the LFLL contention between the conds and the signal */

  if ((ret=pthread_join(testthread,&threadret))!=0) {
    MQ_dprintf3("pthread_join: %s\n",strerror((int)ret));
    goto out;
  }

  if (threadret != (void *)0xabcUL) {
    dprintf(STDERR_FILENO,"testroutine_cond2 returned %p\n",threadret);
    goto out;
  }

  /* and if the thread hasn't entered COND_WAIT yet, don't count the futex in Cond_Signal either */
  if (MQ_RWLOCK_W_COUNT(&testlock)>0)
    Decrement_Futex_Trips(testlock,1);
#if 1
  for (;;) {
    usleep(100); /* always have to wait, because there's a race between testroutine_cond3 and MQ_Cond_Signal(...,0xdefUL) */
    /* unlocked signalling */
    if ((i=MQ_Cond_Signal(&testlock, 2, (void *)0xdefUL, MQ_NOFLAGS)) == 1)
      break;
    else if (i!=0) {
      MQ_dprintf3("MQ_Cond_Signal returned %d/%s\n\n",i,strerror(-i));
      goto out;
    }
  }
  ++futex_trips_possible; /* for the LFLL contention between the cond and the second signal */

  if ((ret=pthread_join(testthread2,&threadret))!=0) {
    MQ_dprintf3("pthread_join: %s\n",strerror((int)ret));
    goto out;
  }

  if (threadret != (void *)0xdefUL) {
    dprintf(STDERR_FILENO,"testroutine_cond3 returned %p\n",threadret);
    goto out;
  }
#endif


  /* test cancellation of thread waiting on condition: */
  if ((ret=MQ_R_Lock(&testlock,1L<<30))<0) {
    MQ_dprintf3("MQ_R_Lock before testroutine_cond 5: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=MQ_R_BequeathLock(&testlock))<0) {
    MQ_dprintf3("MQ_R_BequeathLock before testroutine_cond 5: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=pthread_create(&testthread,&attr2,testroutine_cond,&testlock))!=0) {
    MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
    goto out;
  }
  /* don't count the inevitable futex() in MQ_Cond_Wait */
  Decrement_Futex_Trips(testlock,1);

  if (MQ_Time_Now() & 1) {
#ifdef MQ_DEBUG_MORE
    dprintf(STDERR_FILENO,"sleeping 10ms\n");
#endif
    usleep(10000);
  }

  if ((ret=pthread_cancel(testthread))!=0) {
    MQ_dprintf3("pthread_cancel: %s\n",strerror((int)ret));
    goto out;
  }

  if ((ret=pthread_join(testthread,0))<0) {
    MQ_dprintf3("pthread_join: %s\n",strerror((int)ret));
    goto out;
  }

#if 0
  if ((ret=MQ_R_Unlock(&testlock))<0) {
    MQ_dprintf3("MQ_R_Unlock after cancelled testroutine_cond: %s\n",strerror((int)(-ret)));
    goto out;
  }
#endif


  /* test cancellation with W semantics */
  if ((ret=MQ_W_Lock(&testlock,1L<<30))<0) {
    MQ_dprintf3("MQ_W_Lock before testroutine_cond2 2: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=MQ_W_BequeathLock(&testlock))<0) {
    MQ_dprintf3("MQ_W_BequeathLock before testroutine_cond2 2: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=pthread_create(&testthread,&attr2,testroutine_cond2,&testlock))!=0) {
    MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
    goto out;
  }
  /* don't count the inevitable futex() in MQ_Cond_Wait */
  Decrement_Futex_Trips(testlock,1);

  if (MQ_Time_Now() & 1) {
#ifdef MQ_DEBUG_MORE
    dprintf(STDERR_FILENO,"sleeping 10ms\n");
#endif
    usleep(10000);
  }

  if ((ret=pthread_cancel(testthread))!=0) {
    MQ_dprintf3("pthread_cancel: %s\n",strerror((int)ret));
    goto out;
  }

  if ((ret=pthread_join(testthread,0))!=0) {
    MQ_dprintf3("pthread_join: %s\n",strerror((int)ret));
    goto out;
  }

  /* thread A gets an R lock, thread B sends a
   * signal, thread C gets a W lock, A cond_waits, should return
   * immediately with the signal from B, A releases R lock, C should
   * get W lock immediately.
   */
  if ((ret=MQ_R_Lock(&testlock,MQ_ONE_SECOND*3))<0) {
    MQ_dprintf3("MQ_R_Lock for testroutine_signal: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=pthread_create(&testthread,&attr2,testroutine_signal_w,&testlock))!=0) {
    MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
    goto out;
  }
  /* don't count the inevitable futex() in testroutine_signal */
  Decrement_Futex_Trips(testlock,1);

  /* set up a deliberate race between testroutine_signal_w and testroutine_w */
  /* usleep(1000); */

  if ((ret=pthread_create(&testthread2,&attr2,testroutine_w,&testlock))!=0) {
    MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
    goto out;
  }
  /* don't count the inevitable futex() in testroutine_w */
  Decrement_Futex_Trips(testlock,1);

  if ((ret=MQ_Cond_Wait(&testlock, MQ_FOREVER, &threadret, 0UL)) < 0) {
    MQ_dprintf3("main MQ_Cond_Wait 1: %s\n",strerror((int)(-ret)));
    goto out;
  }
  /* don't count the inevitable futex() in MQ_Cond_Wait */
  Decrement_Futex_Trips(testlock,1);
  ++futex_trips_possible; /* for the LFLL contention between the lock and the wait */

  if (threadret != (void *)0xdead6005eUL) {
    dprintf(STDERR_FILENO,"testroutine_signal cond_wait got %p\n",threadret);
    goto out;
  }
#ifdef MQ_DEBUG_MORE
  else
    dprintf(STDERR_FILENO,"testroutine_signal cond_wait done, threadret = %p\n",threadret);
#endif

  if ((ret=pthread_join(testthread,0))!=0) {
    MQ_dprintf3("pthread_join: %s\n",strerror((int)ret));
    goto out;
  }

  if ((ret=MQ_R_Unlock(&testlock))<0) {
    MQ_dprintf3("MQ_R_Unlock for testroutine_signal: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=pthread_join(testthread2,0))!=0) {
    MQ_dprintf3("pthread_join: %s\n",strerror((int)ret));
    goto out;
  }

#ifdef MQ_DEBUG_MORE
  dprintf(STDERR_FILENO,"\nafter MQ_Cond_Signal, R_Count=%u, W_Count=%u, MutEx=%u, LFList_Head=%p, Wait_List_Head=%p, n_waiters=%d\n",MQ_RWLOCK_R_COUNT(&testlock),MQ_RWLOCK_W_COUNT(&testlock),testlock.MutEx,
	    testlock.LFList_Head,/*testlock.Wait_LL_futex,*/testlock.Wait_List_Head,
	  MQ_RWLock_Cur_Waiter_Count_Unlocked(&testlock)
	  );
#endif

  MQ_SyncInt_Put(testtot,0);

  MQ_SyncInt_Put(launchcount,0);

  for (i=0;i<100;++i)
    if ((ret=pthread_create(&testthread,&attr,testroutine_w,&testlock))!=0) {
      MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
      goto out;
    }

  futex_trips_possible += 3*((uint64_t)i-1);

  nspins = 0;
  while (launchcount < i) {
    usleep(1000);
    ++nspins;
  }

#ifdef MQ_DEBUG_MORE
  dprintf(STDERR_FILENO,"\nafter %d 1ms spins, about to MQ_R_Lock with testtot=%d, R_Count=%u, W_Count=%u, MutEx=%u, LFList_Head=%p, Wait_List_Head=%p, n_waiters=%d\n",nspins,testtot,MQ_RWLOCK_R_COUNT(&testlock),MQ_RWLOCK_W_COUNT(&testlock),testlock.MutEx,
	  testlock.LFList_Head,testlock.Wait_List_Head,
	  MQ_RWLock_Cur_Waiter_Count_Unlocked(&testlock)
	  );
#endif

  /* there's still a race -- if testtot is under 100, loop until it isn't: */
  nspins = 0;
  while (testtot < i) {
    usleep(1000);
    ++nspins;
    if (nspins > 1000) {
      dprintf(STDERR_FILENO,"testtot never made it to %d\n",i);
      goto out;
    }
  }


  if (nspins>
#ifdef MQ_DEBUG_MORE
0
#else
10
#endif
)
    dprintf(STDERR_FILENO,"took %d more 1ms spins for testroutine_w testtot to reach %d\n",nspins,i);

  if ((ret=MQ_R_Lock(&testlock, 2L << 30))<0) {
    MQ_dprintf3("\nMQ_R_Lock failed, testtot=%d, %s\n",testtot,strerror((int)(-ret)));
    goto out;
  }
  futex_trips_possible += 2;

  if (testtot != i) {
    dprintf(STDERR_FILENO,"\ntesttot after 1ms and obtaining read lock: %d (should be %d), Wait_List_Head=%p, n_waiters=%d\n",testtot,i,testlock.Wait_List_Head,
	    MQ_RWLock_Cur_Waiter_Count_Unlocked(&testlock));
    goto out;
  }

  if ((ret=MQ_R_Unlock(&testlock))<0) {
    MQ_dprintf3("MQ_R_Unlock: %s\n",strerror((int)(-ret)));
    goto out;
  }

  MQ_SyncInt_Put(testtot,0);

  for (i=0;i<100;++i) {
    if ((ret=MQ_R_Lock(&testlock,1L<<30))<0) {
      MQ_dprintf3("MQ_R_Lock for testroutine_r 1: %s\n",strerror((int)(-ret)));
      goto out;
    }
    if ((ret=MQ_R_BequeathLock(&testlock))<0) {
      MQ_dprintf3("MQ_R_BequeathLock for testroutine_r: %s\n",strerror((int)(-ret)));
      goto out;
    }
    if ((ret=pthread_create(&testthread,&attr,testroutine_r,&testlock))!=0) {
      MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
      goto out;
    }
  }
  futex_trips_possible += (uint64_t)i-1; /* just the unlocks */

/*  usleep(1000); */
#ifdef MQ_DEBUG_MORE
  dprintf(STDERR_FILENO,"\nabout to MQ_W_Lock with testtot=%d, R_Count=%u, W_Count=%u, MutEx=%u, LFList_Head=%p, Wait_List_Head=%p, n_waiters=%d\n",testtot,MQ_RWLOCK_R_COUNT(&testlock),MQ_RWLOCK_W_COUNT(&testlock),testlock.MutEx,
	  testlock.LFList_Head,testlock.Wait_List_Head,
	  MQ_RWLock_Cur_Waiter_Count_Unlocked(&testlock)
	  );
#endif

  if ((ret=MQ_W_Lock(&testlock, 2L << 30))<0) {
    MQ_dprintf3("MQ_W_Lock failed, testtot=%d, %s\n",testtot,strerror((int)(-ret)));
    goto out;
  }
  /* there's no practical way that W lock is succeeding without waiting, so don't count that: */
  Decrement_Futex_Trips(testlock,1);

  if (testtot != i) {
    dprintf(STDERR_FILENO,"\ntesttot after obtaining write lock: %d (should be %d), Wait_List_Head=%p, n_waiters=%d\n",testtot,i,testlock.Wait_List_Head,
	    MQ_RWLock_Cur_Waiter_Count_Unlocked(&testlock)
	    );
    goto out;
  }

  if ((ret=MQ_W_Unlock(&testlock))<0) {
    MQ_dprintf3("MQ_W_Unlock: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if (testtot != i) {
    dprintf(STDERR_FILENO,"\ntesttot after getting and releasing W lock: %d (should be %d)\n",testtot,i);
    goto out;
  }

  if ((ret=MQ_R_Lock(&testlock, 2L << 30))<0) {
    MQ_dprintf3("\nMQ_R_Lock failed, testtot=%d, %s\n",testtot,strerror((int)(-ret)));
    goto out;
  }

  MQ_SyncInt_Put(testtot,0);
  MQ_SyncInt_Put(launchcount,0);
  for (i=0;i<50;++i) {
    if ((ret=pthread_create(&testthread,&attr,testroutine_r2,&testlock))!=0) {
      MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
      goto out;
    }
  }
  futex_trips_possible += 2*((uint64_t)i-1); /* unlock added below */

#if 0
  /* commented out because an apparent bug in the pthreads library
   * causes this to segv on rare occasions (race between
   * pthread_create and pthread_cancel):
   */
  /* cancellation should be ignored by lock routines: */
  if ((ret=pthread_cancel(testthread))!=0) {
    MQ_dprintf3("pthread_cancel 1: %s\n",strerror((int)ret));
    goto out;
  }
#endif


  if ((ret=MQ_R2W_Lock(&testlock, 2L << 30))<0) {
    MQ_dprintf3("\nMQ_R2W_Lock failed, testtot=%d, launchcount=%d, %s\n",testtot,launchcount,strerror((int)(-ret)));
    goto out;
  }
  futex_trips_possible += 2;

  nspins = 0;
  while (launchcount < i) {
    usleep(1000);
    ++nspins;
  }
  usleep(1000); /* one more sleep in case launchcount was incremented in testroutine_r2 before and adjacent to a context switch */

#if 0
  /* cancellation should be ignored by lock routines: */
  if ((ret=pthread_cancel(testthread))!=0) {
    MQ_dprintf3("pthread_cancel 2: %s\n",strerror((int)ret));
    goto out;
  }
#endif

#ifdef MQ_DEBUG_MORE
  dprintf(STDERR_FILENO," done\n");
#endif

  /* get ten threads waiting for R locks: */
  MQ_SyncInt_Put(launchcount,0);
  for (i=50;i<60;++i) {
    if ((ret=pthread_create(&testthread,&attr,testroutine_r2,&testlock))!=0) {
      MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
      goto out;
    }
  }
  /* don't count the inevitable futex()es in testroutine_r2 */
  Decrement_Futex_Trips(testlock,10);

  nspins = 0;
  while (launchcount < 10) {
    usleep(1000);
    ++nspins;
  }

  if ((ret=MQ_W_Unlock(&testlock))<0) {
    MQ_dprintf3("MQ_W_Unlock: %s\n",strerror((int)(-ret)));
    goto out;
  }

  nspins = 0;
  while (testtot < 60) {
    usleep(1000);
    ++nspins;
    if (nspins>100) {
      dprintf(STDERR_FILENO,"testroutine_r2 blocked on W: %d of 10 didn't complete after unblocking, after %d 1ms spins\n",60-testtot,nspins);
      goto out;
    }
  }

  if (nspins>5)
    dprintf(STDERR_FILENO,"10 testroutine_r2 blocked on W took %d 1ms spins to complete when unblocked\n",nspins);

  for (i=60;i<100;++i) {
    if ((ret=MQ_R_Lock(&testlock,1L<<30))<0) {
      MQ_dprintf3("MQ_R_Lock for testroutine_r 3: %s\n",strerror((int)(-ret)));
      goto out;
    }
    if ((ret=MQ_R_BequeathLock(&testlock))<0) {
      MQ_dprintf3("MQ_R_BequeathLock for testroutine_r 3: %s\n",strerror((int)(-ret)));
      goto out;
    }
    if ((ret=pthread_create(&testthread,&attr,testroutine_r,&testlock))!=0) {
      MQ_dprintf3("pthread_create: %s\n",strerror((int)ret));
      goto out;
    }
  }
  futex_trips_possible += ((uint64_t)i-1); /* includes unlocks in first half */

  nspins = 0;
  while ((testtot<i) || testlock.Wait_List_Head || ((testlock.RW.Count & ~MQ_RWLOCK_COUNT_CONTENDED) != 0)
	 || (MQ_RWLock_Cur_Waiter_Count_Unlocked(&testlock) > 0)
	 ) { 
    ++nspins;
    if (nspins > 1000) {
      MQ_dprintf("testtot never made it to %d -- testtot=%d, WLH=%p, RW.Count=%x, R_bit=%d, Contended_bit=%d, CWC=%d\n",i,testtot,testlock.Wait_List_Head,MQ_RWLOCK_RW_COUNT(&testlock),(testlock.RW.Count&MQ_RWLOCK_COUNT_READ) ? 1 : 0,(testlock.RW.Count&MQ_RWLOCK_COUNT_CONTENDED) ? 1 : 0, MQ_RWLock_Cur_Waiter_Count_Unlocked(&testlock));
      goto out;
    }
    if (nspins<10)
      sched_yield();
    else
      usleep(1000);
  }
	 /*
  if (testtot != i) {
    dprintf(STDERR_FILENO,"\n2 testtot after draining LL and {R,W}_Count (%d spins): %d (should be %d), n_waiters=%d\n",nspins,testtot,i,
	    MQ_RWLock_Cur_Waiter_Count_Unlocked(&testlock));
    goto out;
  }
	 */

  if ((ret=MQ_R_Lock(&testlock, 2L << 30))<0) {
    MQ_dprintf3("MQ_R_Lock: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if ((ret=MQ_R2W_Lock(&testlock, 2L << 30))<0) {
    MQ_dprintf3("MQ_R2W_Lock: %s\n",strerror((int)(-ret)));
    goto out;
  }

  if (MQ_RWLock_Cur_Waiter_Count(&testlock,0) != 0) {
    dprintf(STDERR_FILENO,"\nn_waiters spuriously nonzero\n");
    goto out;
  }

  if ((ret=MQ_W_Unlock(&testlock))<0) {
    MQ_dprintf3("MQ_W_Unlock: %s\n",strerror((int)(-ret)));
    goto out;
  }

  nspins = 0;
  while (_MQ_ThreadState_Count() > 1) {
    ++nspins;
    if (nspins > 1000) {
      MQ_dprintf("_MQ_ThreadState_Count() never made it back to 1\n");
      goto out;
    }
    if (nspins<10)
      sched_yield();
    else
      usleep(1000);
  }

  ++times;

  goto again;

 out:

  if ((times < times_max) && (! exitflag)) {
    dprintf(STDERR_FILENO,"\nCompleted %d run%s of %d requested\n",times,times==1?"":"s",times_max);
#ifdef MQ_DEBUG
    dprintf(STDERR_FILENO,"errno backtrace: %s, file=%s, line=%d, function=%s\n",strerror(MQ_errno),MQ_errno_file,MQ_errno_line,MQ_errno_function);
#endif
  } else {
    if ((ret=MQ_RWLock_Destroy(&testlock))<0)
      dprintf(STDERR_FILENO,"MQ_RWLock_Destroy: %s\n",strerror((int)(-ret)));
  }

  dprintf(STDERR_FILENO,"\nat out: ThreadState_Count=%d, R_Count=%u, W_Count=%u, HeldLocks_Index=%d, RW.Owner=%d, Contended_bit=%d, MutEx=%u, MutEx_Locker=%d, MutEx_Spin_Estimator=%d, LFList_Head=%p, Wait_List_Head=%p, n_waiters=%d"
#if defined(MQ_RWLOCK_STATISTICS_SUPPORT) || defined(MQ_RWLOCK_DEBUGGING_SUPPORT)
", nonintrinsic_futex_trips=%lu/%lu, M=%lu/%lu/%lu/%lu Tavg=%.1fus, R=%lu/%lu/%lu Tavg=%.1fus, W=%lu/%lu/%lu Tavg=%.1fus, R2W=%lu/%lu/%lu Tavg=%.1fus, C=%lu/%lu, S=%lu/%lu/%lu\n\tinit=%s@%d/%s();lastWL=[%d]%s@%d/%s();lastWU=[%d]%s@%d/%s();lastFRL=[%d]%s@%d/%s();lastRL=[%d]%s@%d/%s();lastRU=[%d]%s@%d/%s()\n"
#else
"\n"
#endif
,_MQ_ThreadState_Count(),MQ_RWLOCK_R_COUNT(&testlock),MQ_RWLOCK_W_COUNT(&testlock),
#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
__MQ_RWLock_ThreadState.HeldLocks_Index
#else
-1
#endif
, MQ_TSA_To_TID(testlock.RW.Owner),(testlock.RW.Count&MQ_RWLOCK_COUNT_CONTENDED)!=0, testlock.MutEx.Count,MQ_TSA_To_TID(testlock.MutEx.Owner),testlock.MutEx_Spin_Estimator,
	    testlock.LFList_Head,/*testlock.Wait_LL_futex,*/testlock.Wait_List_Head, 
	  MQ_RWLock_Cur_Waiter_Count_Unlocked(&testlock)
#if defined(MQ_RWLOCK_STATISTICS_SUPPORT) || defined(MQ_RWLOCK_DEBUGGING_SUPPORT)
, testlock.futex_trips,
	  futex_trips_possible,
	  testlock.M_Reqs,testlock.M_Adaptives,testlock.M_Waits,testlock.M_Fails, testlock.M_Wait_Time_Cum / ((double)testlock.M_Waits * 1000.0),
	  testlock.R_Reqs,testlock.R_Waits,testlock.R_Fails, testlock.R_Wait_Time_Cum / ((double)testlock.R_Waits * 1000.0),
	  testlock.W_Reqs,testlock.W_Waits,testlock.W_Fails, testlock.W_Wait_Time_Cum / ((double)testlock.W_Waits * 1000.0),
	  testlock.R2W_Reqs,testlock.R2W_Waits,testlock.R2W_Fails, testlock.R2W_Wait_Time_Cum / ((double)testlock.R2W_Waits * 1000.0),
	  testlock.C_Reqs,testlock.C_Fails,
	  testlock.S_Reqs,testlock.S_Misses,testlock.S_Fails,
	    testlock.init_file,testlock.init_line,testlock.init_function,
	    testlock.last_W_Locker_tid,testlock.last_W_Locker_file,testlock.last_W_Locker_line,testlock.last_W_Locker_function,
	    testlock.last_W_Unlocker_tid,testlock.last_W_Unlocker_file,testlock.last_W_Unlocker_line,testlock.last_W_Unlocker_function,
	    testlock.last_fresh_R_Locker_tid,testlock.last_fresh_R_Locker_file,testlock.last_fresh_R_Locker_line,testlock.last_fresh_R_Locker_function,
	    testlock.last_R_Locker_tid,testlock.last_R_Locker_file,testlock.last_R_Locker_line,testlock.last_R_Locker_function,
	    testlock.last_R_Unlocker_tid,testlock.last_R_Unlocker_file,testlock.last_R_Unlocker_line,testlock.last_R_Unlocker_function
#endif	    
	  );

  if ((times < times_max) && (! exitflag) && (testlock.Flags & MQ_RWLOCK_ABORTWHENCORRUPTED))
    MQ_abort();

  exit(((times<times_max) && (! exitflag))?1:0);
}
