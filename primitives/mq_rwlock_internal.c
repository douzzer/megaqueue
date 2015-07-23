/* MegaQueue
 *
 * Copyright 2015 Daniel Pouzzner
 *
 * mq_rwlock_internal.c  2015mar27  daniel.pouzzner@megaqueue.com
 * 
 * contention management portion of:
 *
 * LWP locks and signals with parsimonious userspace-only logic in
 * uncontended scenarios, reliable order-of-arrival queueing of
 * waiters (optionally, priority-ordered), mutex-weight W and R locks,
 * error checking on all locks, priority inheritance on RW locks,
 * integrated usage statistics, optimized contended scenario handling
 * with parsimonious syscalling, and integrated cond signalling with
 * reliable argument passing and reliable control of number of woken
 * threads
 */

#ifndef MQ_RWLOCK_INHERITPRIORITY_SUPPORT
#  define MQ_RWLOCK_INHERITPRIORITY_SUPPORT
#endif
#ifndef MQ_RWLOCK_INHERITPEAKPRIORITY_SUPPORT
#  define MQ_RWLOCK_INHERITPEAKPRIORITY_SUPPORT
#endif
#ifndef MQ_RWLOCK_STATISTICS_SUPPORT
#  define MQ_RWLOCK_STATISTICS_SUPPORT
#endif
#ifndef MQ_RWLOCK_DEBUGGING_SUPPORT
#  define MQ_RWLOCK_DEBUGGING_SUPPORT
#endif
#ifndef MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT
#  define MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT
#endif
#ifdef MQ_RWLOCK_NOCOSTLYERRORCHECKING_SUPPORT
#  undef MQ_RWLOCK_NOCOSTLYERRORCHECKING_SUPPORT
#endif
#ifdef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
#  undef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
#endif
#ifdef MQ_RWLOCK_INTELRTM_SUPPORT
#  undef MQ_RWLOCK_INTELRTM_SUPPORT
#endif
#ifndef MQ_RWLOCK_RECURSION_SUPPORT
#  define MQ_RWLOCK_RECURSION_SUPPORT
#endif
#ifndef MQ_RWLOCK_REPORTINVERSIONS_SUPPORT
#  define MQ_RWLOCK_REPORTINVERSIONS_SUPPORT
#endif
#ifndef MQ_RWLOCK_AVERTINVERSIONS_SUPPORT
#  define MQ_RWLOCK_AVERTINVERSIONS_SUPPORT
#endif

#define MQ_RWLOCK_LOCALDEFAULTFLAGS 0UL
#define MQ_RWLOCK_INLINE
#include "mq.h"
#include "mq_rwlock.c"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#ifdef __RTM__
#include <immintrin.h>
#endif

struct MQ_Lock_LFList_Ent {
  MQ_TSA_t waiter;
  struct __MQ_ThreadState *waiter_threadstate;
  MQ_Time_t wait_start_at; /* used for _STATISTICS */
  volatile int granted;
  struct MQ_Lock_LFList_Ent * volatile _prev, * volatile _next;
  volatile int my_priority; /* kernel-style, positive numbers, higher number means higher priority */
  volatile pid_t _prev_tid, _my_tid, _next_tid; /* for MQ_RWLOCK_ROBUST */
};

