/* MegaQueue
 *
 * Copyright 2015 Daniel Pouzzner
 *
 * mq_procutil.h  2015mar20  daniel.pouzzner@megaqueue.com
 * 
 * routines for accessing and manipulating process attributes
 */

#ifndef MQ_PROCUTIL_H
#define MQ_PROCUTIL_H

#include <sys/signal.h>

extern __wur int _gettid(void);
#ifdef MQ_NO_PTHREADS
#define gettid() _gettid(void)
#else
extern __thread_scoped pid_t __my_tid;
#define gettid() ((__my_tid==-1) ? _gettid() : __my_tid)
#endif

#define MQ_abort() (abort(),0)

union __MQ_SchedState {
  uint64_t EnBloc;
  struct {
#if __BYTE_ORDER == __LITTLE_ENDIAN
    uint8_t PointerOffsetBytes[6];
    uint8_t policy;
    uint8_t priority; /* 1-99 for realtime policies, kernel-style niceness (1-40, 20 corresponds to the regular default ("0"), 40 is highest priority) for SCHED_OTHER */
#else
    uint8_t priority;
    uint8_t policy;
    uint8_t PointerOffsetBytes[6];
#endif
  };
  struct {
#if __BYTE_ORDER == __LITTLE_ENDIAN
    uint8_t PointerOffsetBytes2[6];
    uint16_t policy_and_priority;
#else
    uint16_t policy_and_priority;
    uint8_t PointerOffsetBytes2[6];
#endif
  };
};


/* note, the pointer in an __MQ_SchedState has non-obvious meaning.
 * in __MQ_ThreadState.BaseSchedState it's just zero.  in the global
 * __PeakSchedulingState it points to the currently associated
 * __MQ_ThreadState.  in __MQ_ThreadState.CurSchedState it is either
 * zero, signifying at own base, or points to an MQ_RWLock_t from
 * which priority is currently inherited, or points to
 * __PeakSchedulingState.
 */

#define MQ_SCHEDSTATE_NEEDSRECOMPUTE_FLAG 0x800000000000UL
#define MQ_SCHEDSTATE_POINTER(ss) ((void *)((ss).EnBloc & 0x7fffffffffffUL))
#define MQ_SCHEDSTATE_MK_ENBLOC(ptr,policy,priority) ((uint64_t)(ptr) | ((uint64_t)(policy) << 48) | ((uint64_t)(priority) << 56))
#define MQ_SCHEDSTATE_MK_ENBLOC2(ptr,policy_and_priority) ((uint64_t)(ptr) | ((uint64_t)(policy_and_priority) << 48))
#define MQ_SCHEDSTATE_CMP(a,b) (((a).policy == SCHED_OTHER) ? ((((b).policy != SCHED_OTHER) || ((b).priority > (a).priority)) ? 1 : (((b).priority == (a).priority) ? 0 : -1)) : ((((b).policy == SCHED_OTHER) || ((b).priority < (a).priority)) ? -1 : (((a).priority == (b).priority) ? 0 : 1)))

extern int _MQ_SchedState_Get_1(pid_t tid, union __MQ_SchedState *ss);
extern int _MQ_SchedState_Set_1(pid_t tid, const volatile union __MQ_SchedState *ss);

#endif
