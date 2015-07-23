/* MegaQueue
 *
 * Copyright 2015 Daniel Pouzzner
 *
 * mq_threadutil.h  2015feb23  daniel.pouzzner@megaqueue.com
 */

#ifndef MQ_THREADUTIL_H
#define MQ_THREADUTIL_H

#ifndef MQ_NO_PTHREADS
#include <pthread.h>
#define START_NOCANCELS { \
  int oldstate; \
  if ((pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&oldstate) != 0) || \
      (oldstate == PTHREAD_CANCEL_DISABLE)) \
    oldstate = -1

#define END_NOCANCELS if (oldstate != -1) \
  (void)pthread_setcancelstate(oldstate,0); \
}
#else
#define START_NOCANCELS {
#define END_NOCANCELS }
#endif

#if 0
#define MQ_TSA gettid() /* Thread-Specific Attribute */
typedef pid_t MQ_TSA_t;
#define MQ_TSA_TO_TID(tsa) (tsa)
#endif

struct __MQ_RWLock_ThreadState;
struct __MQ_ThreadState {
  pid_t tid;
  pthread_t pth_id;
  uint64_t long_tsa; /* pointer to __MQ_ThreadState, plus tid<<48 */
  volatile union __MQ_SchedState BaseSchedState, CurSchedState;
  struct __MQ_RWLock_ThreadState *RWLock_ThreadState;
  struct __MQ_ThreadState * volatile _prev, * volatile _next;
};

extern __thread_scoped struct __MQ_ThreadState __MQ_ThreadState;
extern __thread_scoped struct __MQ_ThreadState *__MQ_ThreadState_Pointer;
extern int MQ_ThreadState_Init(void);
extern int _MQ_RWLock_ThreadState_Init(void); /* in mq_rwlock_internal.c */

extern volatile int __MQ_ThreadState_Walker_Count;
#define MQ_THREADSTATE_WALK_START() MQ_SyncInt_Increment(__MQ_ThreadState_Walker_Count,1)
#define MQ_THREADSTATE_WALK_END() MQ_SyncInt_Decrement(__MQ_ThreadState_Walker_Count,1)
extern struct __MQ_ThreadState * volatile __MQ_ThreadState_Head, * volatile __MQ_ThreadState_Tail;

extern int _MQ_ThreadState_Assoc_ByTID(pid_t tid, struct __MQ_ThreadState **ts);
extern int _MQ_ThreadState_Assoc_ByTID_prelocked(pid_t tid, struct __MQ_ThreadState **ts);
extern int _MQ_ThreadState_Assoc_ByPthID(pthread_t pth_id, struct __MQ_ThreadState **ts);
extern void _MQ_ThreadState_Assoc_Release(void);

extern int _MQ_ThreadState_Count(void);

extern union __MQ_SchedState __PeakSchedState;

extern int MQ_SchedState_Set(pid_t tid, int policy, int priority);
extern int MQ_SchedState_GetBase(pid_t tid, int *policy, int *priority);
extern int MQ_SchedState_GetCur(pid_t tid, int *policy, int *priority, void **from);

extern int _MQ_SchedState_Elevate(struct __MQ_ThreadState *thr, const union __MQ_SchedState new_ss);
extern int _MQ_SchedState_Elevate_EvenIfEqual(struct __MQ_ThreadState *thr, const union __MQ_SchedState new_ss);
extern int _MQ_SchedState_Update(struct __MQ_ThreadState *thr, union __MQ_SchedState *ref_ss, const union __MQ_SchedState new_ss);