struct MQ_Lock_Wait_Ent {
  MQ_RWLock_t *l;
  volatile MQ_TSA_t Waiter_TSA;
  struct __MQ_ThreadState *waiter_threadstate;
  MQ_Wait_Type what;
  MQ_Time_t wait_start_at; /* used for _STATISTICS */
  volatile int granted;
  volatile int * volatile granted_ptr;
  void * volatile arg; /* passed by MQ_cond_signal() to MQ_cond_wait() */
  struct MQ_Lock_Wait_Ent * volatile _prev, * volatile _next;
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

#define MQ_RWLOCK_TIMEOUT_DEFAULT (10*MQ_ONE_SECOND);
#define MQ_RWLOCK_SPIN_YIELD_LIMIT 100 /* calls */
#define MQ_RWLOCK_SPIN_USLEEP_INTERVAL 1000 /* microseconds */
#define MQ_RWLOCK_SPIN_DEADLOCK_THRESHOLD 10 /* seconds */

volatile uint64_t __MQ_RWLock_GlobalDefaultFlags = 0;

__thread_scoped volatile void * __MQ_RWLock_AsyncLock[2] = {0,0};

__thread_scoped struct __MQ_RWLock_ThreadState __MQ_RWLock_ThreadState = { .BlockedOnLock = MQ_RWLOCK_HELDLOCK_INITIALIZER, .HeldLocks_Index = MQ_RWLOCK_MAX_TRACKED /* induce initialization */, .InheritedSchedState_From = 0 };

int _MQ_RWLock_ThreadState_Init(void) {
  if (__MQ_RWLock_ThreadState.HeldLocks_Index != MQ_RWLOCK_MAX_TRACKED)
    MQ_returnerrno(EALREADY);
  __MQ_RWLock_ThreadState.HeldLocks_Index = 0;
  __MQ_ThreadState.RWLock_ThreadState = &__MQ_RWLock_ThreadState;
  return 0;
}


#define handlecorruption() \
  do { \
  if (l->Flags & MQ_RWLOCK_ABORTWHENCORRUPTED) \
{\
START_NOCANCELS;\
dprintf(STDERR_FILENO,"handlecorruption abort at line %d\n", __LINE__ );\
END_NOCANCELS;\
    MQ_abort(); \
}\
  MQ_SyncInt_SetFlags(l->Flags,MQ_RWLOCK_LFLIST_CORRUPTED); \
  ret = MQ_seterrpoint(ENOTRECOVERABLE); \
  goto out; \
  } while (0)

#define spinassert(condition) \
  for (int assertspins=0;! (condition);++assertspins) { \
    if (l->Flags & MQ_RWLOCK_LFLIST_CORRUPTED) { \
      ret = MQ_seterrpoint(ENOTRECOVERABLE); \
      goto out; \
    } \
    if (assertspins > (MQ_RWLOCK_SPIN_YIELD_LIMIT+MQ_RWLOCK_SPIN_DEADLOCK_THRESHOLD*(1000000/MQ_RWLOCK_SPIN_USLEEP_INTERVAL))) \
  {\
START_NOCANCELS;\
dprintf(STDERR_FILENO,"assertspins > %d for %s at line %d\n\n",(MQ_RWLOCK_SPIN_YIELD_LIMIT+MQ_RWLOCK_SPIN_DEADLOCK_THRESHOLD*(1000000/MQ_RWLOCK_SPIN_USLEEP_INTERVAL)), #condition, __LINE__ );\
END_NOCANCELS;\
      handlecorruption(); \
  }\
    if (assertspins<MQ_RWLOCK_SPIN_YIELD_LIMIT) \
      sched_yield(); \
    else { \
      START_NOCANCELS; \
      usleep(MQ_RWLOCK_SPIN_USLEEP_INTERVAL); \
      END_NOCANCELS; \
    } \
  }

#define spinassert_noncorrupting(condition) \
  for (int assertspins=0;! (condition);++assertspins) { \
    if (l->Flags & MQ_RWLOCK_LFLIST_CORRUPTED) { \
      ret = MQ_seterrpoint(ENOTRECOVERABLE); \
      goto out; \
    } \
    if (assertspins > (MQ_RWLOCK_SPIN_YIELD_LIMIT+MQ_RWLOCK_SPIN_DEADLOCK_THRESHOLD*(1000000/MQ_RWLOCK_SPIN_USLEEP_INTERVAL))) \
  {\
START_NOCANCELS;\
dprintf(STDERR_FILENO,"assertspins > %d for %s at line %d\n\n",(MQ_RWLOCK_SPIN_YIELD_LIMIT+MQ_RWLOCK_SPIN_DEADLOCK_THRESHOLD*(1000000/MQ_RWLOCK_SPIN_USLEEP_INTERVAL)), #condition, __LINE__ );\
END_NOCANCELS;\
      ret = MQ_seterrpoint(EDEADLK); \
      goto out; \
  }\
    if (assertspins<MQ_RWLOCK_SPIN_YIELD_LIMIT) \
      sched_yield(); \
    else { \
      START_NOCANCELS; \
      usleep(MQ_RWLOCK_SPIN_USLEEP_INTERVAL); \
      END_NOCANCELS; \
    } \
  }


int _MQ_RWLock_LockState_Initialize(MQ_RWLock_t *l) {
  static volatile uint64_t __MQ_RWLock_ID_Counter = 0;

  if (l->ID)
    MQ_returnerrno(EINVAL);

  l->ID = MQ_SyncInt_Increment(__MQ_RWLock_ID_Counter,1);

  return 0;
}

void _MQ_RWLock_LockState_Extricate(__unused MQ_RWLock_t *l) {
}


/* routines for updating a particular thread's schedstate to reflect a newly owned or priority-changed lock */

int _MQ_SchedState_Inherit(MQ_RWLock_t *l) {
  if (l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY) {
    /* easy: always set Cur as peak */
    union __MQ_SchedState cur_peak_ss = { .EnBloc = __PeakSchedState.EnBloc };
    union __MQ_SchedState inher_peak_ss = { .EnBloc = MQ_SCHEDSTATE_MK_ENBLOC(&__PeakSchedState, cur_peak_ss.policy, cur_peak_ss.priority) };
    return _MQ_SchedState_Elevate_EvenIfEqual(__MQ_ThreadState_Pointer, inher_peak_ss);
  } else if (MQ_SCHEDSTATE_POINTER(__MQ_ThreadState.CurSchedState) == &__PeakSchedState)
    return 0;

  union __MQ_SchedState cur_lock_ss = { .EnBloc = l->SchedState.EnBloc };

  if (MQ_SCHEDSTATE_CMP(__MQ_ThreadState.CurSchedState,cur_lock_ss) <= 0) {
    if ((MQ_SCHEDSTATE_POINTER(__MQ_ThreadState.CurSchedState) != l) || (__MQ_ThreadState.CurSchedState.policy_and_priority == cur_lock_ss.policy_and_priority))
      return 0;
    return _MQ_SchedState_Disinherit(0);
  }

  union __MQ_SchedState inher_lock_ss = { .EnBloc = MQ_SCHEDSTATE_MK_ENBLOC(&l, cur_lock_ss.policy, cur_lock_ss.priority) };
  return _MQ_SchedState_Elevate(__MQ_ThreadState_Pointer, inher_lock_ss);
}

int _MQ_SchedState_Inherit_ByThr(struct __MQ_ThreadState *thr, MQ_RWLock_t *l) {
  if (l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY) {
    /* easy: always set Cur as peak */
    union __MQ_SchedState cur_peak_ss = { .EnBloc = __PeakSchedState.EnBloc };
    union __MQ_SchedState inher_peak_ss = { .EnBloc = MQ_SCHEDSTATE_MK_ENBLOC(&__PeakSchedState, cur_peak_ss.policy, cur_peak_ss.priority) };
    return _MQ_SchedState_Elevate_EvenIfEqual(thr, inher_peak_ss);
  } else if (MQ_SCHEDSTATE_POINTER(thr->CurSchedState) == &__PeakSchedState)
    return 0;

  union __MQ_SchedState cur_lock_ss = { .EnBloc = l->SchedState.EnBloc };

  if (MQ_SCHEDSTATE_CMP(thr->CurSchedState,cur_lock_ss) <= 0) {
    if ((MQ_SCHEDSTATE_POINTER(thr->CurSchedState) != l) || (thr->CurSchedState.policy_and_priority == cur_lock_ss.policy_and_priority))
      return 0;
    return _MQ_SchedState_Disinherit_ByThr(thr,0);
  }

  union __MQ_SchedState inher_lock_ss = { .EnBloc = MQ_SCHEDSTATE_MK_ENBLOC(&l, cur_lock_ss.policy, cur_lock_ss.priority) };
  return _MQ_SchedState_Elevate(thr, inher_lock_ss);
}

/* xxx probably useless: */
int _MQ_SchedState_Inherit_ByTID(pid_t tid, MQ_RWLock_t *lock) {
  if (! tid)
    return _MQ_SchedState_Inherit_ByThr(__MQ_ThreadState_Pointer, lock);
  else {
    struct __MQ_ThreadState *thr;
    int ret;
    if ((ret=_MQ_ThreadState_Assoc_ByTID(tid, &thr)))
      return ret;
    ret = _MQ_SchedState_Inherit_ByThr(thr, lock);
    _MQ_ThreadState_Assoc_Release();
    return ret;
  }
}

/* routines for updating a particular thread's schedstate to reflect change or release of a lock */

int _MQ_SchedState_Disinherit(MQ_RWLock_t *l) {
  /* not so easy: survey held locks for possible inherited lock pri (including another MQ_RWLOCK_INHERITPEAKPRIORITY lock)  */
  union __MQ_SchedState cur_thread_ss = { .EnBloc = __MQ_ThreadState.CurSchedState.EnBloc };

  if (l &&
      (MQ_SCHEDSTATE_POINTER(cur_thread_ss) != l) &&
      (! ((l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY) && (MQ_SCHEDSTATE_POINTER(cur_thread_ss) == &__PeakSchedState))) &&
      (! (cur_thread_ss.EnBloc & MQ_SCHEDSTATE_NEEDSRECOMPUTE_FLAG)))
    return 0;

  union __MQ_SchedState max_ss_so_far = { .EnBloc = 0 }, test_ss;

  int lock_index;
  for (lock_index=__MQ_RWLock_ThreadState.HeldLocks_Index-1;lock_index>=0;--lock_index) {
    MQ_RWLock_t *this_l = MQ_RWLOCK_TRACK_POINTER(__MQ_RWLock_ThreadState.HeldLocks[lock_index]);
    if (this_l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY) {
      union __MQ_SchedState cur_peak_ss = { .EnBloc = __PeakSchedState.EnBloc };
      max_ss_so_far.EnBloc = MQ_SCHEDSTATE_MK_ENBLOC(&__PeakSchedState, cur_peak_ss.policy, cur_peak_ss.priority);
      break;
    }
    if (this_l->Flags & MQ_RWLOCK_DUMMY) /* currently MQ_RWLOCK_DUMMY is used only when MQ_RWLOCK_INHERITPEAKPRIORITY, but be proactive */
      continue;
    test_ss.EnBloc = this_l->SchedState.EnBloc;
    if (MQ_SCHEDSTATE_CMP(max_ss_so_far,test_ss) > 0)
      max_ss_so_far.EnBloc = MQ_SCHEDSTATE_MK_ENBLOC(this_l, test_ss.policy, test_ss.priority);
  }

  if (max_ss_so_far.EnBloc == 0)
    max_ss_so_far.EnBloc = __MQ_ThreadState.BaseSchedState.EnBloc;

  (void)_MQ_SchedState_Update(__MQ_ThreadState_Pointer, &cur_thread_ss, max_ss_so_far);

/*
xxx can't just rifle through these -- the thread might unlock some in the meantime and then they might be deallocated

it's enormously complicated and expensive to do anything other than to
have all priority reductions be self-applied by threads when they drop
a lock.

thus priority elevations of any cause (app-level set, rise in the
peak, nth order inheritance) are propagated asynchronously of
necessity, but elevated priority drop for a thread is deferred until
the thread's next unlock.


when propagating an elevation, it will by definition always be the
case that each lock examined will have at least one thread blocked on
it, which is why we're interested in the lock in the first place, and
SyncInt BlockedOnLock ops on that blocked thread assures the lock
cannot disappear from under the walker.  either BlockedOnLock will be
clear or point at another lock, preempting the cmpxchgq, or it will
have the walker bit set, forcing it to stay blocked until the walker
unblocks it.

*/

  return 0;
}

/* caller guarantees that thr is blocked --
 * this is only safe to call from places where the target thread is blocked and it's arranged or knowable that it will stay blocked,
 * This is used in the contention handlers
 * to recalculate the pri of an extricating or granted thread
 * whenever the lock stops being contended.

 * (when a lock stops being contended, its SchedState goes to zero)

 * this is crucial, so that a subsequent uncontended unlock doesn't
 * leave a thread at spuriously elevated priority, and by doing it in
 * the contention handlers themselves (which are the only places where
 * the contention bit can be cleared), PI entails no computational
 * overhead in the uncontended scenario.

 * with RW locks it looks like it will be perfectly effective to simply couple PI recalculation with unlock in the contended path, and
 * reset of lock ss with contention bit clearing.

 * it *is* possible for the contention handlers to apply priority
 * de-elevation to other waiting threads after a waiter extricates,
 * but best not to, since it's not possible to propagate the
 * de-elevation through any chained inheritance, because the thread of
 * control has no practical/efficient way to effect suspension of
 * extrication/granting in chained lock(s), resulting in a
 * deallocation race.


 * maybe recalculate a thread's pri every time a contention handler wakes it up?  probably not

 */


int _MQ_SchedState_Disinherit_ByThr(struct __MQ_ThreadState *thr, MQ_RWLock_t *l) {
  /* not so easy: survey held locks for possible inherited lock pri (including another MQ_RWLOCK_INHERITPEAKPRIORITY lock)  */
  union __MQ_SchedState cur_thread_ss = { .EnBloc = thr->CurSchedState.EnBloc };

  union __MQ_SchedState max_ss_so_far = { .EnBloc = 0 }, test_ss;
  int lock_index;

 do_it_again:

  if (l &&
      (MQ_SCHEDSTATE_POINTER(cur_thread_ss) != l) &&
      (! ((l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY) && (MQ_SCHEDSTATE_POINTER(cur_thread_ss) == &__PeakSchedState))) &&
      (! (thr->CurSchedState.EnBloc & MQ_SCHEDSTATE_NEEDSRECOMPUTE_FLAG)))
    return 0;

  for (lock_index=thr->RWLock_ThreadState->HeldLocks_Index-1;lock_index>=0;--lock_index) {
    MQ_RWLock_t *this_l = MQ_RWLOCK_TRACK_POINTER(thr->RWLock_ThreadState->HeldLocks[lock_index]);
    if (this_l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY) {
      union __MQ_SchedState cur_peak_ss = { .EnBloc = __PeakSchedState.EnBloc };
      max_ss_so_far.EnBloc = MQ_SCHEDSTATE_MK_ENBLOC(&__PeakSchedState, cur_peak_ss.policy, cur_peak_ss.priority);
      break;
    }
    if ((! (this_l->Flags & MQ_RWLOCK_INHERITPRIORITY)) || (this_l->Flags & MQ_RWLOCK_DUMMY)) /* currently MQ_RWLOCK_DUMMY is used only when MQ_RWLOCK_INHERITPEAKPRIORITY, but be proactive */
      continue;
    test_ss.EnBloc = this_l->SchedState.EnBloc;
    if (MQ_SCHEDSTATE_CMP(max_ss_so_far,test_ss) > 0)
      max_ss_so_far.EnBloc = MQ_SCHEDSTATE_MK_ENBLOC(this_l, test_ss.policy, test_ss.priority);
  }

  if (max_ss_so_far.EnBloc == 0)
    max_ss_so_far.EnBloc = thr->BaseSchedState.EnBloc;

  if (_MQ_SchedState_Update(thr, &cur_thread_ss, max_ss_so_far)) {
    max_ss_so_far.EnBloc = 0UL;
    goto do_it_again;
  }

  return 0;
}


static const uint64_t __PeakDummyLock = MQ_RWLOCK_INHERITPEAKPRIORITY|MQ_RWLOCK_DUMMY;

/* non-inheritance elevated priority, for internal critical sections, exposed for direct usage by applications */
int MQ_SchedState_AdoptPeak(void) {
  for (int i=__MQ_RWLock_ThreadState.HeldLocks_Index-1;i>=0;--i)
    if (__MQ_RWLock_ThreadState.HeldLocks[i].ID_with_status == ((uint64_t)(&__PeakDummyLock)|MQ_RWLOCK_TRACK_DUMMY))
      MQ_returnerrno(EALREADY);
  if (__MQ_RWLock_ThreadState.HeldLocks_Index == MQ_RWLOCK_MAX_TRACKED)
    MQ_returnerrno(ENOLCK);
  __MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index++].ID_with_status = (uint64_t)(&__PeakDummyLock)|MQ_RWLOCK_TRACK_DUMMY;
  union __MQ_SchedState cur_peak_ss = { .EnBloc = __PeakSchedState.EnBloc };
  union __MQ_SchedState inher_peak_ss = { .EnBloc = MQ_SCHEDSTATE_MK_ENBLOC(&__PeakSchedState, cur_peak_ss.policy, cur_peak_ss.priority) };
  return _MQ_SchedState_Elevate(__MQ_ThreadState_Pointer, inher_peak_ss);
}

int MQ_SchedState_ReleasePeak(void) {
  int lock_index;

  for (lock_index=__MQ_RWLock_ThreadState.HeldLocks_Index-1;lock_index>=0;--lock_index) {
    if (MQ_RWLOCK_TRACK_ID(__MQ_RWLock_ThreadState.HeldLocks[lock_index]) != (uint64_t)(&__PeakDummyLock))
      continue;
    if (MQ_RWLOCK_TRACK_TYPE(__MQ_RWLock_ThreadState.HeldLocks[lock_index]) == MQ_RWLOCK_TRACK_DUMMY)
      break;
    MQ_returnerrno(EINVAL); /* can't happen */
  }

  if (lock_index<0)
    MQ_returnerrno(EINVAL);

  if (lock_index == __MQ_RWLock_ThreadState.HeldLocks_Index-1) {
    for (; (lock_index > 0) && (! __MQ_RWLock_ThreadState.HeldLocks[lock_index-1].ID_with_status); --lock_index);
    __MQ_RWLock_ThreadState.HeldLocks_Index = lock_index;
  } else
    __MQ_RWLock_ThreadState.HeldLocks[lock_index].ID_with_status = 0;

  return _MQ_SchedState_Disinherit(0);
}


/* the PI system.  on PI locks, the heritable priority is derived
 * entirely from the RW lock wait list.
 *
 * Thus, applications that need mutex semantics with PI should use W
 * locks, not bare mutexes directly.
 */


/* caller must own l->MutEx */
static int _MQ_SchedState_UpdateLock_1(MQ_RWLock_t *l, union __MQ_SchedState *new_max_ss) {
  union __MQ_SchedState end_lock_ss = { .EnBloc = 0 }, test_ss;

  end_lock_ss.EnBloc = 0UL;

  {
    struct MQ_Lock_Wait_Ent *i;

    for (i=l->Wait_List_Head;i;i=i->_next) {
      test_ss.EnBloc = i->waiter_threadstate->CurSchedState.EnBloc;
      if (MQ_SCHEDSTATE_CMP(test_ss,end_lock_ss) < 0)
	end_lock_ss.EnBloc = test_ss.EnBloc;
    }      
  }

  l->SchedState.EnBloc = end_lock_ss.EnBloc;
  new_max_ss->EnBloc = end_lock_ss.EnBloc;

  return 0;
}

static int _MQ_SchedState_UpdateLockForBlockedThread_1(struct __MQ_ThreadState *thr, int pre_accesslocked);

/* caller must have control of thr */
static int _MQ_SchedState_UpdateThread_1(struct __MQ_ThreadState *thr, MQ_RWLock_t *l) {

  union __MQ_SchedState start_rw_holder_thr_ss = { .EnBloc = thr->CurSchedState.EnBloc };

  if (MQ_SCHEDSTATE_CMP(start_rw_holder_thr_ss,l->SchedState) > 0) {
    union __MQ_SchedState end_rw_holder_thr_ss = { .EnBloc = MQ_SCHEDSTATE_MK_ENBLOC2(l,l->SchedState.policy_and_priority)};
    _MQ_SchedState_Elevate(thr, end_rw_holder_thr_ss);
    if (thr->RWLock_ThreadState->BlockedOnLock.ID_with_status)
      _MQ_SchedState_UpdateLockForBlockedThread_1(thr,0);
  } else if ((MQ_SCHEDSTATE_POINTER(start_rw_holder_thr_ss) == l) && (start_rw_holder_thr_ss.policy_and_priority != l->SchedState.policy_and_priority)) {

    /* if sub_thr is blocked, get the _ACCESSLOCK, update the thread, and if its pri changed, recurse through it to the lock it's blocked on */

    struct HeldLock start_BlockedOnLock = { .ID_with_status = thr->RWLock_ThreadState->BlockedOnLock.ID_with_status };

    if (start_BlockedOnLock.ID_with_status) {
      struct HeldLock end_BlockedOnLock;
      do {
	if (! MQ_RWLOCK_TRACK_POINTER(start_BlockedOnLock))
	  goto set_recompute_flag;
	/* if another thread already locked it, spin here until it's unlocked: */
	if (start_BlockedOnLock.ID_with_status & MQ_RWLOCK_TRACK_ACCESSLOCK)
	  start_BlockedOnLock.ID_with_status &= ~MQ_RWLOCK_TRACK_ACCESSLOCK;
	end_BlockedOnLock.ID_with_status = start_BlockedOnLock.ID_with_status | MQ_RWLOCK_TRACK_ACCESSLOCK;
      } while (! MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(thr->RWLock_ThreadState->BlockedOnLock.ID_with_status,
							    start_BlockedOnLock.ID_with_status,
							    end_BlockedOnLock.ID_with_status));

      MQ_SyncInt_Fence_Acquire(); /* coordinate with _MQ_R_Unlock_contended() to assure this thread sees HeldLocks updates that occured before getting the mutex */

      /* we've now locked the thread and can iterate through its HeldLocks to compute its new pri */
      _MQ_SchedState_Disinherit_ByThr(thr, 0 /* MQ_RWLock_t * */);

      /* and now that its new pri is established, we can update lock holder(s), possibly recursively */
      _MQ_SchedState_UpdateLockForBlockedThread_1(thr,1);

      MQ_SyncInt_ClearFlags(thr->RWLock_ThreadState->BlockedOnLock.ID_with_status,MQ_RWLOCK_TRACK_ACCESSLOCK);

    } else {
      /* set the _NEEDSRECOMPUTE flag on the ss of threads that need to be de-elevated but are neither self nor blocked.
       * by definition such threads are not blocked, so cannot have been inherited by any locks, so yippee!
       */
      union __MQ_SchedState end_rw_holder_thr_ss;
    set_recompute_flag:
      do {
	if (start_rw_holder_thr_ss.EnBloc & MQ_SCHEDSTATE_NEEDSRECOMPUTE_FLAG)
	  break;
	end_rw_holder_thr_ss.EnBloc = start_rw_holder_thr_ss.EnBloc | MQ_SCHEDSTATE_NEEDSRECOMPUTE_FLAG;
      } while (! MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(thr->CurSchedState.EnBloc,start_rw_holder_thr_ss.EnBloc,end_rw_holder_thr_ss.EnBloc));
    }

  }

  return 0;
}

/* recursive */
static int _MQ_SchedState_UpdateLockHolders_1(MQ_RWLock_t *l, MQ_TSA_t exclude_thread) {
  /* if this is an elevation, we propagate it through the recursive chain of locks and lock holders, applying the elevation to each thread as encountered.
   *
   * but if this is a lock drop or de-prioritization, we propagate it to a lock with a synchronous recompute only if we hold the mutex on that lock, else we
   * propagate by setting the _NEEDSRECOMPUTE_FLAG on the lock so that it will be recomputed asynchronously as described below.
   *
   * in either case -- raise or lower -- we have to iterate through the holding thread(s) not only to update their pris to reflect the lock's updated pri,
   * but to check if each is blocked, and iff it is, call this recursively to descend into the lock it's blocked on.
   *
   * need to set the _NEEDSRECOMPUTE_FLAG on any thread CurSchedState derived from a lock with the _NEEDSRECOMPUTE_FLAG set, and recompute at each
   * opportunity (contended lock/unlock op) as long as it's set.

   * and every lock/unlock of a _NEEDSRECOMPUTE_FLAG lock also entails recompute as long as it's set, and a blocked thread with _NEEDSRECOMPUTE_FLAG CurSchedState
   * carries through to the lock it's blocked on iff that lock's computed pri derives from a _NEEDSRECOMPUTE_FLAG thread.

   * this will propagate the pri de-el for a lock drop or app-level pri drop by a thread through a chain of arbitrary length as fast as possible, but no faster.


the big problem is when a contended lock becomes uncontended because the last contender times out.
ah, not a big problem.  in that case the lock pri is always zeroed, and the holder's pri is -- whoops, problem.
ah, only minor problem.  have to recompute the holder's pri asynchronously and with only the walker lock, by walking its held locks until the computed new ss is the same twice in a row (always walk HeldLocks at least twice).  ah, but apply the new pri after each walk, so that the last apply is bracketed by matching walks.

the critical point with the above is that, one way or another, any
thread that is no longer inheriting any priority will always be
returned to its BaseSchedState by the contention handlers, assuring
that an elevated priority doesn't leak (noting well that the
uncontended unlock pathways have no PI logic in them at all).

if the holder is blocked on another lock, then after updating the holder's ss, have to use the _ACCESSLOCK to do the usual recursion.


and XXX note: *always* have to get the _ACCESSLOCK to descend through
a thread to a lock on the basis of that thread's blockage on that
lock, else there is a deallocation race on the lock.  the lock passed
to _MQ_SchedState_UpdateLockHolders_1() must have been secured either
with its mutex owned by the caller, or with an _ACCESSLOCK on a thread
blocked on it.


it is *always* an application programmer error if a mutex
deadlocks/timeouts on any recursion path, because the PI tree walker gets the mutexes 
definitionally in exactly the same order that the present
lock holders and waiters got them in.

so absent app error, including direct app-level manipulation of the
mutex, the mutex will always be successfully obtained.

   */


  /* make sure this lock doesn't stay in or escape to uncontended status while participating in PI, as that would cause owner elevation to leak */

  int got_mutex;
  if (l->MutEx.Owner != MQ_TSA) {
    int64_t mutex_ret;
    MQ_Time_t timeout = MQ_ONE_SECOND/100; /* never deadlock inside the PI system, even when application in error */
    if ((mutex_ret=_MQ_RWLock_MutEx_Lock(l,&timeout))<0)
      return (int)mutex_ret;
    got_mutex = 1;
  } else
    got_mutex = 0;

  if (! (MQ_RWLOCK_RW_RAWCOUNT(l) & MQ_RWLOCK_COUNT_CONTENDED))
    MQ_SyncInt_SetFlags(MQ_RWLOCK_RW_RAWCOUNT(l),MQ_RWLOCK_COUNT_CONTENDED);


  /* with the mutex held and the contended bit set, all other activity on the lock is now blocked, as needed.
   * now we can:
   * (1) update the lock's ss, based on its waiters
   * (2) update its holders, based on its new ss
   */

  struct __MQ_ThreadState *rw_owner_thr;
  if ((MQ_TSA_ValidP(l->RW.Owner)) && (l->RW.Owner != exclude_thread) && (l->RW.Owner != MQ_TSA) && (! _MQ_ThreadState_Assoc_ByTID_prelocked(l->RW.Owner, &rw_owner_thr))) {

    /* now have the owner threadstate and a recent schedstate for it.
     * adjust it as indicated and possible, and if it's currently
     * blocked, recurse through it using
     * _MQ_SchedState_UpdateLockForBlockedThread_1(), which will come
     * back here if that lock's ss had to be changed.
     */

    _MQ_SchedState_UpdateThread_1(rw_owner_thr, l);

  } else if (l->RW.Count&MQ_RWLOCK_COUNT_READ) {
    /* high comedy: iterate through every held lock of every registered thread, looking for l.  such are the wages of PI on shared locks. */

    /* there is no race here because the HeldLock is removed before
       the lock is released.  thus either there will be no PI and the
       lock will be released uncontended, or the contended bit will
       have been set in time to catch the cmpxchgq and force the
       unlock through the contended path that disinherits any PI.
       there is a finite but negligible chance that the HeldLock
       release and lockcore release will be split, so that the
       lockcore is held without proper PI.
    */

    MQ_SyncInt_Fence_Acquire(); /* coordinate with _MQ_R_Unlock_contended() to assure this thread sees HeldLocks updates that occured before getting the mutex */

    /* also make sure the number of holders matches RW.Count -- the above fence combined with count-matching fully compensates for the non-atomic ops on HeldLocks */

    struct __MQ_ThreadState *holding_thr;
    int i, n_holders;

/*  do_it_again: */
    n_holders = 0;

    for (holding_thr=__MQ_ThreadState_Tail; holding_thr; holding_thr=holding_thr->_prev) {
      if ((! holding_thr->RWLock_ThreadState) ||
	  (holding_thr->tid == exclude_thread) ||
	  (MQ_RWLOCK_TRACK_POINTER(holding_thr->RWLock_ThreadState->BlockedOnLock) == l)) /* sometimes a blocked thread will sneak through before BlockedOnLock
											   * is set, and get updated while blocked.  That's completely harmless.
											   */
	continue;
      for (i=holding_thr->RWLock_ThreadState->HeldLocks_Index-1; i>=0; --i)
	if (MQ_RWLOCK_TRACK_POINTER(holding_thr->RWLock_ThreadState->HeldLocks[i]) == l)
	  break;
      if (i>=0) {
	++n_holders;
	_MQ_SchedState_UpdateThread_1(holding_thr, l);
      }
    }

/*    if (n_holders < (int)MQ_RWLOCK_R_COUNT(l))
 *      goto do_it_again;
 */

  } /* else the lock was freed in the meantime (some time before
     * _COUNT_CONTENDED was set), and now there's nothing to do (and
     * the caller will get the lock as soon as it tries to wait for
     * it, preempting the futex()).
     */

  if (got_mutex)
    return _MQ_RWLock_MutEx_Unlock(l);
  else
    return 0;
}

static int _MQ_SchedState_UpdateLockHolders(MQ_RWLock_t *l, MQ_TSA_t exclude_thread) {
  int ret;

  MQ_THREADSTATE_WALK_START();
  ret = _MQ_SchedState_UpdateLockHolders_1(l, exclude_thread);
  MQ_THREADSTATE_WALK_END();

  return ret;
}


/* when a blocked thread's priority needs to be inherited by the lock
 * it's blocked on, either because the thread just blocked on the
 * lock, or the thread's pri changed, this routine is called.  If
 * the lock's pri changes, it recurses through _MQ_SchedState_UpdateLockHolders_1().
 *
 * because the pri update is idempotent and descent stops wherever the
 * update is without effect, recursive loops cannot occur regardless
 * of programmer error (deadlock induction).
 */
static int _MQ_SchedState_UpdateLockForBlockedThread_1(struct __MQ_ThreadState *thr, int pre_accesslocked) {
  int ret;
  MQ_RWLock_t *l;

  /* first, establish control of the thread (to eliminate races with granting), and confirm it's (still) blocked */

  if (thr != __MQ_ThreadState_Pointer) {
    struct HeldLock start_BlockedOnLock = { .ID_with_status = MQ_SyncInt_Get(thr->RWLock_ThreadState->BlockedOnLock.ID_with_status) };

    /* if thr is not/no longer blocked, return without action */
    if (! MQ_RWLOCK_TRACK_POINTER(start_BlockedOnLock))
      return 0;

    /* if caller's curpri is less than thr's curpri, get peakpri, which must be undone by wrapper function */
    if (MQ_SCHEDSTATE_CMP(thr->CurSchedState,__MQ_ThreadState.CurSchedState) < 0)
      MQ_SchedState_AdoptPeak();

    if (! pre_accesslocked) {
      struct HeldLock end_BlockedOnLock;
      do {
	if (! MQ_RWLOCK_TRACK_POINTER(start_BlockedOnLock))
	  return 0;
	/* if another thread already locked it, spin here until it's unlocked: */
	if (start_BlockedOnLock.ID_with_status & MQ_RWLOCK_TRACK_ACCESSLOCK)
	  start_BlockedOnLock.ID_with_status &= ~MQ_RWLOCK_TRACK_ACCESSLOCK;
	end_BlockedOnLock.ID_with_status = start_BlockedOnLock.ID_with_status | MQ_RWLOCK_TRACK_ACCESSLOCK;
      } while (! MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(thr->RWLock_ThreadState->BlockedOnLock.ID_with_status,
							    start_BlockedOnLock.ID_with_status,
							    end_BlockedOnLock.ID_with_status));
    }
    l = MQ_RWLOCK_TRACK_POINTER(start_BlockedOnLock);
  } else {
    l = MQ_RWLOCK_TRACK_POINTER(thr->RWLock_ThreadState->BlockedOnLock);
    if (! l)
      return 0;
  }

  /* second, update the implicated lock to reflect the thread's CurSchedState */

  int got_mutex;
  if (l->MutEx.Owner != MQ_TSA) {
    int64_t mutex_ret;
    MQ_Time_t timeout = MQ_ONE_SECOND/100; /* never deadlock inside the PI system, even when application in error */
    if ((mutex_ret=_MQ_RWLock_MutEx_Lock(l,&timeout))<0) {
      got_mutex = 0;
      ret = (int)mutex_ret;
      goto out;
    }
    got_mutex = 1;
  } else
    got_mutex = 0;

  /* no need to check _COUNT_CONTENDED -- thr is blocked, guaranteeing the bit is set */

  { /* block ends when done with {start,end}_lock_ss */

  union __MQ_SchedState start_lock_ss = { .EnBloc = l->SchedState.EnBloc };

  if ((start_lock_ss.policy_and_priority == thr->CurSchedState.policy_and_priority) &&
      (! ((start_lock_ss.EnBloc & MQ_SCHEDSTATE_NEEDSRECOMPUTE_FLAG) && (! (thr->CurSchedState.EnBloc & MQ_SCHEDSTATE_NEEDSRECOMPUTE_FLAG))))) {
    ret = 0;
    goto out;
  }

  union __MQ_SchedState end_lock_ss;

  if ((MQ_SCHEDSTATE_CMP(start_lock_ss,thr->CurSchedState) < 0) &&
      (((MQ_SCHEDSTATE_POINTER(start_lock_ss) == thr)) || (start_lock_ss.EnBloc & MQ_SCHEDSTATE_NEEDSRECOMPUTE_FLAG))) {
    if (((ret=_MQ_SchedState_UpdateLock_1(l,&end_lock_ss))<0) ||
	(end_lock_ss.EnBloc == start_lock_ss.EnBloc))
      goto out;
  } else {
    do {
      end_lock_ss.EnBloc = MQ_SCHEDSTATE_MK_ENBLOC2(thr,thr->CurSchedState.policy_and_priority);
      if ((MQ_SCHEDSTATE_CMP(start_lock_ss,end_lock_ss) < 0) ||
	  ((start_lock_ss.EnBloc & MQ_SCHEDSTATE_NEEDSRECOMPUTE_FLAG) && (! MQ_SCHEDSTATE_CMP(start_lock_ss,end_lock_ss)))) {
	/* this thread has no [longer any] bearing on this lock's priority, so we can bail forthwith */
	ret = 0;
	goto out;
      }
    } while (! (MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(l->SchedState.EnBloc,start_lock_ss.EnBloc,end_lock_ss.EnBloc)));
  }

  /* the lock is updated.  now update the lock's holder(s), copying over the _NEEDSRECOMPUTE_FLAG if it's set on the lock. */
  ret = _MQ_SchedState_UpdateLockHolders_1(l,MQ_TSP_To_TSA(thr));

  }

  out:

  if (got_mutex)
    (void)_MQ_RWLock_MutEx_Unlock(l);

  if ((thr != __MQ_ThreadState_Pointer) && (! pre_accesslocked))
    MQ_SyncInt_ClearFlags(thr->RWLock_ThreadState->BlockedOnLock.ID_with_status,MQ_RWLOCK_TRACK_ACCESSLOCK);

  return ret;
}




  /*

   * xxx drop scenarios:
   * thread A at highpri is blocked on lock 1, currently held by B which is blocked on 2, currently held by C which is blocked on 3.  thus 2's and 3's holders
   * inherit A's highpri.  when A is granted 1, 1's pri is recomputed at once, to remove A from its pri contributors.
   * now B can be recomputed at once also, to remove 1 from its pri contributors.
   * ah but that couldn't happen unless B stopped waiting for 2 (granted or timed out).
   *
   * in fact, if there are no time-outs, then by definition the inheritances have to unroll with grants starting with the deepest, to the shallowest, in order, so that
   * lock pris can always be recomputed decisively at the time of grant.


   * but let's say A times out on 1 instead.  now 1 is recomputed at once.  B is still blocked on 2.  we can recompute B using the _TRACK_ACCESSLOCK trick as usual.
   * but we can't recompute 2 because we don't have its mutex, and we certainly can't try to get that mutex, given that it's already contended and blocking thread(s).
   * so all we can do is set the _NEEDSRECOMPUTE_FLAG on it, deferring recompute until the next unlock gives us a thread to work with that owns the mutex.
   * (we also can't recompute C now.)
   *
   * continuing the A-times-out scenario:
   * at time out, we can still iterate through the holders to recurse through any that are blocked, and set the _NEEDSRECOMPUTE_FLAG on any lock ss that references
   * the proximal thread, X, through which we descended to reach that lock.
   *
   * but what use is a recompute if X's pri hasn't actually been updated?  and it cannot have been if its CurSchedState comes from a lock that has its _NEEDSRECOMPUTE_FLAG
   * set.  the recompute won't actually change the lock's pri, because X still reflects the pri that was actually dropped prompting the setting of _NEEDSRECOMPUTE_FLAG.


   *
   *
   * a thread's CurSchedState pointer is always either null (equals BaseSchedState), or points at __PeakSchedState, or points at the RW_Lock_t of a lock it holds.
   * a lock's SchedState pointer is either null (when there are no waiters), or points at the __MQ_ThreadState of a thread waiting for it.
   *
   * thus when inheritance is chained through multiple locks, the thread originating the elevated priority is only referenced by the first lock, for which it waits.
   */

  /* if thr's curpri is greater than the pri of the lock it's blocked on, elevate the lock's pri, and if the elevation succeeded,
   *
   * iterate through the lock holders.  for mutex and W locks, this is easy -- carefully read out MutEx_Owner_Long and RW_Owner_Long.
   * for R locks, this is annoying: walk the thread list from __MQ_ThreadState_Tail, and for each thread, iterate through
   * thr->RWLock_ThreadState->HeldLocks starting at thr->RWLock_ThreadState->HeldLocks_Index-1, and if the lock of interest is found,
   * it's a holder.
   *
   * for each blocked holder, call this func recursively.
   */


/*
  logic for updating each lock with a connection to an updated thread, and each thread with a connection to each such lock.

  when a thread's cur priority is updated, the new priority passes to a lock it is waiting for, if any.  if the update is an elevation,
  the lock pri is elevated or not (immediate return) as a function of its current pri, else the new lock pri is recomputed as the highest cur pri of
  all threads waiting for the lock,
  that priority in turn passes to any thread that currently holds the lock, recursively.  free for mutex/write, annoying for read.


after propagating a priority to a thread, need to re-check that that thread still holds the lock at issue, and reverse the inheritance if not.
because any actual change to the thread's priority will involve a syscall, there is no practical way a material inheritance can be effected spuriously
after the thread releases the lock but before it is visible through the HeldLocks mechanism, despite non-SyncInt operations in HeldLocks.

to use HeldLocks, after extracting a HeldLock from it, remember the
lock_index, and recheck that the lock is still held after applying the
inheritance by first checking if thr->HeldLocks_Index <= lock_index,
and rechecking the contents in the slot at lock_index; if either check
fails, undo the inheritance, then redo that thread from the top.



XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
big solution:
the BlockedOnLock slot in ThreadState is accessed exclusively with SyncInt ops.
the slot has a bit for signifying that the referenced lock is currently accessed by a pri inheritance hierarchy walker.
the contended unlock handler locked-cmpxchgq's the am-waiting-for slot to remove the is-waiting bit, but if the
is-accessed bit is currently set, it spins and retries until that bit clears.

note, the pri inheritance hierarchy walker definitely has to run at peak pri.

when a lock's pri has to be calculated after a waiter no longer waits (extricates or is granted), the
wait queue has to be walked.  for direct inheritance, fortunately, by definition, the codepoints where waiters are removed from the queue
are all places the requisite lock is already held, so the walk can just be done there without any additional locking.

those critical stretches all need to be running at peak pri in any case.

unfortunately, when a lock is elevated through indirect inheritance, its wait queue has to be walked 
this is the case *any time* the priority of the RW_Lock_t.SchedState thread is lowered -- the lock's wait queue has to be walked to find the new RW_Lock_t.SchedState

using the BlockedOnLock mechanism to be deallocation-race-free, and the peakpri mechanism to be pri-inversion-free, this is safe.



how to deal with lock grants -- they can sever arbitrarily large chunks of PI hierarchy --
the contended unlock handler needs to pivot a PI reversion walk on the BlockedOnLock value?

handled simply, by deferring such a de-elevation of a thread until that thread drops the implicated lock

*/


/* called when the initiating event is a new blocking of a thread, or a change to a blocked thread's priority */
int _MQ_SchedState_UpdateLockForBlockedThread(struct __MQ_ThreadState *thr) {
  int ret;

  int start_own_heldlocks_index = __MQ_RWLock_ThreadState.HeldLocks_Index;

  /* make sure no threads escape while we're at it */
  MQ_THREADSTATE_WALK_START();

  ret = _MQ_SchedState_UpdateLockForBlockedThread_1(thr,0);

  MQ_THREADSTATE_WALK_END();

  if (__MQ_RWLock_ThreadState.HeldLocks_Index > start_own_heldlocks_index)
    MQ_SchedState_ReleasePeak();

  return ret;
}

/* the caller has to
 * own the mutex in the lock, and the supplied
 * thread has to be blocked in some fashion.  in practice it 
 * is only called from the
 * contention handlers themselves.  iff the ss of the supplied lock is
 * derived from the supplied thr, the lock ss is recomputed from
 * current waiters, and the new pri propagated to current holder(s)
 * by _MQ_SchedState_UpdateLockHolders_1(), which will recurse through
 * blocked holders as necessary.
 */

int _MQ_SchedState_UpdateLockForUnblockedThread(struct __MQ_ThreadState *thr, MQ_RWLock_t *l) {
  if ((MQ_SCHEDSTATE_POINTER(l->SchedState) != thr) &&
      (! (l->SchedState.EnBloc & MQ_SCHEDSTATE_NEEDSRECOMPUTE_FLAG)))
    return 0;

  int ret;

  int start_own_heldlocks_index = __MQ_RWLock_ThreadState.HeldLocks_Index;

  /* make sure no threads escape while we're at it */
  MQ_THREADSTATE_WALK_START();

  union __MQ_SchedState start_lock_ss, end_lock_ss;

  if ((ret=_MQ_SchedState_UpdateLock_1(l, &end_lock_ss))<0)
    goto out;

  if (end_lock_ss.policy_and_priority == start_lock_ss.policy_and_priority)
    goto out;

  /* lock pri changed.  need to propagate that to current holder(s).
   * if there are no longer any waiters, the lock's ss is now zero and
   * will no longer contribute to any inheritance.
   */
  ret = _MQ_SchedState_UpdateLockHolders_1(l,MQ_TSP_To_TSA(thr));

 out:

  MQ_THREADSTATE_WALK_END();

  if (__MQ_RWLock_ThreadState.HeldLocks_Index > start_own_heldlocks_index)
    MQ_SchedState_ReleasePeak();

  return ret;
}


int _MQ_RWLock_CheckFlags(uint64_t CurFlags, uint64_t SetFlags, uint64_t ClearFlags, uint64_t LocalBuildCapMask) {
  MQ_SyncInt_Fence_Consume();
  if ((CurFlags & (MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) ||
      (SetFlags & MQ_RWLOCK_NOSET_FLAGS) ||
      (ClearFlags & MQ_RWLOCK_NOCLEAR_FLAGS))
    MQ_returnerrno_or_abort(EINVAL,CurFlags & MQ_RWLOCK_ABORTONMISUSE);
  else if (SetFlags & ~LocalBuildCapMask)
    MQ_returnerrno_or_abort(ENOTSUP,CurFlags & MQ_RWLOCK_ABORTONMISUSE);
  else
    return 0;
}

int _MQ_X_AddLock(MQ_RWLock_t *l, uint64_t locktype) {
  if (MQ_TSA == MQ_TSA_INITIALIZER)
    MQ_ThreadState_Init();

  if ((l)->Flags & MQ_RWLOCK_NOLOCKTRACKING)
    return 0;

  if (__MQ_RWLock_ThreadState.HeldLocks_Index == MQ_RWLOCK_MAX_TRACKED)
    MQ_returnerrno_or_abort(ENOLCK,(l)->Flags & MQ_RWLOCK_ABORTONMISUSE);

  __MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index++].ID_with_status = (uint64_t)(l)|locktype;

  if (l->Flags & (MQ_RWLOCK_INHERITPRIORITY|MQ_RWLOCK_INHERITPEAKPRIORITY)) {
    int ret;
    if ((ret=_MQ_SchedState_Inherit(l))<0) {
      --__MQ_RWLock_ThreadState.HeldLocks_Index;
      return ret;
    }
  }

  return 0;
}

int _MQ_X_DropLock(MQ_RWLock_t *l, uint64_t locktype) {
  int lock_index;

  if (l->Flags & MQ_RWLOCK_NOLOCKTRACKING)
    return 0;

  for (lock_index=__MQ_RWLock_ThreadState.HeldLocks_Index-1;lock_index>=0;--lock_index) {
    if (MQ_RWLOCK_TRACK_ID(__MQ_RWLock_ThreadState.HeldLocks[lock_index]) != (uint64_t)l)
      continue;
    if (MQ_RWLOCK_TRACK_TYPE(__MQ_RWLock_ThreadState.HeldLocks[lock_index]) == locktype)
      break;
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
  }

  if (lock_index<0)
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);

  if (lock_index == __MQ_RWLock_ThreadState.HeldLocks_Index-1) {
    for (; (lock_index > 0) && (! __MQ_RWLock_ThreadState.HeldLocks[lock_index-1].ID_with_status); --lock_index);
    __MQ_RWLock_ThreadState.HeldLocks_Index = lock_index;
  } else
    __MQ_RWLock_ThreadState.HeldLocks[lock_index].ID_with_status = 0;

  if (l->Flags & (MQ_RWLOCK_INHERITPRIORITY|MQ_RWLOCK_INHERITPEAKPRIORITY))
    (void)_MQ_SchedState_Disinherit(l);

  return 0;
}


void _MQ_RWLock_ThreadCleanup(void) {
  for (int lock_index = __MQ_RWLock_ThreadState.HeldLocks_Index-1; lock_index >= 0; --lock_index) {
    MQ_RWLock_t *l = MQ_RWLOCK_TRACK_POINTER(__MQ_RWLock_ThreadState.HeldLocks[lock_index]);
    if (! l)
      continue;
    if (l->Flags & MQ_RWLOCK_TRACK_DUMMY)
      continue;
    if (l->Flags & MQ_RWLOCK_ABORTONMISUSE)
      abort();
    (void)MQ_Unlock(l); /* xxx want to unlock in a way that doesn't relinquish elevated priority */
  }
  return;
}

#ifdef __RTM__

__wur int _MQ_MutEx_Lock_RTM(MQ_RWLock_t *l) {
  if (__MQ_RWLock_ThreadState.HeldLocks_Index && (__MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index-1].ID_with_status & MQ_RWLOCK_TRACK_RTM)) {
    __MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index++].ID_with_status = (uint64_t)l|(MQ_RWLOCK_TRACK_RTM|MQ_RWLOCK_TRACK_MUTEX); /* continue within the transaction already in progress */
    return 0;
  }
  for (int tries=MQ_RWLOCK_INTELRTM_MAX_RETRIES; tries; --tries) {
    uint32_t rtm_status;
    rtm_status = _xbegin();
    if (rtm_status == _XBEGIN_STARTED) {
      if (! l->MutEx.Count) {
	__MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index++].ID_with_status = (uint64_t)l|(MQ_RWLOCK_TRACK_RTM|MQ_RWLOCK_TRACK_MUTEX);
	return 0;
      } else
	_xabort(0);
    }
    if (rtm_status & _XABORT_RETRY)
      continue;
    if ((rtm_status & _XABORT_EXPLICIT) && (! _XABORT_CODE(rtm_status))) {
      /* borrow the estimator from the adaptive mechanism in the contention handler (but obviously can't adjust it, as that would induce contention) */
      int nspins, maxspins = l->MutEx_Spin_Estimator << 1;
      if (maxspins > MQ_RWLOCK_ADAPTIVE_MAX_SPINS)
	maxspins = MQ_RWLOCK_ADAPTIVE_MAX_SPINS;
      for (nspins=maxspins;nspins;--nspins)
	if (! l->MutEx.Count)
	  break;
      if (nspins)
	continue;
      else
	break;
    }
    break;
  }
  MQ_returnerrno(EBUSY);
}

#endif /* __RTM__ */

static __always_inline int futex(volatile int * uaddr, int op, int val, const struct timespec *timeout, int *uaddr2, int val3) {
  /* on FreeBSD, _umtx_op(uaddr,{UMTX_OP_WAIT,UMTX_OP_WAKE},val,sizeof *tm_p,struct _umtx_time *tm_p) does the same thing
   * (see http://svnweb.freebsd.org/base/head/lib/libc/gen/sem_new.c?view=markup&pathrev=232144)
   */
  return (int)syscall((uint64_t)__NR_futex,uaddr,op,val,timeout,uaddr2,val3);
}

int _MQ_RWLock_MutEx_Lock_contended(MQ_RWLock_t *l, MQ_Time_t *wait_nsecs) {

  /* make sure the caller wasn't compiled without MQ_RWLOCK_INHERITPEAKPRIORITY_SUPPORT but tried to lock a MQ_RWLOCK_INHERITPEAKPRIORITY lock
   * (won't catch uncontended locks, but if there's contention, which is when it matters, this will catch the programmer error promptly).
   */
  if ((l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY) &&
      (MQ_SCHEDSTATE_POINTER(__MQ_ThreadState.CurSchedState) != &__PeakSchedState))
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);

  if (l->MutEx.Count == MQ_RWLOCK_COUNT_INITIALIZER) {
    union MQ_LockCore start_core = { .Count = MQ_RWLOCK_COUNT_INITIALIZER, .Owner = MQ_TSA_INVALIDOWNER }, end_core = { .Count = 1, .Owner = MQ_TSA };
    if (MQ_SyncInt_ExchangeIfEq(l->MutEx.EnBloc,start_core.EnBloc,end_core.EnBloc)) {
      _MQ_RWLock_LockState_Initialize(l);
      return 0;
    }
  }

  /* the stack-like, LIFO nature of signal handling, and the
   * __threadlocal variables, mean that we don't have to use (I don't
   * think..) hardware-locked atomic instructions for _AsyncLock:
   */
  if (! __MQ_RWLock_AsyncLock[0])
    __MQ_RWLock_AsyncLock[0] = l;
  else if (__MQ_RWLock_AsyncLock[0] == l)
    MQ_returnerrno_or_abort(EDEADLK,l->Flags & MQ_RWLOCK_ABORTONEDEADLK);
  else if (! __MQ_RWLock_AsyncLock[1]) /* a signal handler was interrupted by another signal handler.  cope two levels deep, because it's easy. */
    __MQ_RWLock_AsyncLock[1] = l; /* not actually able to make use of the pointer identity of this one, but there's no harm storing it as the flag. */
  else
    MQ_returnerrno_or_abort(EDEADLK,l->Flags & MQ_RWLOCK_ABORTONEDEADLK);

  int ret;

  if (! MQ_SyncInt_PostIncrement(l->MutEx.Count,1)) {
    ret = 0;
    goto out;
  }

  /* contention confirmed, onward ho */

  int nspins;

  /* try adaptive spin locking first, but only if there's no one else in line */
  if ((l->Flags & MQ_RWLOCK_ADAPTIVE) && (l->MutEx.Count <= 2) && MQ_SyncInt_ExchangeIfEq(l->LFList_Head_Lock,0,MQ_TSA /*|0x10000000*/)) {
    /* Kaz Kylheku's algorithm for opportunistic dynamically tuned spin mutex acquisition in lieu of FUTEX_WAIT
     * (also preempts the FUTEX_WAKE in the unlocker).
     */
#if MQ_RWLOCK_ADAPTIVE_MAX_SPINS < 1
#error MQ_RWLOCK_ADAPTIVE_MAX_SPINS must be positive
#endif
    int maxspins = l->MutEx_Spin_Estimator << 1;
    if (maxspins > MQ_RWLOCK_ADAPTIVE_MAX_SPINS)
      maxspins = MQ_RWLOCK_ADAPTIVE_MAX_SPINS;
    /* the optimizer does this for us (convert countup to countdown, to make the test cheap) but be completely sure: */
    for (ret=nspins=maxspins;nspins;--nspins) {
      if (l->MutEx.Count == 1) { /* that's us! */
	l->MutEx.Owner = MQ_TSA;
	l->LFList_Head_Lock = 0;
	if (nspins<maxspins)
	  l->MutEx_Spin_Estimator += (((maxspins-nspins) - l->MutEx_Spin_Estimator)/16);
	++l->M_Adaptives;
	ret = 0;
	goto out;
      }
    }

    l->LFList_Head_Lock = 0;
  }

  /* committed to wait; no harm taking the time to do some error checking now */
  if (((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(l->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED)) {
    ret = MQ_seterrpoint((l->Flags & (MQ_RWLOCK_INITED)) ? ENOTRECOVERABLE : EINVAL);
    goto out;
  }

  if (l->Flags & MQ_RWLOCK_INACTIVE) {
    MQ_SyncInt_Decrement(l->MutEx.Count,1);
    ret = MQ_seterrpoint(ECANCELED);
    goto out;
  }

  if (l->MutEx.Owner == MQ_TSA) {
    MQ_SyncInt_Decrement(l->MutEx.Count,1);
    ret = MQ_seterrpoint_or_abort(EDEADLK,l->Flags & MQ_RWLOCK_ABORTONEDEADLK);
    goto out;
  }

  /* _MQ_RWLock_MutEx_Lock() called here because MutEx != 1, deliberately more restrictive than > 1, to make it free to call here and detect that it's haywire */
  if (MQ_SyncInt_Get(l->MutEx.Count) <= 0) {
    if (l->Flags & MQ_RWLOCK_ABORTWHENCORRUPTED)
      MQ_abort();
    MQ_SyncInt_SetFlags(l->Flags,MQ_RWLOCK_LFLIST_CORRUPTED);
    ret = MQ_seterrpoint(ENOTRECOVERABLE);
    goto out;
  }

  { /* block to just before "out" label */
  struct MQ_Lock_LFList_Ent ent = { .waiter = MQ_TSA, .waiter_threadstate = __MQ_ThreadState_Pointer, .granted = 0, ._prev = 0, ._next = 0, ._prev_tid = 0, ._my_tid = gettid(), ._next_tid = 0 }, *oldtail;

  struct MQ_Lock_LFList_Ent *prev_oldtail = MQ_SyncInt_Get(l->LFList_Tail);
  pid_t cur_LFList_Tail_Waiter = MQ_SyncInt_Exchange(l->LFList_Tail_Waiter,-gettid());
  if ((oldtail = ent._prev = MQ_SyncInt_Exchange(l->LFList_Tail,&ent))) {
    if (oldtail == prev_oldtail) {
      /* the two MQ_SyncInt_Exchange()s above might have been split,
       * leaving mismatching LFList_Tail and LFList_Tail_Waiter, in
       * which case cur_LFList_Tail_Waiter might already be gone, and
       * moreover we mustn't grab LFList_Tail_Waiter back.
       */
      if (MQ_SyncInt_ExchangeIfEq(l->LFList_Tail_Waiter,-gettid(),gettid())) {
	/* because a contending thread will have exchanged
	 * LFList_Tail_Waiter for its own -gettid() before appending
	 * itself to the LL, (oldtail == prev_oldtail) &&
	 * (l->LFList_Tail_Waiter == -gettid()) means that
	 * cur_LFList_Tail_Waiter matches oldtail, so kill-testing
	 * cur_LFList_Tail_Waiter will reliably succeed, because only
	 * we can set oldtail->_next, so oldtail cannot exit until we
	 * do that unless it is broken.
	 */
	if (l->Flags & MQ_RWLOCK_ROBUST) {
	  /* obviously this whole mechanism of checking existence of a
	   * thread, then accessing resources it maintains, implicitly
	   * races against deallocation, but this is the best we can do
	   * from userspace without moving the LFList state out of the
	   * locking threads' stacks and into shared memory, with the
	   * associated additional complexity and overhead (need that
	   * for emulating PTHREAD_PROCESS_SHARED anyway).  (see also
	   * https://www.kernel.org/doc/Documentation/robust-futexes.txt)
	   */
	  if ((cur_LFList_Tail_Waiter>0) && (kill(cur_LFList_Tail_Waiter,0)<0) && (errno == ESRCH))
	    handlecorruption();
	}
      }
    }
    ent._prev_tid = oldtail->_my_tid;
    MQ_SyncInt_Put(oldtail->_next_tid,gettid());
    MQ_SyncInt_Put(oldtail->_next,&ent); /* When there is more than one ent in the LFList, as is guaranteed by the
					  * MQ_SyncInt_Exchange() above, the extricate and _MQ_RWLock_MutEx_Unlock_contended() algorithms stall
					  * until oldtail->_next has been set, so we are guaranteed that
					  * oldtail is accessible here, though no further.
					  */
  } else {
    if (! prev_oldtail)
      MQ_SyncInt_ExchangeIfEq(l->LFList_Tail_Waiter,-gettid(),gettid());
    MQ_SyncInt_Put(l->LFList_Head_Waiter,gettid());
    MQ_SyncInt_Put(l->LFList_Head,&ent);
  }

  /* tie-in to the RW system -- mark it contended now that LFList_Tail is non-null.
   * this is only best effort to try to stop additional read lockers from sneaking
   * through at the last moment.  the caller needs to mark it contended
   * after getting the mutex, whether the mutex was gotten here or was uncontended.
   * (there are races with the tests of LFList_Tail in the unlock routines.)
   */
  MQ_SyncInt_SetFlags(MQ_RWLOCK_RW_RAWCOUNT(l),MQ_RWLOCK_COUNT_CONTENDED);

  struct timespec ts;
  MQ_Time_t start_waiting_at, failsafe_wait_nsecs;
  start_waiting_at = ent.wait_start_at = MQ_Time_Now_Monotonic_inline();

  for (nspins=0;MQ_SyncInt_Get(ent.granted) == 0;++nspins) {

    if (l->Flags & MQ_RWLOCK_LFLIST_CORRUPTED) {
      ret = MQ_seterrno(ENOTRECOVERABLE);
      goto out;
    }

    if (l->Flags & MQ_RWLOCK_INACTIVE) {
      errno = ECANCELED;
      goto extricate;
    }

    if (wait_nsecs && (*wait_nsecs <= 0)) {
      errno = ETIMEDOUT;
      goto extricate;
    }

    if (l->Flags & MQ_RWLOCK_ROBUST) {
      pid_t MutEx_Locker = MQ_TSA_To_TID(MQ_SyncInt_Get(l->MutEx.Owner));
      if ((MutEx_Locker > 0) && (kill(MutEx_Locker,0)<0) && (errno == ESRCH) && (MQ_TSA_To_TID(MQ_SyncInt_Get(l->MutEx.Owner)) == MutEx_Locker))
	handlecorruption();
    }

    if ((! wait_nsecs) || (*wait_nsecs >= 10L*MQ_ONE_SECOND) || (*wait_nsecs == MQ_FOREVER)) {
      /* wake up once every ten seconds to have a look around */
      ts.tv_sec = 10;
      ts.tv_nsec = 0;
    } else {
      ts.tv_sec = (*wait_nsecs)/MQ_ONE_SECOND;
      ts.tv_nsec = (*wait_nsecs) - ((MQ_Time_t)ts.tv_sec * MQ_ONE_SECOND);
    }

    if ((! nspins) && (l->Flags & MQ_RWLOCK_STATISTICS))
      MQ_SyncInt_Increment(l->futex_trips,1); /* since we're always re-futex-waiting every ten seconds, it doesn't make sense to count each spin */

    /*
     * need to use FUTEX_LOCK_PI here, and set ent.granted to _prev->_my_tid, or if ! _prev, to l->MutEx.Owner, unless that is negative, in which case to zero
     *    and the user space side is to set head->granted to FUTEX_WAITERS|head->_my_tid before calling FUTEX_UNLOCK_PI
     * note that FUTEX_LOCK_PI ignores the val argument.  apparently it will return immediately if *uaddr is already set to the caller's TID, otherwise block
     *    until FUTEX_UNLOCK_PI is called on it (by the locker set in *uaddr before calling, presumably, but is this enforced?).
     *
     * actually, need to separate out the granted slot from the futex uaddr slot.
     *
     * if the _prev waiter times out, it has to FUTEX_UNLOCK_PI its _next waiter after snipping itself out, so that the _next waiter can re-FUTEX_LOCK_PI on its
     *    new _prev


     * if a lock owner's priority is lower than a lock waiter's, then 
     *
     * a lock waiter will identify with FUTEX_LOCK_PI the headmost ent (or the owner) with a priority lower than itself, or the _prev.
     * when a thread unlocks, it has to check for any ents pointing at its TID, and FUTEX_UNLOCK_PI them.  they will then refutex with
     * the new head.
     *
     * what about the shared uaddr?

     * no:
     * just maintain a shared PI uaddr in the rwlock_t PI_Granted, and record with it a refcount PI_RefCount, the highest priority PI_Highest_Pri of anyone waiting on it,
     *    and the lowest priority PI_Lowest_Pri of any current waiter or lock holder
     * whenever a holder with a lower priority than PI_Highest_Pri unlocks and the shared PI has a nonzero refcount, PI_Granted always has to be FUTEX_PI_UNLOCKed
     *   whether or not 
     * whenever a waiter has a higher priority than PI_Lowest_Pri, it waits on PI_Granted
     * :on

     * a going-into-waiter does a PI_LOCK identifying the current lock holder, and increments the lockwide PI_RefCount,
     *   iff the holder has lower pri, and when the holder unlocks, it PI_UNLOCKS
     *   all the ents that reference it (possibly skipping those for waiters with pri <= the new lowest pri in the LFList).
     *   each waiter thus unlocked will either be the/a new holder, or will reenter PI_LOCK iff the new holder has a lower pri than itself.


     * key thing: FUTEX_PI_UNLOCK only wakes one of the waiters, which then has to FUTEX_PI_UNLOCK for the next one, in a cascade of syscalls.

     * thus, Shared_Futex_Granted and Shared_Futex_RefCount need to still use FUTEX_WAIT/_WAKE


     *
     * see third/glibc-2.21/nptl/pthread_mutex_unlock.c
    */

    if (futex(&ent.granted,FUTEX_WAIT,0,&ts,0,0) < 0) {
      if (MQ_SyncInt_Get(ent.granted)) {
	ret = 0;
	break;
      }
      if ((errno == EINTR) || (errno == ETIMEDOUT)) {
	stopwatch:
	if ((! wait_nsecs) || (*wait_nsecs == MQ_FOREVER))
	  continue;
	if (errno == ETIMEDOUT) {
	  *wait_nsecs -= (ts.tv_nsec + MQ_ONE_SECOND*ts.tv_sec);
	  start_waiting_at += (ts.tv_nsec + MQ_ONE_SECOND*ts.tv_sec);
	} else {
	  /* sure would be nice if futex() updated the struct timespec for us on EINTR, but noooo, we have to do it ourselves... */
	  MQ_Time_t stop_waiting_at = MQ_Time_Now_Monotonic_inline();
	  *wait_nsecs -= stop_waiting_at - start_waiting_at;
	  start_waiting_at = stop_waiting_at;
	}
	continue;
      } else {
	/* something weird is going on.  If there's no timeout, add one as a safety valve. */
	if ((! wait_nsecs) || (*wait_nsecs == MQ_FOREVER)) {
	  failsafe_wait_nsecs = MQ_RWLOCK_SPIN_DEADLOCK_THRESHOLD;
	  wait_nsecs = &failsafe_wait_nsecs;
	}
	goto stopwatch;
      }

    extricate:

      /* now we have to extricate ourselves from the list safely.  don't try to do this completely
       * lock-free!
       *
       * get the LFList_Head_Lock, to mutually exclude with _MQ_RWLock_MutEx_Unlock_contended()
       * and any other timer-outers, so that we can safely operate arbitrarily on the head and
       * middle of the LFList.
       *
       * the tricky part happens if we are at the tail, in which case we have to use some lock-free
       * logic to work around newly arrived lockers.  the coping strategy is similar to that
       * in _MQ_RWLock_MutEx_Unlock_contended().
       */
      for (int assertspins=0; ! MQ_SyncInt_ExchangeIfEq(l->LFList_Head_Lock,0,MQ_TSA /*|0x20000000*/) ;++assertspins) {
	if (MQ_SyncInt_Get(ent.granted)) {
	  ret = 0;
	  goto out;
	}

	if (l->Flags & MQ_RWLOCK_LFLIST_CORRUPTED) {
	  ret = MQ_seterrpoint(ENOTRECOVERABLE);
	  goto out;
	}

	if (assertspins > (MQ_RWLOCK_SPIN_DEADLOCK_THRESHOLD*(1000000/MQ_RWLOCK_SPIN_USLEEP_INTERVAL)))
	  handlecorruption();

	/* we're timed out, and it's almost certainly
	 * _Unlock_contended that we're contending with.  match our
	 * retry cadence to its phase 2 retry cadence.
	 */
	START_NOCANCELS;
	usleep(MQ_RWLOCK_SPIN_USLEEP_INTERVAL);
	END_NOCANCELS;
      }

      if (MQ_SyncInt_Get(ent.granted)) {
	l->LFList_Head_Lock = 0;
	ret = 0;
	break;
      }

      union __MQ_SchedState start_lock_ss = { .EnBloc = l->SchedState.EnBloc };
      if ((MQ_SyncInt_Decrement(l->MutEx.Count,1) <= 1) && (start_lock_ss.EnBloc)) {
	union __MQ_SchedState end_lock_ss;
	if (! l->Wait_List_Head)
	  end_lock_ss.EnBloc = 0UL;
	else
	  end_lock_ss.EnBloc = start_lock_ss.EnBloc | MQ_SCHEDSTATE_NEEDSRECOMPUTE_FLAG;
	MQ_SyncInt_ExchangeIfEq(l->SchedState.EnBloc,start_lock_ss.EnBloc,end_lock_ss.EnBloc);
      }

      pid_t cur_LFList_Head_Waiter = MQ_SyncInt_Exchange(l->LFList_Head_Waiter,0);
      struct MQ_Lock_LFList_Ent *orig_head = MQ_SyncInt_Exchange(l->LFList_Head,0);

      /* now atomically (set LFList_Tail to _prev if LFList_Tail is us), and only if that fails, delete ourselves the easy way.
       * note there is no race on ent._prev akin to the orig_head->_next race mitigated in _MQ_RWLock_MutEx_Unlock_contended(), because
       * ent is us, so we couldn't be here if ._prev weren't properly assigned in the first place, and it can only have been updated
       * since then by another timer-outer, with which we have mutually excluded ourselves with LFList_Head_Lock.
       */

      prev_oldtail = MQ_SyncInt_Get(l->LFList_Tail);
      pid_t orig_LFList_Tail_Waiter = MQ_SyncInt_ExchangeIfEq(l->LFList_Tail_Waiter,gettid(),-gettid());

      if ((oldtail = MQ_SyncInt_ExchangeIfEq_ReturnOldVal(l->LFList_Tail,&ent,ent._prev)) == &ent) {
	/* we were at the tail */

	if (oldtail == prev_oldtail)
	  MQ_SyncInt_ExchangeIfEq(l->LFList_Tail_Waiter,-gettid(),ent._prev_tid);

	if (ent._prev) {

	  if (l->Flags & MQ_RWLOCK_ROBUST) {
	    if ((ent._prev_tid > 0) && (kill(ent._prev_tid,0)<0) && (errno == ESRCH))
	      handlecorruption();
	  }

	  /* we may not be at the tail anymore (because the appender runs lockless), so we have to clear _prev->_next iff it still equals us. */
          if (MQ_SyncInt_ExchangeIfEq(ent._prev->_next,&ent,0)) /* it's the tail now */
	    ent._prev->_next_tid = 0;
	  l->LFList_Head = orig_head; /* restore the head, which can't be us since ent._prev is set. */
	  l->LFList_Head_Waiter = cur_LFList_Head_Waiter;
	} /* else it's an empty list now, though possibly only for an instant, hence the pre-nulling of l->LFList_Head */
      } else {
	/* we weren't at the tail */

	if (oldtail == prev_oldtail)
	  MQ_SyncInt_ExchangeIfEq(l->LFList_Tail_Waiter,-gettid(),orig_LFList_Tail_Waiter);

	/*
	 * another thread may have completed its append (MQ_SyncInt_Exchange()) before the above MQ_SyncInt_ExchangeIfEq(),
	 * but not yet assigned ent._prev to its return value.  mitigate that race.
	 */
	spinassert(ent._next && ent._next->_prev);

	if (l->Flags & MQ_RWLOCK_ROBUST) {
	  if ((ent._next_tid > 0) && (kill(ent._next_tid,0)<0) && (errno == ESRCH))
	    handlecorruption();
	}

	if (! ent._prev /* orig_head == &ent */) {
	  /* but we were at the head */
	  ent._next->_prev_tid = 0;
	  ent._next->_prev = 0;
	  l->LFList_Head = ent._next;
	  l->LFList_Head_Waiter = ent._next_tid;
	} else {
	  ent._next->_prev_tid = ent._prev_tid;
	  ent._next->_prev = ent._prev;

	  if (l->Flags & MQ_RWLOCK_ROBUST) {
	    if ((ent._prev_tid > 0) && (kill(ent._prev_tid,0)<0) && (errno == ESRCH))
	      handlecorruption();
	  }

	  ent._prev->_next = ent._next;
	  ent._prev->_next_tid = ent._next_tid;
	  if (orig_head) {
	    l->LFList_Head = orig_head;
	    l->LFList_Head_Waiter = cur_LFList_Head_Waiter;
	  }
	}
      }

      l->LFList_Head_Lock = 0;

      if (! errno) /* be perfectly sure non-zero is returned */
	ret = MQ_seterrpoint(EINVAL);
      else
	ret = MQ_seterrpoint(errno);
      goto out;
    } else {
      /* futex returned zero (success) -- but still have to check ent.granted */
      if (wait_nsecs && (*wait_nsecs != MQ_FOREVER)) {
	MQ_Time_t stop_waiting_at = MQ_Time_Now_Monotonic_inline();
	*wait_nsecs -= stop_waiting_at - start_waiting_at;
	start_waiting_at = stop_waiting_at;
      }
    }
  }

  ret = 0;

  if (nspins && (l->Flags & MQ_RWLOCK_STATISTICS)) {
    ++l->M_Waits;
    l->M_Wait_Time_Cum += (double)(MQ_Time_Now_Monotonic_inline() - ent.wait_start_at);
  }
  }

 out:

  if (! ret)
    l->MutEx.Owner = MQ_TSA; /* async signal safety depends on this happening before clearing __MQ_RWLock_AsyncLock */

  if (__MQ_RWLock_AsyncLock[1])
    __MQ_RWLock_AsyncLock[1] = 0;
  else
    __MQ_RWLock_AsyncLock[0] = 0;

  return ret;
}

int _MQ_RWLock_MutEx_Unlock_contended(MQ_RWLock_t *l) {
  /* because timeouts can drop MutEx to zero asynchronously, more than one thread can be here at a time, hence LFList_Head_Lock.
  * Do what we can do unlocked, and if we're adaptive and lucky we'll find that the next locker is already established by the time
  * we're ready to get the LFList_Head_Lock.  Then we're done with just the one MQ_SyncInt_Get.
  */
  struct MQ_Lock_LFList_Ent *orig_head;
  int nspins, ret;

  MQ_TSA_t MutEx_Locker_start = MQ_TSA_MkInvalid(MQ_TSA); /* invalidated it right before calling _MQ_RWLock_MutEx_Unlock_contended() */

  /* check for and exploit an adaptive spin in progress (if we don't catch it here, pre-LFList_Head_Lock, we'll catch after getting the lock) */
  if (l->LFList_Head_Lock && (l->MutEx.Count == 1)) {
    int maxspins = l->MutEx_Spin_Estimator << 1;
    if (maxspins > MQ_RWLOCK_ADAPTIVE_MAX_SPINS)
      maxspins = MQ_RWLOCK_ADAPTIVE_MAX_SPINS;
    for (ret=nspins=maxspins;nspins;--nspins) {
      if (l->MutEx.Owner != MutEx_Locker_start)
	return 0;
    }
  }

  if (! MQ_RWLOCK_COUNT_VALID_P(l->MutEx.Count)) {
    if (l->Flags & MQ_RWLOCK_ABORTWHENCORRUPTED)
      MQ_abort();
    MQ_SyncInt_SetFlags(l->Flags,MQ_RWLOCK_LFLIST_CORRUPTED);
  }

  if (((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(l->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno((l->Flags & (MQ_RWLOCK_INITED)) ? ENOTRECOVERABLE : EINVAL);

  if (__fastpath(! __MQ_RWLock_AsyncLock[0]))
    __MQ_RWLock_AsyncLock[0] = l;
  else if (__MQ_RWLock_AsyncLock[0] == l)
    MQ_returnerrno_or_abort(EDEADLK,l->Flags & MQ_RWLOCK_ABORTONEDEADLK);
  else if (! __MQ_RWLock_AsyncLock[1]) /* a signal handler was interrupted by another signal handler.  cope two levels deep, because it's easy. */
    __MQ_RWLock_AsyncLock[1] = l;
  else
    MQ_returnerrno_or_abort(EDEADLK,l->Flags & MQ_RWLOCK_ABORTONEDEADLK);

  MQ_Time_t started_spinning_at = 0;

  for (nspins=0;;++nspins) {
    /* if adaptive spin locking is in play, the lock may have already
     * been taken just since the decrement right before entry to
     * _Unlock_contended().
     *
     * also, one or more waiters may have timed out and called
     * _Unlock_contended() on their own initiative while we were
     * spinning.  If so, the lock has moved on and we need to bail.
     */
    int got_head_lock = 0;
    spinassert((! l->MutEx.Count) || (l->MutEx.Owner != MutEx_Locker_start) || (got_head_lock = MQ_SyncInt_ExchangeIfEq(l->LFList_Head_Lock,0,MQ_TSA /*|0x40000000*/)));
    if (! got_head_lock) {
      ret = 0;
      goto out;
    }

    /* mitigate race */
    if (! MQ_SyncInt_Get(l->LFList_Head)) {
      l->LFList_Head_Lock = 0;

      /* if the LFList is properly empty now because a waiter timed
       * out after _Unlock_contended was called and before it locked
       * LFList_Head_Lock, we need to bail or we'll likely deadlock.
       *
       * intrinsically, if MutEx_Locker changes while we twiddle our
       * thumbs here, that means we were called spuriously.
       */
      if (! l->MutEx.Count) {
	ret = 0;
	goto out;
      }
      if (l->Flags & MQ_RWLOCK_LFLIST_CORRUPTED) {
	ret = MQ_seterrpoint(ENOTRECOVERABLE);
	goto out;
      }
      if (! started_spinning_at)
	started_spinning_at = MQ_Time_Now();
      else if (MQ_Time_Now() - started_spinning_at > MQ_ONE_SECOND*MQ_RWLOCK_SPIN_DEADLOCK_THRESHOLD)
	handlecorruption();

      if (nspins<MQ_RWLOCK_SPIN_YIELD_LIMIT)
	sched_yield(); /* usually we're racing assignment of the head immediately after the MS_SyncInt_Exchange that opens _Lock_contended(). */
      else {
	START_NOCANCELS;
	usleep(MQ_RWLOCK_SPIN_USLEEP_INTERVAL); /* but on occasion we're contending with _Lock_contended() timeouts, which won't get anywhere without a looser grip from
						 * this side, eventually deadlocking (_LFLIST_CORRUPTED).
						 */
	END_NOCANCELS;
      }
    } else
      break;
  }

  if (MQ_SyncInt_Get(l->MutEx.Owner) != MutEx_Locker_start) {
    l->LFList_Head_Lock = 0;
    ret = 0;
    goto out;
  }

  {
    pid_t cur_LFList_Head_Waiter = MQ_SyncInt_Exchange(l->LFList_Head_Waiter,0);
    orig_head = MQ_SyncInt_Exchange(l->LFList_Head,0); /* the moment LFList_Tail is null, _MQ_RWLock_MutEx_Lock() can set the head, so we can't null the
							* head in the success clause of the MQ_SyncInt_ExchangeIfEq, but we can pre-null it, and that's
							* just as good and is race-free (with the above mitigation).
							*/

    if (l->Flags & MQ_RWLOCK_ROBUST) {
      if ((cur_LFList_Head_Waiter>0) && orig_head && (kill(cur_LFList_Head_Waiter,0)<0) && (errno == ESRCH) &&
	  (cur_LFList_Head_Waiter == MQ_SyncInt_Get(l->LFList_Head_Waiter)))
	handlecorruption();
    }

    /* now atomically (null LFList_Tail if LFList_Tail is the head), and only if that fails, set head to head->_next */
    {
      struct MQ_Lock_LFList_Ent *oldtail, *prev_oldtail = MQ_SyncInt_Get(l->LFList_Tail);
      pid_t orig_LFList_Tail_Waiter = MQ_SyncInt_Exchange(l->LFList_Tail_Waiter,-gettid());
      if ((oldtail = MQ_SyncInt_ExchangeIfEq_ReturnOldVal(l->LFList_Tail,orig_head,0)) != orig_head) {
	if (oldtail == prev_oldtail)
	  MQ_SyncInt_ExchangeIfEq(l->LFList_Tail_Waiter,-gettid(),orig_LFList_Tail_Waiter);

	spinassert(MQ_SyncInt_Get(orig_head->_next));
	MQ_SyncInt_Put(l->LFList_Head_Waiter,MQ_SyncInt_Get(orig_head->_next_tid));
	MQ_SyncInt_Put(l->LFList_Head,orig_head->_next);

	spinassert(l->LFList_Head->_prev);
	l->LFList_Head->_prev_tid = 0;
	l->LFList_Head->_prev = 0;
      }

      {
	volatile int *futexp = &orig_head->granted;
	if (MQ_TSA_ValidP(MQ_SyncInt_Get(l->MutEx.Owner)))
	  handlecorruption();
	MQ_SyncInt_Put(orig_head->granted,1);
	(void)futex(futexp,FUTEX_WAKE,1,0,0,0); /* it is possible, though unlikely, that the FUTEX_WAITer never waited, because granted was set
						 * before FUTEX_WAIT, so that this is effectively sending a FUTEX_WAKE to an address that is
						 * either unmapped, unwaited, or waited by something else.  the first two are harmless, and the
						 * last is harmless as long as it's something in MQ_RWLock that's FUTEX_WAITing, because in
						 * MQ_RWLock, the value of the "granted" variable controls flow, and the dynamics of the value
						 * are well-behaved.
						 */
      }
    }
  }

  l->LFList_Head_Lock = 0;

  ret = 0;

 out:

  if (__MQ_RWLock_AsyncLock[1])
    __MQ_RWLock_AsyncLock[1] = 0;
  else
    __MQ_RWLock_AsyncLock[0] = 0;

  return ret;
}


/* the MQ RW lock and cond-signal facilities, layered on the foregoing mutex facility */

#ifdef __RTM__

__wur int _MQ_W_Lock_RTM(MQ_RWLock_t *l) {
  if (__MQ_RWLock_ThreadState.HeldLocks_Index && (__MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index-1].ID_with_status & MQ_RWLOCK_TRACK_RTM)) {
    __MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index++].ID_with_status = (uint64_t)l|(MQ_RWLOCK_TRACK_RTM|MQ_RWLOCK_TRACK_WRITE); /* continue within the transaction already in progress */
    return 0;
  }
  for (int tries=MQ_RWLOCK_INTELRTM_MAX_RETRIES; tries; --tries) {
    uint32_t rtm_status;
    rtm_status = _xbegin();
    if (rtm_status == _XBEGIN_STARTED) {
      if (! MQ_RWLOCK_RW_RAWCOUNT(l)) {
	__MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index++].ID_with_status = (uint64_t)l|(MQ_RWLOCK_TRACK_RTM|MQ_RWLOCK_TRACK_WRITE);
	return 0;
      } else
	_xabort(0);
    }
    if (rtm_status & _XABORT_RETRY)
      continue;
    if ((rtm_status & _XABORT_EXPLICIT) && (! _XABORT_CODE(rtm_status))) {
      /* borrow the estimator from the adaptive mechanism in the contention handler (but obviously can't adjust it, as that would induce contention) */
      int nspins, maxspins = l->MutEx_Spin_Estimator << 1;
      if (maxspins > MQ_RWLOCK_ADAPTIVE_MAX_SPINS)
	maxspins = MQ_RWLOCK_ADAPTIVE_MAX_SPINS;
      for (nspins=maxspins;nspins;--nspins)
	if (! MQ_RWLOCK_RW_RAWCOUNT(l))
	  break;
      if (nspins)
	continue;
      else
	break;
    }
    break;
  }
  MQ_returnerrno(EBUSY);
}

extern __wur int _MQ_R_Lock_RTM(MQ_RWLock_t *l) {
  if (__MQ_RWLock_ThreadState.HeldLocks_Index && (__MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index-1].ID_with_status & MQ_RWLOCK_TRACK_RTM)) {
    __MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index++].ID_with_status = (uint64_t)l|(MQ_RWLOCK_TRACK_RTM|MQ_RWLOCK_TRACK_READ); /* continue within the transaction already in progress */
    return 0;
  }
  for (int tries=MQ_RWLOCK_INTELRTM_MAX_RETRIES; tries; --tries) {
    uint32_t rtm_status;
    rtm_status = _xbegin();
    if (rtm_status == _XBEGIN_STARTED) {
      if (! MQ_RWLOCK_RW_RAWCOUNT(l)) {
	__MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index++].ID_with_status = (uint64_t)l|(MQ_RWLOCK_TRACK_RTM|MQ_RWLOCK_TRACK_READ);
	return 0;
      } else
	_xabort(0);
    }
    if (rtm_status & _XABORT_RETRY)
      continue;
    if ((rtm_status & _XABORT_EXPLICIT) && (! _XABORT_CODE(rtm_status))) {
      /* borrow the estimator from the adaptive mechanism in the contention handler (but obviously can't adjust it, as that would induce contention) */
      int nspins, maxspins = l->MutEx_Spin_Estimator << 1;
      if (maxspins > MQ_RWLOCK_ADAPTIVE_MAX_SPINS)
	maxspins = MQ_RWLOCK_ADAPTIVE_MAX_SPINS;
      for (nspins=maxspins;nspins;--nspins)
	if (! MQ_RWLOCK_RW_RAWCOUNT(l))
	  break;
      if (nspins)
	continue;
      else
	break;
    }
    break;
  }
  MQ_returnerrno(EBUSY);
}