#if 0
#define MQ_TSA ((struct __MQ_RWLock_ThreadState * const)__MQ_RWLock_ThreadState_Pointer) /* force non-lvalue with a no-op cast */
#define MQ_TSA_AUTOINIT (__MQ_RWLock_ThreadState_Pointer ? __MQ_RWLock_ThreadState_Pointer : (_MQ_RWLock_ThreadState_Initialize(), __MQ_RWLock_ThreadState_Pointer))
typedef struct __MQ_RWLock_ThreadState *MQ_TSA_t;
#define MQ_TSA_INITIALIZER ((struct __MQ_RWLock_ThreadState *)0UL)
#define MQ_TSA_BEQUEATHED_VALUE ((struct __MQ_RWLock_ThreadState *)0x7fffffffffffffffUL)
#define MQ_TSA_ValidP(tsa) (! (((uint64_t)(tsa))&(1UL<<63)))
#define MQ_TSA_Invalidate(tsa) ((tsa) = (struct __MQ_RWLock_ThreadState *)(((uint64_t)(tsa))|(1UL<<63)))
#define MQ_TSA_MkInvalid(tsa) ((struct __MQ_RWLock_ThreadState *)(((uint64_t)(tsa))|(1UL<<63)))
#define MQ_TSA_To_TID(tsa) ((MQ_TSA_ValidP(tsa) && (tsa != MQ_TSA_INITIALIZER) && (tsa != MQ_TSA_BEQUEATHED_VALUE)) ? ((tsa)->tid) : -1)
#define MQ_TSA_To_PthID(tsa) ((MQ_TSA_ValidP(tsa) && (tsa != MQ_TSA_INITIALIZER) && (tsa != MQ_TSA_BEQUEATHED_VALUE)) ? ((tsa)->pth_id) : (pthread_t)0)
#endif

#if 0
#define MQ_TSA ((struct __MQ_RWLock_ThreadState * const)__MQ_RWLock_ThreadState_Pointer) /* force non-lvalue with cast */
#define MQ_TSA_AUTOINIT (__MQ_RWLock_ThreadState_Pointer ? __MQ_RWLock_ThreadState_Pointer : (_MQ_RWLock_ThreadState_Initialize(), __MQ_RWLock_ThreadState_Pointer))
typedef uint64_t MQ_TSA_t;
#define MQ_TSA_INVALIDATOR 0x800000000000UL
#define MQ_TSA_INITIALIZER 0x800000000001UL
#define MQ_TSA_BEQUEATHED_VALUE 0x800000000002UL
#define MQ_TSA_ValidP(tsa) (! (((uint64_t)(tsa))&MQ_TSA_INVALIDATOR))
#define MQ_TSA_Invalidate(tsa) (((uint64_t)(tsa)) |= MQ_TSA_INVALIDATOR)
#define MQ_TSA_MkInvalid(tsa) (((uint64_t)(tsa))|MQ_TSA_INVALIDATOR)
#define MQ_TSA_To_TID(tsa) ((MQ_TSA_ValidP(tsa) && (tsa != MQ_TSA_INITIALIZER) && (tsa != MQ_TSA_BEQUEATHED_VALUE)) ? (((struct __MQ_RWLock_ThreadState *)tsa)->tid) : -1)
#define MQ_TSA_To_PthID(tsa) ((MQ_TSA_ValidP(tsa) && (tsa != MQ_TSA_INITIALIZER) && (tsa != MQ_TSA_BEQUEATHED_VALUE)) ? (((struct __MQ_RWLock_ThreadState *)tsa)->pth_id) : (pthread_t)0)
#endif

typedef int32_t MQ_TSA_t;
#define MQ_TSA ((const MQ_TSA_t)__MQ_ThreadState.tid) /* force non-lvalue with cast */
#define MQ_TSA_AUTOINIT (__MQ_ThreadState_Pointer ? __MQ_ThreadState.tid : (MQ_ThreadState_Init(), __MQ_ThreadState.tid))
#define MQ_TSA_INITIALIZER (-1)
#define MQ_TSA_BEQUEATHED_VALUE (-2)
#define MQ_TSA_INVALIDOWNER (-3)
#define MQ_TSA_ValidP(tsa) (tsa>0)
#define MQ_TSA_Invalidate(tsa) ((tsa) = -(tsa))
#define MQ_TSA_MkInvalid(tsa) (-tsa)
#define MQ_TSA_To_TID(tsa) (tsa)
#define MQ_TS_To_TSA(ts) ((ts).tid)
#define MQ_TSP_To_TSA(ts) ((ts)->tid)

typedef uint64_t MQ_TSA_Long_t;
#define MQ_TSA_LONG ((const MQ_TSA_Long_t)__MQ_ThreadState.long_tsa) /* force non-lvalue with cast */
#define MQ_TSA_LONG2(x) ((const MQ_TSA_Long_t)(x)->long_tsa) /* force non-lvalue with cast */
#define MQ_TSA_LONG_POINTER(x) ((struct __MQ_ThreadState *)((x) & 0x7fffffffffff))
#define MQ_TSA_LONG_TO_TID(x) ((pid_t)((x) >> 48UL))
#define MQ_TSA_LONG_TID_BITS 16

#endif