#endif /* __RTM__ */

static int _MQ_RWLock_Signal_1(MQ_RWLock_t *l);

/* have to enter holding List_Lock */
static __always_inline int _MQ_RWLock_Signal(MQ_RWLock_t *l) {
  int ret;
  if (__fastpath(! l->Wait_List_Head))
    return _MQ_RWLock_MutEx_Unlock(l);
  ret = _MQ_RWLock_Signal_1(l);
  int ret2;
  if ((ret2=_MQ_RWLock_MutEx_Unlock(l))<0)
    return ret2;
  return ret;
}

static __wur int _MQ_RWLock_Wait_1(MQ_RWLock_t *l, MQ_Wait_Type what, MQ_Time_t wait_nsecs, void **arg, int64_t LFList_ID, const char *file, const int line, const char *func);

__wur static __always_inline int _MQ_RWLock_Wait(MQ_RWLock_t *l, MQ_Wait_Type what, MQ_Time_t wait_nsecs, void **arg, int64_t LFList_ID, const char *file, const int line, const char *func) {
  if (__fastpath(wait_nsecs <= 0)) {
    _MQ_RWLock_MutEx_Unlock(l);
    MQ_returnerrno(EBUSY);
  }

  return _MQ_RWLock_Wait_1(l, what, wait_nsecs, arg, LFList_ID, file, line, func);
}

/* note that the _UP and _DOWN macros can only be safely used while holding the mutex, and when MQ_RWLOCK_COUNT_CONTENDED is set, disabling atomic operations on RW.Count */
#define MQ_RWLOCK_R_UP(l) (((l)->RW.Count&MQ_RWLOCK_COUNT_READ) ? ++(l)->RW.Count : (((l)->RW.Count) |= (MQ_RWLOCK_COUNT_READ|1)))
#define MQ_RWLOCK_R_DOWN(l) ((--(l)->RW.Count & (MQ_RWLOCK_COUNT_MINFLAG-1)) ? (l)->RW.Count : ((l)->RW.Count &= ~MQ_RWLOCK_COUNT_READ))
/* MQ_RWLOCK_W_UP() is used inline so appears in mq_rwlock.c */
#define MQ_RWLOCK_W_DOWN(l) (--(l)->RW.Count)
#define MQ_RWLOCK_RW_R2W(l) ((l)->RW.Count &= ~MQ_RWLOCK_COUNT_READ)
#define MQ_RWLOCK_RW_W2R(l) ((l)->RW.Count |= MQ_RWLOCK_COUNT_READ)

static __wur inline int MQ_Wait_Type_OK_p(MQ_Wait_Type what) {
  if ((what < MQ_WAIT_TYPE_MIN) || (what > MQ_WAIT_TYPE_MAX) || (__popcount(what) != 1))
    return 0;
  else
    return 1;
}

__wur int _MQ_W_Lock_contended(MQ_RWLock_t *l, MQ_Time_t wait_nsecs, const char *file, const int line, const char *func) {
  if (((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(l->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno((l->Flags & (MQ_RWLOCK_INITED)) ? ENOTRECOVERABLE : EINVAL);

  if (MQ_RWLOCK_RW_LOCKED_P(l) && MQ_R_HaveLock_p(l))
    MQ_returnerrno_or_abort(EDEADLK,l->Flags & MQ_RWLOCK_ABORTONEDEADLK);

  int ret;
  int got_peak_pri = 0;

  if (l->Flags & MQ_RWLOCK_INHERITPRIORITY) {
    if (! MQ_SchedState_AdoptPeak())
      got_peak_pri = 1;
  }

  int64_t LFList_ID = _MQ_RWLock_MutEx_Lock(l,&wait_nsecs);

  if (LFList_ID < 0) {
    ret = (int)LFList_ID;
    goto out;
  }

  MQ_SyncInt_SetFlags(MQ_RWLOCK_RW_RAWCOUNT(l),MQ_RWLOCK_COUNT_CONTENDED);

  if (MQ_RWLOCK_RW_LOCKED_P(l))
    ret = _MQ_RWLock_Wait(l, MQ_LOCK_WAIT_W, wait_nsecs, 0, LFList_ID, file, line, func);
  else {
    MQ_RWLOCK_W_UP(l);
    l->RW.Owner = MQ_TSA;
    ret = _MQ_RWLock_MutEx_Unlock(l);
  }

 out:

  /* if the lock failed, zero out the record of it now, to remove it from the set contributing to the thread's pri */
  if (ret && (! (l->Flags & MQ_RWLOCK_NOLOCKTRACKING)))
    __MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index-1].ID_with_status = 0UL;

  if (got_peak_pri)
    (void)MQ_SchedState_ReleasePeak(); /* this will pick up the priority of the lock, if any */
  /* else we were at peak pri on entry and will inherit this lock's pri (if it's the max held) when/if peak is relinquished */

  return ret;
}

__wur int _MQ_W2R_Lock_contended(MQ_RWLock_t *l) {
  if (((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(l->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno((l->Flags & (MQ_RWLOCK_INITED)) ? ENOTRECOVERABLE : EINVAL);

  int64_t ret;
  if ((ret=_MQ_RWLock_MutEx_Lock(l,0))<0)
    return (int)ret;

  /* note, not setting RW.Count _CONTENDED here, because we're not doing anything here that needs it.
   * specifically, the held W lock excludes all until we (implicitly atomically) change it to a held R lock,
   * and we don't do anything after that with RW.Count.
   */
  MQ_SyncInt_SetFlags(MQ_RWLOCK_RW_RAWCOUNT(l),MQ_RWLOCK_COUNT_CONTENDED);

  if (l->Flags & MQ_RWLOCK_RECURSION) {
    if (MQ_RWLOCK_RW_COUNT(l) != 1) {
      if ((ret=_MQ_RWLock_MutEx_Unlock(l))<0)
	return (int)ret;
      MQ_returnerrno_or_abort(EDEADLK,l->Flags & MQ_RWLOCK_ABORTONEDEADLK);
    }
  }

  if (l->Flags & MQ_RWLOCK_STATISTICS)
    MQ_SyncInt_Increment(l->R_Reqs,1);

  MQ_TSA_Invalidate(l->RW.Owner); /* leave a mark */
  MQ_RWLOCK_RW_W2R(l);
  (void)_MQ_RWLock_Signal(l);

  return 0;
}

__wur int _MQ_R2W_Lock_contended(MQ_RWLock_t *l, MQ_Time_t wait_nsecs, const char *file, const int line, const char *func) {
  if (((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(l->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno((l->Flags & (MQ_RWLOCK_INITED)) ? ENOTRECOVERABLE : EINVAL);

  int ret;

  int64_t LFList_ID = _MQ_RWLock_MutEx_Lock(l,&wait_nsecs);

  if (LFList_ID<0)
    return (int)LFList_ID;

  MQ_SyncInt_SetFlags(MQ_RWLOCK_RW_RAWCOUNT(l),MQ_RWLOCK_COUNT_CONTENDED);

  if (MQ_RWLOCK_RW_COUNT(l) > 1)
    ret = _MQ_RWLock_Wait(l, MQ_LOCK_WAIT_R2W, wait_nsecs, 0, LFList_ID, file, line, func);
  else {
    MQ_RWLOCK_RW_R2W(l);
    l->RW.Owner = MQ_TSA;
    ret = _MQ_RWLock_MutEx_Unlock(l);
  }

  return ret;
}

int _MQ_W_Unlock_contended(MQ_RWLock_t *l) {
  if (((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(l->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno((l->Flags & (MQ_RWLOCK_INITED)) ? ENOTRECOVERABLE : EINVAL);

  int64_t ret;

  int got_peak_pri = 0;

  if (l->Flags & MQ_RWLOCK_INHERITPRIORITY) {
    if (! MQ_SchedState_AdoptPeak())
      got_peak_pri = 1;
  }

  if ((ret=_MQ_RWLock_MutEx_Lock(l,0))<0)
    goto out;

  MQ_SyncInt_SetFlags(MQ_RWLOCK_RW_RAWCOUNT(l),MQ_RWLOCK_COUNT_CONTENDED);

  if (! l->Wait_List_Head) {
    if (MQ_RWLOCK_RW_RAWCOUNT(l) == (MQ_RWLOCK_COUNT_CONTENDED|1UL)) {
      MQ_TSA_Invalidate(l->RW.Owner); /* leave a mark */
      if (MQ_SyncInt_Get(l->LFList_Tail))
	--MQ_RWLOCK_RW_RAWCOUNT(l);
      else {
	MQ_SyncInt_Put(l->SchedState.EnBloc,0UL);
	MQ_RWLOCK_RW_RAWCOUNT(l) = 0UL;
      }
    } else if (MQ_SyncInt_Get(l->LFList_Tail))
      --MQ_RWLOCK_RW_RAWCOUNT(l);
    else { /* recursive, and nonzero, but no longer contended */
      MQ_SyncInt_Put(l->SchedState.EnBloc,0UL);
      MQ_RWLOCK_RW_RAWCOUNT(l) = (MQ_RWLOCK_RW_RAWCOUNT(l) & ~MQ_RWLOCK_COUNT_CONTENDED) - 1U;
    }
    ret = _MQ_RWLock_MutEx_Unlock(l);
    goto out;
  }

  MQ_RWLOCK_W_DOWN(l);
  if (! MQ_RWLOCK_RW_LOCKED_P(l)) { /* when recursive, this may not have gone to zero. */
    MQ_TSA_Invalidate(l->RW.Owner); /* leave a mark */
    ret = _MQ_RWLock_Signal(l);
  } else
    ret = _MQ_RWLock_MutEx_Unlock(l);

 out:

  if (got_peak_pri)
    (void)MQ_SchedState_ReleasePeak(); /* unless this is a recursive unlock, this will drop the priority of the lock, if it's currently the thread's max */
  /* else we were at peak pri on entry and will drop this lock's pri (if it was the max held) when/if peak is relinquished */

  return (int)ret;
}

__wur int _MQ_R_Lock_contended(MQ_RWLock_t *l, MQ_Time_t wait_nsecs, const char *file, const int line, const char *func) {
  if (((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(l->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno((l->Flags & (MQ_RWLOCK_INITED)) ? ENOTRECOVERABLE : EINVAL);

  int64_t ret;

  if (l->RW.Owner == MQ_TSA)
    MQ_returnerrno_or_abort(EDEADLK,l->Flags & MQ_RWLOCK_ABORTONEDEADLK);

  int got_peak_pri = 0;

  if (l->Flags & MQ_RWLOCK_INHERITPRIORITY) {
    if (! MQ_SchedState_AdoptPeak())
      got_peak_pri = 1;
  }

  if ((ret = _MQ_RWLock_MutEx_Lock(l,&wait_nsecs))<0)
    goto out;

  MQ_SyncInt_SetFlags(MQ_RWLOCK_RW_RAWCOUNT(l),MQ_RWLOCK_COUNT_CONTENDED);

  if (MQ_RWLOCK_W_NOTLOCKED_P(l) &&
      ((l->Wait_List_Head == 0) || /* have to wait if there's an R2W pending */
       (l->Wait_List_Head->what & (MQ_COND_WAIT_R|MQ_COND_WAIT_R_RETUNLOCKED|MQ_COND_WAIT_W|MQ_COND_WAIT_W_RETUNLOCKED|MQ_COND_WAIT_MUTEX|MQ_COND_WAIT_MUTEX_RETUNLOCKED)) ||
       MQ_R_HaveLock_p(l))) { /* if we already have R on this lock, allow R recursion without waiting */
    MQ_RWLOCK_R_UP(l);
    ret = MQ_RWLOCK_R_COUNT(l);
    int ret2;
    if ((ret2=_MQ_RWLock_MutEx_Unlock(l))<0)
      ret = ret2;
  } else {
    ret = _MQ_RWLock_Wait(l, MQ_LOCK_WAIT_R, wait_nsecs, 0, ret, file, line, func);
    if (ret == 0)
      ret = (int64_t)MQ_RWLOCK_R_COUNT(l); /* subject to races */
  }

 out:

  /* if the lock failed, zero out the record of it now, to remove it from the set contributing to the thread's pri */
  if ((ret<0) && (! (l->Flags & MQ_RWLOCK_NOLOCKTRACKING)))
    __MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index-1].ID_with_status = 0UL;

  if (got_peak_pri)
    (void)MQ_SchedState_ReleasePeak(); /* this will pick up the priority of the lock, if any */
  /* else we were at peak pri on entry and will inherit this lock's pri (if it's the max held) when/if peak is relinquished */

  return (int)ret;
}

int _MQ_R_Unlock_contended(MQ_RWLock_t *l) {
  if (((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(l->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno((l->Flags & (MQ_RWLOCK_INITED)) ? ENOTRECOVERABLE : EINVAL);

  int64_t ret;
  int got_peak_pri = 0;

  if (l->Flags & MQ_RWLOCK_INHERITPRIORITY) {
    if (! MQ_SchedState_AdoptPeak())
      got_peak_pri = 1;
    MQ_SyncInt_Fence_Release(); /* make sure it's not possible for an update to HeldLocks to become visible
				 *	to another thread some time after we get the mutex, for coordination with
				 * _MQ_SchedState_UpdateLockHolders_1()
				 */
  }

  if ((ret=_MQ_RWLock_MutEx_Lock(l,0))<0)
    goto out;

  if (! MQ_RWLOCK_R_COUNT(l)) {
    _MQ_RWLock_MutEx_Unlock(l);
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
  }

  MQ_SyncInt_SetFlags(MQ_RWLOCK_RW_RAWCOUNT(l),MQ_RWLOCK_COUNT_CONTENDED);

  MQ_RWLOCK_R_DOWN(l);

  if (MQ_RWLOCK_RW_COUNT(l) <= 1) { /* 1, not 0, because there may be an R2W waiting */
    if (l->Wait_List_Head) {
      ret = _MQ_RWLock_Signal_1(l);
      int ret2 = _MQ_RWLock_MutEx_Unlock(l);
      if (ret2)
	ret = ret2;
    } else {
      if (! MQ_SyncInt_Get(l->LFList_Tail)) {
	union __MQ_SchedState start_lock_ss = { .EnBloc = l->SchedState.EnBloc };
	MQ_SyncInt_Put(l->SchedState.EnBloc,0UL);
	if (start_lock_ss.EnBloc)
	  _MQ_SchedState_UpdateLockHolders(l, MQ_TSA);
	MQ_RWLOCK_RW_RAWCOUNT(l) &= ~MQ_RWLOCK_COUNT_CONTENDED;
      }
      ret = _MQ_RWLock_MutEx_Unlock(l);
    }
  } else {
    if ((! l->Wait_List_Head) && (! MQ_SyncInt_Get(l->LFList_Tail))) {
      union __MQ_SchedState start_lock_ss = { .EnBloc = l->SchedState.EnBloc };
      MQ_SyncInt_Put(l->SchedState.EnBloc,0UL);
      if (start_lock_ss.EnBloc)
	_MQ_SchedState_UpdateLockHolders(l, MQ_TSA);
      MQ_RWLOCK_RW_RAWCOUNT(l) &= ~MQ_RWLOCK_COUNT_CONTENDED;
    }
    ret = _MQ_RWLock_MutEx_Unlock(l);
  }

 out:

  if (got_peak_pri)
    (void)MQ_SchedState_ReleasePeak(); /* unless this is a recursive unlock, this will drop the priority of the lock, if it's currently the thread's max */
  /* else we were at peak pri on entry and will drop this lock's pri (if it was the max held) when/if peak is relinquished */

  if (ret > 0)
    return 0;
  else
    return (int)ret;
}

/* xxx these two, or their callers, probably need logic for getting peak pri when MQ_RWLOCK_INHERITPRIORITY is set on a lock used for conding */
static __wur int64_t _MQ_RWLock_MutEx_Lock_tracking(MQ_RWLock_t *l, MQ_Time_t *wait_nsecs) {
  if (! (l->Flags & MQ_RWLOCK_NOLOCKTRACKING))
    __MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index++].ID_with_status = (uint64_t)l|MQ_RWLOCK_TRACK_MUTEX;

  int64_t ret;

  if ((l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY) && (l->MutEx.Owner != MQ_TSA)) {
    if ((ret=_MQ_SchedState_Inherit(l))<0) {
      --__MQ_RWLock_ThreadState.HeldLocks_Index;
      return ret;
    }
  }

  ret = _MQ_RWLock_MutEx_Lock(l, wait_nsecs);

  if (ret<0) {
    if (! (l->Flags & MQ_RWLOCK_NOLOCKTRACKING))
      --__MQ_RWLock_ThreadState.HeldLocks_Index;

    if (l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY)
      (void)_MQ_SchedState_Disinherit(l);
  }

  return ret;
}

static int _MQ_RWLock_MutEx_Unlock_tracking(MQ_RWLock_t *l) {
  int lastlock_index;
  if (! (l->Flags & MQ_RWLOCK_NOLOCKTRACKING)) {
    lastlock_index = __MQ_RWLock_ThreadState.HeldLocks_Index-1;
    int lock_index;
    for (lock_index=lastlock_index;lock_index>=0;--lock_index) {
      if (MQ_RWLOCK_TRACK_ID(__MQ_RWLock_ThreadState.HeldLocks[lock_index]) != (uint64_t)l)
	continue;
      if (MQ_RWLOCK_TRACK_TYPE(__MQ_RWLock_ThreadState.HeldLocks[lock_index]) == MQ_RWLOCK_TRACK_MUTEX)
	break;
      MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
    }
    if (lock_index<0)
      MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
    if (lock_index == lastlock_index) {
      for (; (lock_index > 0) && (! __MQ_RWLock_ThreadState.HeldLocks[lock_index-1].ID_with_status); --lock_index);
      __MQ_RWLock_ThreadState.HeldLocks_Index = lock_index;
    } else
      __MQ_RWLock_ThreadState.HeldLocks[lock_index].ID_with_status = 0;
  }

  int ret = _MQ_RWLock_MutEx_Unlock(l);

  if ((l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY) && (l->MutEx.Owner != MQ_TSA)) {
    int ret2;
    if ((ret2=_MQ_SchedState_Disinherit(l))<0)
      return ret2;
  }

  return ret;
}


/* caution, this is not a very clever macro, and will break if e.g. x is "l->Wait_List_Head" */
#define MQ_RWLock_snip_from_Wait_List(l,x)	\
	MQ_SyncInt_SetFlags((l)->Flags,MQ_RWLOCK_WAITLIST_DIRTY); \
	if ((x)->_prev)			\
	  (x)->_prev->_next = (x)->_next; \
	else				\
	  (l)->Wait_List_Head = (x)->_next; \
	if ((x)->_next)			\
	  (x)->_next->_prev = (x)->_prev; \
	else				\
	  (l)->Wait_List_Tail = (x)->_prev; \
	--(l)->Wait_List_Length;	\
	(x)->_next = (x)->_prev = 0;	\
	MQ_SyncInt_ClearFlags((l)->Flags,MQ_RWLOCK_WAITLIST_DIRTY)

static int _MQ_RWLock_Signal_1(MQ_RWLock_t *l) {
  int n_woken = 0;

  if ((l->Flags & MQ_RWLOCK_WAITLIST_DIRTY) && (MQ_SyncInt_Get(l->Flags) & MQ_RWLOCK_WAITLIST_DIRTY)) {
    if (l->Flags & MQ_RWLOCK_ABORTWHENCORRUPTED)
      MQ_abort();
    MQ_SyncInt_SetFlags(l->Flags,MQ_RWLOCK_WAITLIST_CORRUPTED);
  }
  if (l->Flags & MQ_RWLOCK_WAITLIST_CORRUPTED)
    MQ_returnerrno(ENOTRECOVERABLE);

  if (MQ_RWLOCK_W_LOCKED_P(l))
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);

  struct MQ_Lock_Wait_Ent *i = (struct MQ_Lock_Wait_Ent *)l->Wait_List_Head;

  MQ_Time_t now;
  if (l->Flags & MQ_RWLOCK_STATISTICS)
    now = MQ_Time_Now_Monotonic_inline();

  while (i->what & (MQ_COND_WAIT_R|MQ_COND_WAIT_R_RETUNLOCKED|MQ_COND_WAIT_W|MQ_COND_WAIT_W_RETUNLOCKED|MQ_COND_WAIT_MUTEX|MQ_COND_WAIT_MUTEX_RETUNLOCKED)) {
    i = i->_next;
    if (! i)
      return 0;
  }

  union __MQ_SchedState max_w_ss = { .EnBloc = 0 };

  if (l->Flags & MQ_RWLOCK_PRIORITYORDER) {
    /* tricky logic: R2W always goes before R or W, but the first R2W with highest pri goes.
     * R and W contend with each other, with the first of either one of them at the highest pri going.
     * If it's an R, then all R's with pri greater than the highest write waiter pri,
     * or with pri equal to the highest write waiter pri and appearing before the first such write waiter, go too.
     */
    if (i->what & (MQ_LOCK_WAIT_R|MQ_LOCK_WAIT_W)) {
      struct MQ_Lock_Wait_Ent *j;
      for (j=i; j && (j->what & (MQ_LOCK_WAIT_R|MQ_LOCK_WAIT_W)); j=j->_next) {
	if ((j->what == MQ_LOCK_WAIT_W) && (MQ_SCHEDSTATE_CMP(j->waiter_threadstate->CurSchedState,max_w_ss)<0)) {
	  max_w_ss.EnBloc = j->waiter_threadstate->CurSchedState.EnBloc;
	  i = j;
	} else if (MQ_SCHEDSTATE_CMP(j->waiter_threadstate->CurSchedState,i->waiter_threadstate->CurSchedState)<0)
	  i = j;
      }
    } else if (i->what == MQ_LOCK_WAIT_R2W) {
      struct MQ_Lock_Wait_Ent *j;
      for (j=i; j && (j->what == MQ_LOCK_WAIT_R2W); j=j->_next)
	if (MQ_SCHEDSTATE_CMP(j->waiter_threadstate->CurSchedState,i->waiter_threadstate->CurSchedState)<0)
	  i = j;
    }
  }

  switch(i->what) {
  case MQ_LOCK_WAIT_R:
    {
      struct MQ_Lock_Wait_Ent *i_next;
      int *i_granted_ptr;
      int n_sharers_to_wake = 0;
      int seen_max_w_ss = 0;

      for (; i; i = i_next) {
	i_next = i->_next; /* avoid racing the deallocation of i */
	if (l->Flags & MQ_RWLOCK_PRIORITYORDER) {
	  if ((i->what == MQ_LOCK_WAIT_W) && (! seen_max_w_ss) && (MQ_SCHEDSTATE_CMP(i->waiter_threadstate->CurSchedState,max_w_ss)==0))
	    seen_max_w_ss = 1;
	  if ((i->what != MQ_LOCK_WAIT_R) || (MQ_SCHEDSTATE_CMP(i->waiter_threadstate->CurSchedState,max_w_ss)>0))
	    continue;
	  if (seen_max_w_ss && (MQ_SCHEDSTATE_CMP(i->waiter_threadstate->CurSchedState,max_w_ss)==0))
	    continue;
	} else {
	  if (i->what != MQ_LOCK_WAIT_R)
	    break;
	}
	i_granted_ptr = (int *)i->granted_ptr;
	++n_woken;
	MQ_RWLOCK_R_UP(l);
	MQ_RWLock_snip_from_Wait_List(l,i);

	if (l->Flags & MQ_RWLOCK_INHERITPRIORITY)
	  (void)_MQ_SchedState_UpdateLockForUnblockedThread(i->waiter_threadstate,l);

	if (i->waiter_threadstate->RWLock_ThreadState->BlockedOnLock.ID_with_status) {
	  union __MQ_SchedState start_bol = { .EnBloc = i->waiter_threadstate->RWLock_ThreadState->BlockedOnLock.ID_with_status };
	  do {
	    start_bol.EnBloc &= ~MQ_RWLOCK_TRACK_ACCESSLOCK;
	  } while (! MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(i->waiter_threadstate->RWLock_ThreadState->BlockedOnLock.ID_with_status,start_bol.EnBloc, 0UL));
	}

	if ((l->Flags & MQ_RWLOCK_STATISTICS) && (i->wait_start_at)) {
	  ++l->R_Waits;
	  l->R_Wait_Time_Cum += (double)(now - i->wait_start_at);
	}

	MQ_SyncInt_Put(i->granted,1);

	if (i_granted_ptr == &l->Shared_Futex_Granted) {
	  if (! l->Shared_Futex_Granted)
	    MQ_SyncInt_Put(l->Shared_Futex_Granted,1);
	  ++n_sharers_to_wake;
	  continue;
	}

	futex(&i->granted,FUTEX_WAKE,0x7fffffff,0,0,0);
      }

      if (n_sharers_to_wake > 0)
	futex(&l->Shared_Futex_Granted,FUTEX_WAKE,0x7fffffff,0,0,0);
    }
    break;

  case MQ_LOCK_WAIT_R2W:
    if (MQ_RWLOCK_R_COUNT(l) == 1) {
      MQ_RWLOCK_RW_R2W(l);
      l->RW.Owner = i->Waiter_TSA;
      MQ_RWLock_snip_from_Wait_List(l,i);

      if (l->Flags & MQ_RWLOCK_INHERITPRIORITY)
	(void)_MQ_SchedState_UpdateLockForUnblockedThread(i->waiter_threadstate,l);

      if (i->waiter_threadstate->RWLock_ThreadState->BlockedOnLock.ID_with_status) {
	union __MQ_SchedState start_bol = { .EnBloc = i->waiter_threadstate->RWLock_ThreadState->BlockedOnLock.ID_with_status };
	do {
	  start_bol.EnBloc &= ~MQ_RWLOCK_TRACK_ACCESSLOCK;
	} while (! MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(i->waiter_threadstate->RWLock_ThreadState->BlockedOnLock.ID_with_status,start_bol.EnBloc, 0UL));
      }

      if ((l->Flags & MQ_RWLOCK_STATISTICS) && (i->wait_start_at)) {
	++l->R2W_Waits;
	l->R2W_Wait_Time_Cum += (double)(now - i->wait_start_at);
      }
      i->granted = 1;

      ++n_woken;
      futex(&i->granted,FUTEX_WAKE,1,0,0,0);
    }
    break;
  case MQ_LOCK_WAIT_W:
    if (! MQ_RWLOCK_RW_LOCKED_P(l)) {
      MQ_RWLOCK_W_UP(l);
      l->RW.Owner = i->Waiter_TSA;
      MQ_RWLock_snip_from_Wait_List(l,i);

      if (l->Flags & MQ_RWLOCK_INHERITPRIORITY)
	(void)_MQ_SchedState_UpdateLockForUnblockedThread(i->waiter_threadstate,l);

      if (i->waiter_threadstate->RWLock_ThreadState->BlockedOnLock.ID_with_status) {
	union __MQ_SchedState start_bol = { .EnBloc = i->waiter_threadstate->RWLock_ThreadState->BlockedOnLock.ID_with_status };
	do {
	  start_bol.EnBloc &= ~MQ_RWLOCK_TRACK_ACCESSLOCK;
	} while (! MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(i->waiter_threadstate->RWLock_ThreadState->BlockedOnLock.ID_with_status,start_bol.EnBloc, 0UL));
      }

      if ((l->Flags & MQ_RWLOCK_STATISTICS) && (i->wait_start_at)) {
	++l->W_Waits;
	/* bug in gcc -O causes spurious -Wmaybe-uninitialized on this access to "now" */
	_Pragma("GCC diagnostic push");
	_Pragma("GCC diagnostic ignored \"-Wmaybe-uninitialized\"");
	l->W_Wait_Time_Cum += (double)(now - i->wait_start_at);
	_Pragma("GCC diagnostic pop");
      }
      i->granted = 1;

      ++n_woken;
      futex(&i->granted,FUTEX_WAKE,1,0,0,0);
    }
    break;
  default:
    __unreachable;
  }

  return n_woken;
}


int _MQ_Cond_Signal(MQ_RWLock_t *cond, int maxtowake, void *arg, uint64_t flags, const char *file, const int line, const char *func) {
  if (((cond->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(cond->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno((cond->Flags & MQ_RWLOCK_INITED) ? ENOTRECOVERABLE : EINVAL);

  if (MQ_TSA == MQ_TSA_INITIALIZER) {
    int ret;
    if ((ret=MQ_ThreadState_Init()))
      return ret;
  }

  int n_woken = 0;
  int ret;
  int got_peak_pri = 0;

  if (maxtowake == 0)
    MQ_returnerrno_or_abort(EINVAL,cond->Flags & MQ_RWLOCK_ABORTONMISUSE);

  if (cond->Flags & MQ_RWLOCK_STATISTICS)
    MQ_SyncInt_Increment(cond->S_Reqs,1);

  int64_t LFList_ID;
  if (cond->MutEx.Owner == MQ_TSA) /* caller may be using the mutex to lock in, via MQ_Mutex_Lock() -- that's fine as long as we notice! */
    LFList_ID = 0;
  else {
    if (cond->Flags & MQ_RWLOCK_INHERITPRIORITY) {
      if (! MQ_SchedState_AdoptPeak())
	got_peak_pri = 1;
    }

    LFList_ID = _MQ_RWLock_MutEx_Lock(cond,0);

    if (LFList_ID<0) {
      if (got_peak_pri)
	MQ_SchedState_ReleasePeak();
      if (cond->Flags & MQ_RWLOCK_STATISTICS)
	MQ_SyncInt_Increment(cond->S_Fails,1);
      return (int)LFList_ID;
    }
    if (! LFList_ID)
      LFList_ID = 1;
  }

  MQ_SyncInt_SetFlags(MQ_RWLOCK_RW_RAWCOUNT(cond),MQ_RWLOCK_COUNT_CONTENDED); /* this also implicitly aborts RTM trans, if any */

  if ((cond->Flags & MQ_RWLOCK_WAITLIST_DIRTY) && (MQ_SyncInt_Get(cond->Flags) & MQ_RWLOCK_WAITLIST_DIRTY)) {
    if (cond->Flags & MQ_RWLOCK_ABORTWHENCORRUPTED)
      MQ_abort();
    MQ_SyncInt_SetFlags(cond->Flags,MQ_RWLOCK_WAITLIST_CORRUPTED);
  }
  if (cond->Flags & MQ_RWLOCK_WAITLIST_CORRUPTED) {
    ret = MQ_seterrpoint(ENOTRECOVERABLE);
    goto out;
  }

  if (flags & MQ_COND_RETURN_UNLOCKED) {
    if (cond->RW.Owner == MQ_TSA) {
      _MQ_W_DropLock(cond);
      MQ_RWLOCK_W_DOWN(cond);
      MQ_TSA_Invalidate(cond->RW.Owner); /* leave a mark */
      if (cond->Flags & MQ_RWLOCK_DEBUGGING) {
	cond->last_W_Unlocker_file = file;
	cond->last_W_Unlocker_line = line;
	cond->last_W_Unlocker_function = func;
	cond->last_W_Unlocker_tid = gettid();
      }
    } else if ((MQ_RWLOCK_R_LOCKED_P(cond)) && (! _MQ_R_DropLock(cond))) {
      MQ_RWLOCK_R_DOWN(cond);
      if (cond->Flags & MQ_RWLOCK_DEBUGGING) {
	cond->last_R_Unlocker_file = file;
	cond->last_R_Unlocker_line = line;
	cond->last_R_Unlocker_function = func;
	cond->last_R_Unlocker_tid = gettid();
      }
    } else if (LFList_ID) {
      /* don't allow caller to specify _COND_RETURN_UNLOCKED without passing in a usable lock of some description */
      if (got_peak_pri)
	MQ_SchedState_ReleasePeak();
      (void)_MQ_RWLock_MutEx_Unlock(cond);
      MQ_returnerrno_or_abort(EINVAL,cond->Flags & MQ_RWLOCK_ABORTONMISUSE);
    }
  }

  if (! cond->Wait_List_Head) {
    ret = 0;
    goto out;
  }

  { /* sub-blocking to resolve -Wjump-misses-init for i; block closes immediately before "out" label */
  struct MQ_Lock_Wait_Ent * volatile i = cond->Wait_List_Head;

  union __MQ_SchedState max_w_ss = { .EnBloc = 0 };

  if (cond->Flags & MQ_RWLOCK_PRIORITYORDER) {
    /* R and W contend with each other, with the first of either one of them at the highest pri going.
     * If it's an R, then all R's with pri greater than the highest write waiter pri,
     * or with pri equal to the highest write waiter pri and appearing before the first such write waiter, go too.
     */
    struct MQ_Lock_Wait_Ent *j;
    for (j=i; j && (! (j->what & (MQ_LOCK_WAIT_R|MQ_LOCK_WAIT_W))); j=j->_next) {
      if ((j->what & (MQ_COND_WAIT_W|MQ_COND_WAIT_W_RETUNLOCKED|MQ_COND_WAIT_MUTEX|MQ_COND_WAIT_MUTEX_RETUNLOCKED)) &&
	  (MQ_SCHEDSTATE_CMP(j->waiter_threadstate->CurSchedState,max_w_ss)<0)) {
	max_w_ss.EnBloc = j->waiter_threadstate->CurSchedState.EnBloc;
	i = j;
      } else if (MQ_SCHEDSTATE_CMP(j->waiter_threadstate->CurSchedState,i->waiter_threadstate->CurSchedState)<0)
	i = j;
    }
  }

 switch_again:

  switch(i->what) {
  case MQ_LOCK_WAIT_R2W:
    i = i->_next;
    if (i)
      goto switch_again;
    else
      break;
  case MQ_LOCK_WAIT_R:
  case MQ_LOCK_WAIT_W:
    break;
  case MQ_COND_WAIT_R:
  case MQ_COND_WAIT_R_RETUNLOCKED:
    {
      struct MQ_Lock_Wait_Ent *i_next;
      int *i_granted_ptr;
      int n_sharers_to_wake = 0;
      int seen_max_w_ss = 0;

      for (; i; i = i_next) {
	i_next = i->_next; /* avoid racing the deallocation of i */

	if (cond->Flags & MQ_RWLOCK_PRIORITYORDER) {
	  if ((i->what & (MQ_COND_WAIT_W|MQ_COND_WAIT_W_RETUNLOCKED|MQ_COND_WAIT_MUTEX|MQ_COND_WAIT_MUTEX_RETUNLOCKED)) &&
	      (! seen_max_w_ss) &&
	      (MQ_SCHEDSTATE_CMP(i->waiter_threadstate->CurSchedState,max_w_ss)==0))
	    seen_max_w_ss = 1;
	  if ((! (i->what & (MQ_COND_WAIT_R|MQ_COND_WAIT_R_RETUNLOCKED))) || (MQ_SCHEDSTATE_CMP(i->waiter_threadstate->CurSchedState,max_w_ss)>0))
	    continue;
	  if (seen_max_w_ss && (MQ_SCHEDSTATE_CMP(i->waiter_threadstate->CurSchedState,max_w_ss)==0))
	    continue;
	} else {
	  if (! (i->what & (MQ_COND_WAIT_R|MQ_COND_WAIT_R_RETUNLOCKED)))
	    break;
	}

	i_granted_ptr = (int *)i->granted_ptr;
	if ((maxtowake < 0) || (n_woken < maxtowake)) {
	  ++n_woken;
	  i->arg = arg;
	  MQ_RWLock_snip_from_Wait_List(cond,i);

	  if (i->what == MQ_COND_WAIT_R_RETUNLOCKED) {
	    MQ_SyncInt_Put(i->granted,1);
	    if (i_granted_ptr == &cond->Shared_Futex_Granted) {
	      if (! cond->Shared_Futex_Granted)
		MQ_SyncInt_Put(cond->Shared_Futex_Granted,1);
	      ++n_sharers_to_wake;
	      continue;
	    }
	  } else if (MQ_RWLOCK_W_NOTLOCKED_P(cond)) {
	    MQ_RWLOCK_R_UP(cond); /* MQ_R_Addlock() has to be done by the waiter */
	    MQ_SyncInt_Put(i->granted,1);
	    if (i_granted_ptr == &cond->Shared_Futex_Granted) {
	      if (! cond->Shared_Futex_Granted)
		MQ_SyncInt_Put(cond->Shared_Futex_Granted,1);
	      ++n_sharers_to_wake;
	      continue;
	    }
	  } else {
	    /* woken, but alas right back into a wait.
	     *
	     * the n initial referencers that are going back into wait
	     * don't need their granted_ptr updated (can continue to
	     * share), because they're moved with their leader,
	     * preserving order (and contiguity, because we have the
	     * mutex here).
	     */
	    i->what = MQ_LOCK_WAIT_R;
	    i->_prev = cond->Wait_List_Tail;
	    MQ_SyncInt_SetFlags(cond->Flags,MQ_RWLOCK_WAITLIST_DIRTY);
	    if (cond->Wait_List_Tail) {
	      cond->Wait_List_Tail->_next = i;
	      cond->Wait_List_Tail = i;
	    } else
	      cond->Wait_List_Head = cond->Wait_List_Tail = i;
	    ++cond->Wait_List_Length; /* was decremented by snip */
	    MQ_SyncInt_ClearFlags(cond->Flags,MQ_RWLOCK_WAITLIST_DIRTY);
	    continue; /* leader-follower relationships carry forward */
	  }
	} else {
	  /* FUTEX_WAKE of the shared granted is always needed if
	   * there are sharers excluded by a maxtowake limit.  The
	   * fate of the excluded ents decouples from that of those included.
	   */
	  if (i_granted_ptr == &cond->Shared_Futex_Granted) {
	    if (! cond->Shared_Futex_Granted)
	      MQ_SyncInt_Put(cond->Shared_Futex_Granted,1);
	    ++n_sharers_to_wake;
	  }
	  continue;
	}

	/* have to FUTEX_WAKE non-sharers as we go, iff they're granted. */
	if (i->granted)
	  futex(&i->granted,FUTEX_WAKE,0x7fffffff,0,0,0);
      }

      if (n_sharers_to_wake > 0)
	futex(&cond->Shared_Futex_Granted,FUTEX_WAKE,0x7fffffff,0,0,0);
    }
    break;

  case MQ_COND_WAIT_W:
  case MQ_COND_WAIT_W_RETUNLOCKED:
  case MQ_COND_WAIT_MUTEX:
  case MQ_COND_WAIT_MUTEX_RETUNLOCKED:

    ++n_woken;
    i->arg = arg;
    MQ_RWLock_snip_from_Wait_List(cond,i);

    if (i->what & (MQ_COND_WAIT_W_RETUNLOCKED|MQ_COND_WAIT_MUTEX_RETUNLOCKED|MQ_COND_WAIT_MUTEX))
      i->granted = 1;
    else if (! MQ_RWLOCK_RW_LOCKED_P(cond)) {
      MQ_RWLOCK_W_UP(cond);
      cond->RW.Owner = i->Waiter_TSA;
      i->granted = 1;
    } else {
      /* woken, but alas right back into a wait */
      i->what = MQ_LOCK_WAIT_W;
      i->_prev = cond->Wait_List_Tail;
      MQ_SyncInt_SetFlags(cond->Flags,MQ_RWLOCK_WAITLIST_DIRTY);
      if (cond->Wait_List_Tail) {
	cond->Wait_List_Tail->_next = i;
	cond->Wait_List_Tail = i;
      } else
	cond->Wait_List_Head = cond->Wait_List_Tail = i;
      ++cond->Wait_List_Length; /* was decremented by snip */
      MQ_SyncInt_ClearFlags(cond->Flags,MQ_RWLOCK_WAITLIST_DIRTY);
      break;
    }

    futex(&i->granted,FUTEX_WAKE,1,0,0,0);

    break;
  }

  ret = 0;

  }

 out:

  if (LFList_ID) {
    if (MQ_RWLOCK_RW_LOCKED_P(cond)) {
      int ret2;
      ret2 = _MQ_RWLock_MutEx_Unlock(cond);
      if (ret2<0)
	ret = ret2;
    } else {
      int ret2 = _MQ_RWLock_Signal(cond);
      if (ret2<0)
	ret = ret2;
    }
  } else {
    if (! MQ_RWLOCK_RW_LOCKED_P(cond)) {
      /* have to call _MQ_RWLock_Signal_1() directly to keep the MutEx if the caller passed it in locked and we're not _COND_RETURN_UNLOCKED */
      if (cond->Wait_List_Head) {
	int ret2;
	ret2 = _MQ_RWLock_Signal_1(cond);
	if (ret2<0)
	  ret = ret2;
      }
    }
    if (flags & MQ_COND_RETURN_UNLOCKED) {
      int ret2 = _MQ_RWLock_MutEx_Unlock_tracking(cond);
      if (ret2<0)
	ret = ret2;
      else if (cond->Flags & MQ_RWLOCK_DEBUGGING) {
	cond->last_M_Unlocker_file = file;
	cond->last_M_Unlocker_line = line;
	cond->last_M_Unlocker_function = func;
	cond->last_M_Unlocker_tid = gettid();
      }
    }
  }

  if (got_peak_pri)
    MQ_SchedState_ReleasePeak();

  if (cond->Flags & MQ_RWLOCK_STATISTICS) {
    if (ret<0)
      MQ_SyncInt_Increment(cond->S_Fails,1);
    else if (ret == 0)
      MQ_SyncInt_Increment(cond->S_Misses,1);
  }

  if (! ret)
    return n_woken;
  else
    return ret;
}

#ifndef MQ_NO_PTHREADS
static void _MQ_RWLock_Wait_cleanup_handler(struct MQ_Lock_Wait_Ent *ent) {
  int64_t lockret;

  if (ent->granted_ptr == &ent->l->Shared_Futex_Granted)
    MQ_SyncInt_Decrement(ent->l->Shared_Futex_RefCount,1);

  if (ent->async_origtype != -1)
    (void)pthread_setcanceltype(ent->async_origtype,0);

  if (ent->what == MQ_COND_WAIT_MUTEX_RETUNLOCKED)
    lockret = _MQ_RWLock_MutEx_Lock_tracking(ent->l,0);
  else
    lockret = _MQ_RWLock_MutEx_Lock(ent->l,0);

  if (lockret<0) {
    if (ent->l->Flags & MQ_RWLOCK_ABORTWHENCORRUPTED)
      MQ_abort();
    MQ_SyncInt_SetFlags(ent->l->Flags,MQ_RWLOCK_WAITLIST_CORRUPTED);
    return;
  }

  MQ_RWLock_snip_from_Wait_List(ent->l,ent);

  if (ent->what == MQ_COND_WAIT_MUTEX)
    return;

  if (ent->what == MQ_COND_WAIT_MUTEX_RETUNLOCKED)
    (void)_MQ_RWLock_MutEx_Unlock_tracking(ent->l);
  else
    (void)_MQ_RWLock_MutEx_Unlock(ent->l);

  /* use the _contended interfaces directly here, to suppress duplicate HeldLocks entries.  a cond lock is usually contended anyway. */
  if (ent->what == MQ_COND_WAIT_W) {
    if (ent->l->Flags & MQ_RWLOCK_DEBUGGING)
      lockret = _MQ_W_Lock_contended(ent->l,MQ_FOREVER,ent->file,ent->line,ent->function);
    else
      lockret = _MQ_W_Lock_contended(ent->l,MQ_FOREVER,0,0,0);
  } else if (ent->what == MQ_COND_WAIT_R) {
    if (ent->l->Flags & MQ_RWLOCK_DEBUGGING)
      lockret = _MQ_R_Lock_contended(ent->l,MQ_FOREVER,ent->file,ent->line,ent->function);
    else
      lockret = _MQ_R_Lock_contended(ent->l,MQ_FOREVER,0,0,0);
    if (ent->l->Flags & MQ_RWLOCK_NOLOCKTRACKING)
      --__MQ_RWLock_ThreadState.HeldLocks_Index;
  } else
    lockret = 0;

  if (lockret<0) {
    if (ent->l->Flags & MQ_RWLOCK_ABORTWHENCORRUPTED)
      MQ_abort();
    MQ_SyncInt_SetFlags(ent->l->Flags,MQ_RWLOCK_WAITLIST_CORRUPTED);
  }
}
#endif

/* must be called with the mutex held, to be released after lodging the wait instance in the Wait_List (returned locked iff what == MQ_COND_WAIT_MUTEX) */
static __wur int _MQ_RWLock_Wait_1(MQ_RWLock_t *l, MQ_Wait_Type what, MQ_Time_t wait_nsecs, void **arg, int64_t LFList_ID, const char *file, const int line, const char *func) {
  int ret;
  int64_t ret2;

  if ((l->Flags & MQ_RWLOCK_WAITLIST_DIRTY) && (MQ_SyncInt_Get(l->Flags) & MQ_RWLOCK_WAITLIST_DIRTY)) {
    if (l->Flags & MQ_RWLOCK_ABORTWHENCORRUPTED)
      MQ_abort();
    MQ_SyncInt_SetFlags(l->Flags,MQ_RWLOCK_WAITLIST_CORRUPTED);
  }
  if (l->Flags & MQ_RWLOCK_WAITLIST_CORRUPTED) {
    ret = MQ_seterrpoint(ENOTRECOVERABLE);
    goto out;
  }

  if ((wait_nsecs <= 0) || (! MQ_Wait_Type_OK_p(what))) {
    ret = MQ_seterrpoint_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
    goto out;
  }

  { /* sub-block from here to "out" label to resolve -Wjump-misses-init */

  uint64_t rwcount_at_entry = MQ_SyncInt_SetFlags(MQ_RWLOCK_RW_RAWCOUNT(l),MQ_RWLOCK_COUNT_CONTENDED) & ((MQ_RWLOCK_COUNT_MINFLAG-1)|MQ_RWLOCK_COUNT_READ);

  struct MQ_Lock_Wait_Ent ent =
    { .l = l,
      .what = what,
      .wait_start_at = 0,
      .granted = 0,
      .Waiter_TSA = MQ_TSA,
      .waiter_threadstate = __MQ_ThreadState_Pointer,
      .async_origtype = -1,
      .LFList_ID = LFList_ID,
      .file = file,
      .line = line,
      .function = func
    };

  struct MQ_Lock_Wait_Ent *i;

  switch (what) {
  case MQ_LOCK_WAIT_R2W:

    if (! MQ_R_HaveLock_p(l)) {
      ret = MQ_seterrpoint_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
      goto out;
    }

    if (rwcount_at_entry == (1UL|MQ_RWLOCK_COUNT_READ)) {
      MQ_RWLOCK_RW_R2W(l);
      l->RW.Owner = MQ_TSA;
      return _MQ_RWLock_MutEx_Unlock(l);
    }

    __MQ_RWLock_ThreadState.BlockedOnLock.ID_with_status = (uint64_t)l|MQ_RWLOCK_TRACK_WRITE;

    /*
     * for R2W, we jump in front unconditionally, since we already
     * have a lock that's holding up the works.
     */
    i=l->Wait_List_Head;
    /* if two threads are simultaneously in WAIT_R2W on the same lock, that's a classic deadlock.  definitely wouldn't do anyone any good. */
    if (i && (i->what == MQ_LOCK_WAIT_R2W)) {
      ret = MQ_seterrpoint_or_abort(EDEADLK,l->Flags & MQ_RWLOCK_ABORTONEDEADLK);
      goto out;
    }
    goto insert;

  case MQ_COND_WAIT_W:
  case MQ_COND_WAIT_W_RETUNLOCKED:

    if (what == MQ_COND_WAIT_W_RETUNLOCKED) {
      if ((ret=_MQ_W_DropLock(l))<0)
	goto out;
    }
    MQ_TSA_Invalidate(l->RW.Owner); /* leave a mark */
    MQ_RWLOCK_W_DOWN(l);
    if ((! MQ_RWLOCK_RW_LOCKED_P(l)) &&
	l->Wait_List_Head)
      (void)_MQ_RWLock_Signal_1(l);

    /* fall through */

  case MQ_COND_WAIT_MUTEX:
  case MQ_COND_WAIT_MUTEX_RETUNLOCKED:

    goto cond_wait_iteration;

  case MQ_COND_WAIT_R:
  case MQ_COND_WAIT_R_RETUNLOCKED:

    if (what == MQ_COND_WAIT_R_RETUNLOCKED) {
      if ((ret=_MQ_R_DropLock(l))<0)
	goto out;
    }
    MQ_RWLOCK_R_DOWN(l);
    if ((! MQ_RWLOCK_RW_LOCKED_P(l)) &&
	l->Wait_List_Head)
      (void)_MQ_RWLock_Signal_1(l);

  cond_wait_iteration:

    /* for _COND_WAIT, we jump in front of any regular lock waiters,
     * because we already had to hold a lock to get this far, and it
     * produces delays and deadlocks to demote the thread.  But we
     * don't jump in front of an R2W waiter if any, and we append to
     * any existing _COND_WAIT_ ents:
     */

    for (i=l->Wait_List_Head; i; i=i->_next) {
      if (i->what & (MQ_LOCK_WAIT_R|MQ_LOCK_WAIT_W))
	break;
    }

  insert:

    if (i) {
      MQ_SyncInt_SetFlags(l->Flags,MQ_RWLOCK_WAITLIST_DIRTY);
      if (i->_prev) {
	ent._prev = i->_prev;
	i->_prev->_next = &ent;
      } else {
	l->Wait_List_Head = &ent;
	ent._prev = 0;
      }
      ent._next = i;
      i->_prev = &ent;
      break;
    }
    goto append;

  case MQ_LOCK_WAIT_R:

    if (l->RW.Owner == MQ_TSA) {
      ret = MQ_seterrpoint_or_abort(EDEADLK,l->Flags & MQ_RWLOCK_ABORTONEDEADLK);
      goto out;
    }

    if ((! rwcount_at_entry) || (rwcount_at_entry&MQ_RWLOCK_COUNT_READ)) {
      MQ_RWLOCK_R_UP(l);
      return _MQ_RWLock_MutEx_Unlock(l);
    }

    __MQ_RWLock_ThreadState.BlockedOnLock.ID_with_status = (uint64_t)l|MQ_RWLOCK_TRACK_READ;

    goto append;

  case MQ_LOCK_WAIT_W:

    /* recursive W locks are allowed at the inline level */
    if ((l->RW.Owner == MQ_TSA) || MQ_R_HaveLock_p(l)) {
      ret = MQ_seterrpoint_or_abort(EDEADLK,l->Flags & MQ_RWLOCK_ABORTONEDEADLK);
      goto out;
    }

    if (! rwcount_at_entry) {
      MQ_RWLOCK_W_UP(l);
      l->RW.Owner = MQ_TSA;
      return _MQ_RWLock_MutEx_Unlock(l);
    }

    __MQ_RWLock_ThreadState.BlockedOnLock.ID_with_status = (uint64_t)l|MQ_RWLOCK_TRACK_WRITE;

  append:
    /* regular lock waiters just get appended: */

    MQ_SyncInt_SetFlags(l->Flags,MQ_RWLOCK_WAITLIST_DIRTY);

    ent._prev = l->Wait_List_Tail;
    ent._next = 0;
    if (l->Wait_List_Tail) {
      l->Wait_List_Tail->_next = &ent;
      l->Wait_List_Tail = &ent;
    } else
      l->Wait_List_Head = l->Wait_List_Tail = &ent;

  }

  if ((! (l->Flags & MQ_RWLOCK_ROBUST)) &&
      (what & (MQ_LOCK_WAIT_R|MQ_COND_WAIT_R|MQ_COND_WAIT_R_RETUNLOCKED)) &&
      ((! l->Shared_Futex_RefCount) ||
       (ent._prev &&
	((ent._prev->what == what) || ((ent._prev->what&MQ_LOCK_WAIT_R) == (what&MQ_LOCK_WAIT_R))) &&
	(ent._prev->granted_ptr == &l->Shared_Futex_Granted)))) {
    /* for shared requests, for the first resort, arrange to be
     * awakened by the same FUTEX_WAKE as the n like neighbors.  The
     * Shared_Futex_Granted will propagate through as many contiguous
     * like ents as insert/append before the wake.
     */
    ent.granted_ptr = &l->Shared_Futex_Granted;
    MQ_SyncInt_Increment(l->Shared_Futex_RefCount,1); /* have to use hardware lock because this is decremented outside the mutex below */
    if (l->Shared_Futex_Granted)
      l->Shared_Futex_Granted = 0; /* we're mutually excluded with the wakers here, so this is safe and simple. */
  } else
    ent.granted_ptr = &ent.granted;

  ++(l)->Wait_List_Length;
  MQ_SyncInt_ClearFlags(l->Flags,MQ_RWLOCK_WAITLIST_DIRTY);

  if (__MQ_RWLock_ThreadState.BlockedOnLock.ID_with_status && (l->Flags & MQ_RWLOCK_INHERITPRIORITY))
    (void)_MQ_SchedState_UpdateLockForBlockedThread(__MQ_ThreadState_Pointer);

  if (what == MQ_COND_WAIT_MUTEX_RETUNLOCKED) {
    if ((ret=_MQ_RWLock_MutEx_Unlock_tracking(l))<0)
      return ret;
  } else {
    if ((ret=_MQ_RWLock_MutEx_Unlock(l))<0)
      return ret;
  }

  /* volatiles to avoid compiler warnings ("might be clobbered") from the setjmp in pthread_cleanup_push */
  volatile MQ_Time_t wait_nsecs_volatile = wait_nsecs;
  if (wait_nsecs_volatile == MQ_DEFAULTTIME)
    wait_nsecs_volatile = MQ_RWLOCK_TIMEOUT_DEFAULT;
  volatile struct timespec ts;
  volatile MQ_Time_t start_waiting_at;

#ifndef MQ_NO_PTHREADS
  pthread_cleanup_push(_MQ_RWLock_Wait_cleanup_handler,&ent);

  if (what & (MQ_COND_WAIT_R|MQ_COND_WAIT_R_RETUNLOCKED|MQ_COND_WAIT_W|MQ_COND_WAIT_W_RETUNLOCKED|MQ_COND_WAIT_MUTEX|MQ_COND_WAIT_MUTEX_RETUNLOCKED)) {
    if ((pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS,&ent.async_origtype)<0) ||
	(ent.async_origtype == PTHREAD_CANCEL_ASYNCHRONOUS))
      ent.async_origtype = -1;
    pthread_testcancel();
  } else
    ent.async_origtype = -1;
#endif

 futex_again:

  if (ent.granted) {
    ret = 0;
    if (ent.granted_ptr == &l->Shared_Futex_Granted)
      MQ_SyncInt_Decrement(l->Shared_Futex_RefCount,1);
    goto no_futex_after_all;
  }

  if (l->Flags & MQ_RWLOCK_STATISTICS) {
    MQ_SyncInt_Increment(l->futex_trips,1);
    if (! ent.wait_start_at)
      ent.wait_start_at = MQ_Time_Now_Monotonic_inline();
  }

  if (wait_nsecs_volatile > 0) {
    ts.tv_sec = wait_nsecs_volatile/MQ_ONE_SECOND;
    ts.tv_nsec = wait_nsecs_volatile - ((MQ_Time_t)ts.tv_sec * MQ_ONE_SECOND);
    start_waiting_at = MQ_Time_Now_Monotonic_inline();
    ret = futex(ent.granted_ptr,FUTEX_WAIT,0,(const struct timespec *)&ts,0,0);
  } else
    ret = futex(ent.granted_ptr,FUTEX_WAIT,0,0,0,0);

  if (ent.granted_ptr == &l->Shared_Futex_Granted) {
    MQ_SyncInt_Decrement(l->Shared_Futex_RefCount,1);
    /* if we may FUTEX_WAIT again, we have to mutually exclude with the wakers while updating our granted_ptr.
     *
     * if the waker sees the waiter at the shared granted_ptr at the start of the iteration, that is where it will wake it after setting it granted,
     * but without mutexing, the waiter may have updated its granted_ptr after the waker read it, and resumed waiting before the waker granted it, deadlocking it.
     */
    if ((ret && (errno != EWOULDBLOCK)) || ((! ent.granted) && (! MQ_SyncInt_Get(ent.granted)))) {
      if (what == MQ_COND_WAIT_MUTEX_RETUNLOCKED)
	ret2 = _MQ_RWLock_MutEx_Lock_tracking(l,0);
      else
	ret2 = _MQ_RWLock_MutEx_Lock(l,0);
      if (ret2<0) {
	if (l->Flags & MQ_RWLOCK_ABORTWHENCORRUPTED)
	  MQ_abort();
	MQ_SyncInt_SetFlags(l->Flags,MQ_RWLOCK_WAITLIST_CORRUPTED);
	return (int)ret2;
      }
      ent.granted_ptr = &ent.granted;
      if (what == MQ_COND_WAIT_MUTEX_RETUNLOCKED)
	(void)_MQ_RWLock_MutEx_Unlock_tracking(l);
      else
	(void)_MQ_RWLock_MutEx_Unlock(l);
    }
  }

  if (ret || ((! ent.granted) && (! MQ_SyncInt_Get(ent.granted)))) {
    if (ent.granted || MQ_SyncInt_Get(ent.granted))
      ret = 0;
    else if ((errno == EACCES) || (errno == EFAULT) || (errno == EINVAL))
      ret = MQ_seterrpoint(errno);
    else {
      if (errno == ETIMEDOUT)
	ret = MQ_seterrpoint(ETIMEDOUT);
      else {
	if (wait_nsecs_volatile > 0) {
	  wait_nsecs_volatile -= (MQ_Time_Now_Monotonic_inline() - start_waiting_at);
	  if (wait_nsecs_volatile <= 0)
	    ret = MQ_seterrpoint(ETIMEDOUT);
	  else
	    goto futex_again;
	} else
	  goto futex_again;
      }
    }
  }

 no_futex_after_all:

#ifndef MQ_NO_PTHREADS
  if (ent.async_origtype != -1)
    (void)pthread_setcanceltype(ent.async_origtype,0);

  pthread_cleanup_pop(0);
#endif

  if (ret == 0) { /* _MQ_RWLock_Signal_1() already took care of removing the LL entry and adjusting the appropriate counts */
    /* __MQ_RWLock_ThreadState.BlockedOnLock.ID_with_status is cleared by the signaller (and _MQ_SchedState_UpdateLockForUnblockedThread() is called) */
    if (arg)
      *arg = ent.arg;
    if (what == MQ_COND_WAIT_MUTEX) {
      ret2 = _MQ_RWLock_MutEx_Lock(l,0);
      if (ret2 < 0)
	return (int)ret2;
      else
	return 0;
    } else
      return ret;
  }

  if (what == MQ_COND_WAIT_MUTEX_RETUNLOCKED)
    ret2 = _MQ_RWLock_MutEx_Lock_tracking(l,0);
  else
    ret2 = _MQ_RWLock_MutEx_Lock(l,0);
  if (ret2<0) {
    if (l->Flags & MQ_RWLOCK_ABORTWHENCORRUPTED)
      MQ_abort();
    MQ_SyncInt_SetFlags(l->Flags,MQ_RWLOCK_WAITLIST_CORRUPTED);
    return (int)ret2;
  }

  /* need to make one last check that we're not granted -- when
   * there's a timeout on the FUTEX_WAIT, there are races between the
   * unlock routines and the FUTEX_WAIT, because (obviously) the
   * LFList MutEx isn't held during FUTEX_WAIT.
   */
  if (ent.granted) {
    ret = 0;
    if (arg)
      *arg = ent.arg;
    goto out;
  }

  MQ_RWLock_snip_from_Wait_List(l,&ent);

  if (__MQ_RWLock_ThreadState.BlockedOnLock.ID_with_status) {
    union __MQ_SchedState start_bol = { .EnBloc = __MQ_RWLock_ThreadState.BlockedOnLock.ID_with_status };
    do {
      start_bol.EnBloc &= ~MQ_RWLOCK_TRACK_ACCESSLOCK;
    } while (! MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(__MQ_RWLock_ThreadState.BlockedOnLock.ID_with_status,start_bol.EnBloc, 0UL));

    if (l->Flags & MQ_RWLOCK_INHERITPRIORITY)
      (void)_MQ_SchedState_UpdateLockForUnblockedThread(__MQ_ThreadState_Pointer,l);
  }

  }

 out:

  /* note, LL lock held on entry, not held on return, with one exception */
  if (what != MQ_COND_WAIT_MUTEX) {
    if (what == MQ_COND_WAIT_MUTEX_RETUNLOCKED)
      (void)_MQ_RWLock_MutEx_Unlock_tracking(l);
    else
      (void)_MQ_RWLock_MutEx_Unlock(l);
  }

  return ret;
}

/* cond must be locked, Mutex or R or W, on entry, and unless _COND_RETURN_UNLOCKED, will be locked with the same mode on return or cancellation */
__wur int _MQ_Cond_Wait(MQ_RWLock_t *cond, MQ_Time_t maxwait, void **arg, uint64_t flags, const char *file, const int line, const char *func) {
  if (((cond->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(cond->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno((cond->Flags & MQ_RWLOCK_INITED) ? ENOTRECOVERABLE : EINVAL);

  if (MQ_TSA == MQ_TSA_INITIALIZER)
    MQ_returnerrno_or_abort(EINVAL,cond->Flags & MQ_RWLOCK_ABORTONMISUSE);

  int ret;
  int got_peak_pri = 0;

  if (cond->Flags & MQ_RWLOCK_STATISTICS)
    MQ_SyncInt_Increment(cond->C_Reqs,1);

  int64_t LFList_ID;
  if (cond->MutEx.Owner == MQ_TSA)
    LFList_ID = 0;
  else {
    if (cond->Flags & MQ_RWLOCK_INHERITPRIORITY) {
      if (! MQ_SchedState_AdoptPeak())
	got_peak_pri = 1;
    }

    LFList_ID = _MQ_RWLock_MutEx_Lock(cond,0);

    if (LFList_ID<0) {
      if (got_peak_pri)
	MQ_SchedState_ReleasePeak();
      return (int)LFList_ID;
    }
    if (! LFList_ID)
      LFList_ID = 1;
  }

  MQ_SyncInt_SetFlags(MQ_RWLOCK_RW_RAWCOUNT(cond),MQ_RWLOCK_COUNT_CONTENDED);

  MQ_Wait_Type what;

  if (! LFList_ID) {
    if (flags&MQ_COND_RETURN_UNLOCKED) {
      /* call _MQ_Mutex_DropLock() in _wait_1() immediately before the actual unlock, to minimize priority inversion danger */
      what = MQ_COND_WAIT_MUTEX_RETUNLOCKED;
      if (cond->Flags & MQ_RWLOCK_DEBUGGING) {
	cond->last_M_Unlocker_file = file;
	cond->last_M_Unlocker_line = line;
	cond->last_M_Unlocker_function = func;
	cond->last_M_Unlocker_tid = gettid();
      }
    } else
      what = MQ_COND_WAIT_MUTEX;
  } else if (MQ_RWLOCK_W_COUNT(cond) > 0) {
    if (cond->RW.Owner != MQ_TSA) {
      ret = MQ_seterrpoint_or_abort(EPERM,cond->Flags & MQ_RWLOCK_ABORTONMISUSE);
      goto errout;
    }
    if (MQ_RWLOCK_W_COUNT(cond) > 1) {
      /* this can only happen if _RECURSION */
      ret = MQ_seterrpoint_or_abort(EDEADLK,cond->Flags & MQ_RWLOCK_ABORTONEDEADLK);
      goto errout;
    }
    if (flags&MQ_COND_RETURN_UNLOCKED) {
      /* call _MQ_W_DropLock() in _wait_1() immediately before the actual unlock, to minimize priority inversion danger */
      what = MQ_COND_WAIT_W_RETUNLOCKED;
      if (cond->Flags & MQ_RWLOCK_DEBUGGING) {
	cond->last_W_Unlocker_file = file;
	cond->last_W_Unlocker_line = line;
	cond->last_W_Unlocker_function = func;
	cond->last_W_Unlocker_tid = gettid();
      }
    } else
      what = MQ_COND_WAIT_W;
  } else if (MQ_RWLOCK_R_LOCKED_P(cond)) /* assumes a lot, when _NOCOSTLYERRORCHECKING */  {
    if (flags&MQ_COND_RETURN_UNLOCKED) {
      /* call _MQ_R_DropLock() in _wait_1() immediately before the actual unlock, to minimize priority inversion danger */
      what = MQ_COND_WAIT_R_RETUNLOCKED;
      if (cond->Flags & MQ_RWLOCK_DEBUGGING) {
	cond->last_R_Unlocker_file = file;
	cond->last_R_Unlocker_line = line;
	cond->last_R_Unlocker_function = func;
	cond->last_R_Unlocker_tid = gettid();
      }
    } else
      what = MQ_COND_WAIT_R;
  } else {
    ret = MQ_seterrpoint_or_abort(EINVAL,cond->Flags & MQ_RWLOCK_ABORTONMISUSE);
    goto errout;
  }

  ret = _MQ_RWLock_Wait(cond, what, maxwait, arg, LFList_ID, file, line, func);

  if (got_peak_pri)
    MQ_SchedState_ReleasePeak();

  if (ret == 0) {
    if (what == MQ_COND_WAIT_W) {
      if (cond->Flags & MQ_RWLOCK_DEBUGGING) {
	cond->last_W_Locker_file = file;
	cond->last_W_Locker_line = line;
	cond->last_W_Locker_function = func;
	cond->last_W_Locker_tid = gettid();
      }
    } else if (what == MQ_COND_WAIT_R) {
      if (cond->Flags & MQ_RWLOCK_DEBUGGING) {
	cond->last_R_Locker_file = file;
	cond->last_R_Locker_line = line;
	cond->last_R_Locker_function = func;
	cond->last_R_Locker_tid = gettid();
      }
    } else if (what == MQ_COND_WAIT_MUTEX) {
      if (cond->Flags & MQ_RWLOCK_DEBUGGING) {
	cond->last_M_Locker_file = file;
	cond->last_M_Locker_line = line;
	cond->last_M_Locker_function = func;
	cond->last_M_Locker_tid = gettid();
      }
    }
  } else {
    if (cond->Flags & MQ_RWLOCK_STATISTICS)
      MQ_SyncInt_Increment(cond->C_Fails,1);
    if (flags & MQ_COND_RETURN_UNLOCKED) {
      if ((ret == -EWOULDBLOCK) || (ret == -EINVAL)) {
	if (what == MQ_COND_WAIT_W_RETUNLOCKED) {
	  (void)_MQ_W_AddLock(cond);
	  (void)_MQ_W_Unlock(cond,file,line,func);
	  if (cond->Flags & MQ_RWLOCK_NOLOCKTRACKING)
	    __MQ_RWLock_ThreadState.HeldLocks_Index = 0;
        } else if (what == MQ_COND_WAIT_R_RETUNLOCKED) {
	  (void)_MQ_R_AddLock(cond);
	  (void)_MQ_R_Unlock(cond,file,line,func);
	  if (cond->Flags & MQ_RWLOCK_NOLOCKTRACKING)
	    __MQ_RWLock_ThreadState.HeldLocks_Index = 0;
	}
      }
    } else {
      if ((ret != -EWOULDBLOCK) && (ret != -EINVAL)) {
	int64_t lockret;
	if (what == MQ_COND_WAIT_W) {
	  if ((lockret=_MQ_W_DropLock(cond))==0)
	    lockret = _MQ_W_Lock(cond,MQ_FOREVER,file,line,func);
	  if (cond->Flags & MQ_RWLOCK_NOLOCKTRACKING)
	    __MQ_RWLock_ThreadState.HeldLocks_Index = 0;
	} else if (what == MQ_COND_WAIT_R) {
	  if ((lockret=_MQ_R_DropLock(cond))==0)
	    lockret = _MQ_R_Lock(cond,MQ_FOREVER,file,line,func);
	  if (cond->Flags & MQ_RWLOCK_NOLOCKTRACKING)
	    __MQ_RWLock_ThreadState.HeldLocks_Index = 0;
	} else if (what == MQ_COND_WAIT_MUTEX) {
	  if ((lockret=_MQ_Mutex_DropLock(cond))==0)
	    lockret = _MQ_RWLock_MutEx_Lock_tracking(cond,0);
	  if (cond->Flags & MQ_RWLOCK_NOLOCKTRACKING)
	    __MQ_RWLock_ThreadState.HeldLocks_Index = 0;
	} else
	  lockret = 0;
	if (lockret < 0) {
	  /* MQ_seterrno(ENOTRECOVERABLE); */
	  return (int)lockret;
	}
      }
    }
  }

  return ret;

 errout:

  if (LFList_ID) {
    if (got_peak_pri)
      MQ_SchedState_ReleasePeak();
    (void)_MQ_RWLock_MutEx_Unlock(cond);
  }
  if (cond->Flags & MQ_RWLOCK_STATISTICS)
    MQ_SyncInt_Increment(cond->C_Fails,1);
  return ret;
}

__wur int MQ_RWLock_Cur_Waiter_Count(MQ_RWLock_t *l, MQ_Wait_Type which) {
  if ((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED)
    MQ_returnerrno((l->Flags & (MQ_RWLOCK_INITED)) ? ENOTRECOVERABLE : EINVAL);

  int64_t ret;

  MQ_Time_t onesecond = MQ_ONE_SECOND;
  if ((ret=_MQ_RWLock_MutEx_Lock_tracking(l,&onesecond))<0)
    return (int)ret;

  if ((l->Flags & MQ_RWLOCK_WAITLIST_DIRTY) && (MQ_SyncInt_Get(l->Flags) & MQ_RWLOCK_WAITLIST_DIRTY)) {
    if (l->Flags & MQ_RWLOCK_ABORTWHENCORRUPTED)
      MQ_abort();
    MQ_SyncInt_SetFlags(l->Flags,MQ_RWLOCK_WAITLIST_CORRUPTED);
    MQ_returnerrno(ENOTRECOVERABLE);
  }

  if (! which)
    ret = (int64_t)(l->Wait_List_Length + (int)l->MutEx.Count) - 1; /* don't bother with SyncInt_Get on MutEx --
								 * it's no staler than the value established in the
								 * _MutEx_Lock immediately above.
								 */
  else {
    ret = 0;
    for (struct MQ_Lock_Wait_Ent *i = l->Wait_List_Head; i; i = i->_next)
      if (i->what & which)
	++ret;
  }

  int ret2;
  if ((ret2=_MQ_RWLock_MutEx_Unlock_tracking(l))<0)
    return ret2;

  return (int)ret;
}

#if 0

/* two main kinds of lock inversions: out-of-order as a function of
   file@line class of the lock, and out-of-order per-instance.  Though
   the former must be tracked with resources set aside expressly for
   the purpose, it is intrinsically modest in its resource overhead,
   can be implemented quite efficiently, and detects most inversions
   very early even if the actual deadlock scenario is made exceedingly
   rare by a race condition.  It is a useful hybrid of static and
   dynamic analysis.  The latter only has controlled resource overhead
   if only currently held locks are considered, rather than the
   history of all lock instances held at least once, but then is
   self-limited in its own way, and indeed is simply a matter of
   detecting and interpreting actual runtime deadlocks.  The most
   obvious implementation entails each thread maintaining a registered
   ordered list of all locks currently held (which we do to implement
   priority inheritance), and each lock maintaining a list of all and
   for each lock obtained while holding another lock, all lists must
   be scanned

*/

struct MQ_RWLock_Thread_Locks {
  const char *init_file;
  int init_line;
  const char *init_function;
  const char *_file;
  int init_line;
  const char *init_function;


MQ_DECLARE_LL_STATIC(elptype,llname

use threadspecific cleanup routine mechanism to catch threads terming holding locks


impl priority-based insert into Wait_List
  MQ_setschedprio(double)
  MQ_getschedprio

use const-branching and passed-in flags to switch on/off options

#endif
