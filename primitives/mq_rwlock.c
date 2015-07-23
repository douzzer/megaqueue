/* MegaQueue
 *
 * Copyright 2015 Daniel Pouzzner
 *
 * mq_rwlock.c  2015feb24  daniel.pouzzner@megaqueue.com
 * 
 * uncontended, inlinable portion of:
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

#ifndef MQ_RWLOCK_C
#define MQ_RWLOCK_C

#ifndef MQ_RWLOCK_INLINE
#include "mq.h"
#undef _MQ_RWLOCK_MAYBE_INLINE
#define _MQ_RWLOCK_MAYBE_INLINE
#endif

#ifdef MQ_RWLOCK_INTELRTM_SUPPORT
#include <immintrin.h>
#endif

/* this (implicitly __ATOMIC_SEQ_CST) is the fastest way to cmpxchg, at least for now and on x86-64: */
#define MQ_SyncInt_ExchangeIfEq_MaybeLock(i,from,to) MQ_SyncInt_ExchangeIfEq(i,from,to)
#define MQ_SyncInt_ExchangeIfEq_MaybeUnlock(i,from,to) MQ_SyncInt_ExchangeIfEq(i,from,to)

extern int _MQ_RWLock_CheckFlags(uint64_t CurFlags, uint64_t SetFlags, uint64_t ClearFlags, uint64_t LocalBuildCapMask);

static uint64_t __MQ_RWLock_LocalDefaultFlags = MQ_RWLOCK_DEFAULTFLAGS;

#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
_MQ_RWLOCK_MAYBE_INLINE int _MQ_RWLock_Init(MQ_RWLock_t *l, uint64_t flags, int zero_p, const char *file, const int line, const char *func)
#else
_MQ_RWLOCK_MAYBE_INLINE int _MQ_RWLock_Init(MQ_RWLock_t *l, uint64_t flags, int zero_p)
#endif
{
  if (flags == MQ_RWLOCK_DEFAULTFLAGS) {
    if (zero_p)
      memset(l,0,sizeof *l);
    if (__MQ_RWLock_LocalDefaultFlags == MQ_RWLOCK_DEFAULTFLAGS)
      __MQ_RWLock_LocalDefaultFlags = (MQ_RWLOCK_LOCALDEFAULTFLAGS);
    l->Flags = MQ_RWLOCK_INITED|__MQ_RWLock_LocalDefaultFlags|(__MQ_RWLock_GlobalDefaultFlags&__MQ_RWLock_LocalBuildCapMask);
  } else {
    int ret;
    if ((ret=_MQ_RWLock_CheckFlags(0UL, flags, 0UL, __MQ_RWLock_LocalBuildCapMask))<0)
      return ret;
    if (zero_p)
      memset(l,0,sizeof *l);
    l->Flags = MQ_RWLOCK_INITED|flags;
  }
  l->MutEx.Count = MQ_RWLOCK_COUNT_INITIALIZER;
  l->MutEx.Owner = MQ_TSA_INVALIDOWNER;
  l->RW.Count = MQ_RWLOCK_COUNT_CONTENDED;
  l->MutEx_Spin_Estimator = MQ_RWLOCK_ADAPTIVE_INIT_SPINS;
  l->RW.Owner = MQ_TSA_INVALIDOWNER;
  l->BuildFlags = &__MQ_RWLock_LocalBuildFlags;
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
  if (l->Flags & MQ_RWLOCK_DEBUGGING) {
    l->init_file = file;
    l->init_line = line;
    l->init_function = func;
  }
#endif
  return 0;
}

/* not safe to call until all threads that have held the lock have unlocked, and the unlock routines have all returned,
 * but obviously application-level coordination is an unavoidable necessity for deallocation.
 */
_MQ_RWLOCK_MAYBE_INLINE int MQ_RWLock_Destroy(MQ_RWLock_t *l) {
  MQ_SyncInt_Fence_Consume();
  if (! (l->Flags & MQ_RWLOCK_INITED))
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
  /* give verification of clean shutdown the good college try */
  if ((l->MutEx.Count && (l->MutEx.Count != MQ_RWLOCK_COUNT_INITIALIZER)) ||
      MQ_RWLOCK_RW_LOCKED_P(l) ||
      l->Wait_List_Length)
    MQ_returnerrno_or_abort(EBUSY,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
  MQ_SyncInt_ClearFlags(l->Flags,MQ_RWLOCK_INITED);
  return 0;
}

_MQ_RWLOCK_MAYBE_INLINE int MQ_RWLock_SetDefaultFlags(uint64_t SetFlags) {
  if (SetFlags & ~((MQ_RWLOCK_MAXUSERFLAG<<1)-1UL))
    MQ_returnerrno(EINVAL);
  MQ_SyncInt_SetFlags(__MQ_RWLock_GlobalDefaultFlags,SetFlags);
  return 0;
}

_MQ_RWLOCK_MAYBE_INLINE int MQ_RWLock_ClearDefaultFlags(uint64_t ClearFlags) {
  if (__MQ_RWLock_LocalDefaultFlags == MQ_RWLOCK_DEFAULTFLAGS)
    MQ_SyncInt_Put(__MQ_RWLock_LocalDefaultFlags,(MQ_RWLOCK_LOCALDEFAULTFLAGS) & ~ClearFlags);
  else
    MQ_SyncInt_ClearFlags(__MQ_RWLock_LocalDefaultFlags,ClearFlags);
  MQ_SyncInt_ClearFlags(__MQ_RWLock_GlobalDefaultFlags,ClearFlags);
  return 0;
}

_MQ_RWLOCK_MAYBE_INLINE int MQ_RWLock_SetFlags(MQ_RWLock_t *l,uint64_t SetFlags) {
  int ret;
  if (SetFlags == MQ_RWLOCK_DEFAULTFLAGS) {
    if (__MQ_RWLock_LocalDefaultFlags == MQ_RWLOCK_DEFAULTFLAGS)
      MQ_SyncInt_Put(__MQ_RWLock_LocalDefaultFlags,MQ_RWLOCK_LOCALDEFAULTFLAGS);
    if ((ret=_MQ_RWLock_CheckFlags(l->Flags,__MQ_RWLock_LocalDefaultFlags|(__MQ_RWLock_GlobalDefaultFlags&__MQ_RWLock_LocalBuildCapMask),0UL,__MQ_RWLock_LocalBuildCapMask&(*l->BuildFlags)))<0)
      return ret;
    MQ_SyncInt_SetFlags(l->Flags,__MQ_RWLock_LocalDefaultFlags|(__MQ_RWLock_GlobalDefaultFlags&__MQ_RWLock_LocalBuildCapMask));
    return 0;
  } else {
    if ((ret=_MQ_RWLock_CheckFlags(l->Flags,SetFlags,0UL,__MQ_RWLock_LocalBuildCapMask&(*l->BuildFlags)))<0)
      return ret;
    MQ_SyncInt_SetFlags(l->Flags,SetFlags);
    return 0;
  }
}

_MQ_RWLOCK_MAYBE_INLINE int MQ_RWLock_ClearFlags(MQ_RWLock_t *l,uint64_t ClearFlags) {
  int ret;
  if ((ret=_MQ_RWLock_CheckFlags(l->Flags,0UL,ClearFlags,__MQ_RWLock_LocalBuildCapMask&(*l->BuildFlags)))<0)
    return ret;
  MQ_SyncInt_ClearFlags(l->Flags,ClearFlags);
  return 0;
}

/*
 * the MQ mutex facility
 *
 * Stack up contending threads at entry with a hardware-lock-only-appender 2LL
 * implementing a mutex facility.
 *
 * References for RTM support:
 * see https://01.org/blogs/tlcounts/2014/lock-elision-glibc
 * originally https://lwn.net/Articles/534758/ (2013-Jan-30, Andi Kleen, "a software engineer working on Linux in the Intel Open Source Technology Center")
 * https://gcc.gnu.org/onlinedocs/gcc/x86-specific-memory-model-extensions-for-transactional-memory.html
 * https://gcc.gnu.org/onlinedocs/gcc-4.8.1/gcc/X86-transactional-memory-intrinsics.html
 * https://software.intel.com/en-us/articles/tsx-anti-patterns-in-lock-elision-code
 */

extern int _MQ_RWLock_MutEx_Lock_contended(MQ_RWLock_t *l, MQ_Time_t *wait_nsecs);
__wur static __always_inline int64_t _MQ_RWLock_MutEx_Lock(MQ_RWLock_t *l, MQ_Time_t *wait_nsecs) {
#ifdef MQ_RWLOCK_STATISTICS_SUPPORT
  int64_t ret;
  if (l->Flags & MQ_RWLOCK_STATISTICS)
    ret = (int64_t)MQ_SyncInt_Increment(l->M_Reqs,1);
  else
    ret = 0;
#endif

#ifdef MQ_RWLOCK_RECURSION_SUPPORT
  if (l->MutEx.Owner == MQ_TSA) {
    MQ_SyncInt_Increment(l->MutEx_Recursion_Depth,1); /* atomic to stay async signal safe */
#ifdef MQ_RWLOCK_STATISTICS_SUPPORT
    return ret;
#else
    return 0;
#endif
  }
#endif

  /* whenever the owner decrements MutEx.Count and it goes to 0, or a
     locker increments it from 0 and becomes the owner, .Owner has to
     be changed with it atomically, to stay async-signal-safe and
     avoid several races with the contention-handling routines.  In
     particular, were the uncontended .Count increment to be split
     from the .Owner assignment in _Lock(), or the uncontended .Owner
     invalidation to be split from the .Count decrement in _Unlock(),
     then in the async signal scenario, an unaverted deadlock would
     occur if the signal handler tried to lock the mutex.

     note that the piecemeal assignment of MutEx.Owner in the success
     clause of _MQ_RWLock_MutEx_Lock_contended() is OK, because
     MutEx.Count never touches zero in any scenario that follows that
     code path.
  */

  union MQ_LockCore end_core = { .Count = 1, .Owner = MQ_TSA };
  union MQ_LockCore start_core = { .Count = 0, .Owner = l->MutEx.Owner };
  if (! MQ_SyncInt_ExchangeIfEq(l->MutEx.EnBloc,start_core.EnBloc,end_core.EnBloc)) {
    if (wait_nsecs && (*wait_nsecs <= 0)) {
#ifdef MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT
      MQ_returnerrno((l->MutEx.Owner == MQ_TSA) ? EDEADLK : EBUSY);
#else
      MQ_returnerrno(EBUSY);
#endif
    }
    int ret2 = _MQ_RWLock_MutEx_Lock_contended(l,wait_nsecs);
    if (ret2) {
#ifdef MQ_RWLOCK_STATISTICS_SUPPORT
      if (l->Flags & MQ_RWLOCK_STATISTICS)
	MQ_SyncInt_Increment(l->M_Fails,1);
#endif
      return ret2;
    }
  }

#ifdef MQ_RWLOCK_STATISTICS_SUPPORT
  return ret;
#else
  return 0;
#endif
}

extern int _MQ_RWLock_MutEx_Unlock_contended(MQ_RWLock_t *l);
static __always_inline int _MQ_RWLock_MutEx_Unlock(MQ_RWLock_t *l) {
#ifdef MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT
  if (! l->ID)
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
#endif

#ifndef MQ_RWLOCK_NOCOSTLYERRORCHECKING_SUPPORT
  if (l->MutEx.Owner != MQ_TSA) {
    if (MQ_TSA_ValidP(l->MutEx.Owner))
      MQ_returnerrno_or_abort(EPERM,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
    else
      MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
  }
#endif

#ifdef MQ_RWLOCK_RECURSION_SUPPORT
  if (l->MutEx_Recursion_Depth > 0) {
    MQ_SyncInt_Decrement(l->MutEx_Recursion_Depth,1); /* atomic to stay async signal safe */
    return 0;
  }
#endif

  /* tricky math to invalidate the owner and decrement the count with
   * a single atomic decrement -- the two's complement result, when
   * cast to uint64_t and subtracted, causes a carry effect as if
   * .Count was decremented by one, as needed.
   */
  if ((MQ_SyncInt_Decrement(l->MutEx.EnBloc, (((uint64_t)(MQ_TSA - MQ_TSA_MkInvalid(MQ_TSA))))) & MQ_RWLOCK_COUNT_MASK) != 0)
    return _MQ_RWLock_MutEx_Unlock_contended(l);
  else
    return 0;
}

#ifdef __RTM__
extern __wur int _MQ_MutEx_Lock_RTM(MQ_RWLock_t *l);
#endif
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
__wur _MQ_RWLOCK_MAYBE_INLINE int64_t _MQ_Mutex_Lock(MQ_RWLock_t *l, MQ_Time_t wait_nsecs, const char *file, const int line, const char *func)
#else
__wur _MQ_RWLOCK_MAYBE_INLINE int64_t _MQ_Mutex_Lock(MQ_RWLock_t *l, MQ_Time_t wait_nsecs)
#endif
{
#ifdef MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT
  if (((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(l->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno_or_abort((l->Flags & MQ_RWLOCK_INITED) ? ENOTRECOVERABLE : EINVAL, l->Flags & MQ_RWLOCK_ABORTONMISUSE);
#endif

#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
  if (__MQ_RWLock_ThreadState.HeldLocks_Index == MQ_RWLOCK_MAX_TRACKED) {
#endif
    if (MQ_TSA == MQ_TSA_INITIALIZER) {
      int ret;
      if ((ret=MQ_ThreadState_Init()))
	return ret;
    }
#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
    else
      MQ_returnerrno_or_abort(ENOLCK,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
  }
#endif

#ifdef MQ_RWLOCK_INTELRTM_SUPPORT
  if ((! l->MutEx.Count) && (l->Flags & MQ_RWLOCK_INTELRTM) && (_MQ_MutEx_Lock_RTM(l) == 0))
    return 0; /* can't do _DEBUGGING as it would induce contention */
#endif

#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
  __MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index++].ID_with_status = (uint64_t)l|MQ_RWLOCK_TRACK_MUTEX;
#endif

  int64_t ret;

#ifdef MQ_RWLOCK_INHERITPEAKPRIORITY_SUPPORT
  if ((l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY)
#ifdef MQ_RWLOCK_RECURSION_SUPPORT
      && (l->MutEx.Owner != MQ_TSA)
#endif
      ) {
    if ((ret=_MQ_SchedState_Inherit(l))<0) {
      --__MQ_RWLock_ThreadState.HeldLocks_Index;
      return ret;
    }
  }
#endif

  ret = _MQ_RWLock_MutEx_Lock(l, &wait_nsecs);

#ifdef MQ_RWLOCK_INHERITPEAKPRIORITY_SUPPORT
  if ((ret<0) && (l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY))
    (void)_MQ_SchedState_Disinherit(l);
#endif

#ifdef MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT
  if (ret<0) {
#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
    --__MQ_RWLock_ThreadState.HeldLocks_Index;
#endif
    return ret;
  }
  if (MQ_RWLOCK_RW_LOCKED_P(l)) {
#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
    --__MQ_RWLock_ThreadState.HeldLocks_Index;
#endif
    if ((ret = _MQ_RWLock_MutEx_Unlock(l)) < 0)
      return ret;
    else
      MQ_returnerrno(EBUSY);
  }
#endif

#if (! defined(MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT)) && ((! defined(MQ_RWLOCK_NOLOCKTRACKING_SUPPORT)) || defined(MQ_RWLOCK_DEBUGGING_SUPPORT))
  if 
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
     (ret<0)
#else
       (ret)
#endif
  {
#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
    --__MQ_RWLock_ThreadState.HeldLocks_Index;
#endif
    return ret;
  }
#endif
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
  if (l->Flags & MQ_RWLOCK_DEBUGGING) {
    l->last_M_Locker_file = file;
    l->last_M_Locker_line = line;
    l->last_M_Locker_function = func;
    l->last_M_Locker_tid = gettid();
  }
#endif
  return ret;
}

#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
_MQ_RWLOCK_MAYBE_INLINE int _MQ_Mutex_Unlock(MQ_RWLock_t *l, const char *file, const int line, const char *func)
#else
_MQ_RWLOCK_MAYBE_INLINE int _MQ_Mutex_Unlock(MQ_RWLock_t *l)
#endif
{
#ifdef MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT
  if (((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(l->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno_or_abort((l->Flags & MQ_RWLOCK_INITED) ? ENOTRECOVERABLE : EINVAL, l->Flags & MQ_RWLOCK_ABORTONMISUSE);
#endif

#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
  int lastlock_index = __MQ_RWLock_ThreadState.HeldLocks_Index-1;
#endif

#ifdef MQ_RWLOCK_INTELRTM_SUPPORT
  if (__MQ_RWLock_ThreadState.HeldLocks_Index &&
      MQ_RWLOCK_TRACK_RTM_P(__MQ_RWLock_ThreadState.HeldLocks[lastlock_index])) {
    if (__MQ_RWLock_ThreadState.HeldLocks[--__MQ_RWLock_ThreadState.HeldLocks_Index].ID_with_status == (l->ID|(MQ_RWLOCK_TRACK_RTM|MQ_RWLOCK_TRACK_MUTEX))) {
      if ((! __MQ_RWLock_ThreadState.HeldLocks_Index) || (! (__MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index-1].ID_with_status & MQ_RWLOCK_TRACK_RTM)))
	_xend();
      return 0;
    } else
      _xabort(1);
  }
#endif

#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
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

  /* removing the lock from the HeldLocks set before releasing the
   * lock means this thread may miss priority update(s), but the
   * complexity and performance penalties of doing otherwise are just
   * silly.  In the words of Bob Scheifler and Jim Gettys, "If you can
   * get 90 percent of the desired effect for 10 percent of the work,
   * use the simpler solution." (hmm, maybe this PI stuff isn't worth
   * the trouble... ;^)
   */
  if (lock_index == lastlock_index) {
    for (; (lock_index > 0) && (! __MQ_RWLock_ThreadState.HeldLocks[lock_index-1].ID_with_status); --lock_index);
    __MQ_RWLock_ThreadState.HeldLocks_Index = lock_index;
  } else
    __MQ_RWLock_ThreadState.HeldLocks[lock_index].ID_with_status = 0;

#endif

  int ret = _MQ_RWLock_MutEx_Unlock(l);

#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
  if (l->Flags & MQ_RWLOCK_DEBUGGING) {
    if (! ret) {
/*    save_debug_info:*/
      l->last_M_Unlocker_file = file;
      l->last_M_Unlocker_line = line;
      l->last_M_Unlocker_function = func;
      l->last_M_Unlocker_tid = gettid();
    }
  }
#endif

#ifdef MQ_RWLOCK_INHERITPEAKPRIORITY_SUPPORT
  if ((l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY)
#ifdef MQ_RWLOCK_RECURSION_SUPPORT
      && (l->MutEx.Owner != MQ_TSA)
#endif
      ) {
    int ret2;
    if ((ret2=_MQ_SchedState_Disinherit(l))<0)
      return ret2;
  }
#endif

  return ret;
}

/* must call MQ_ThreadState_Init() before using this */
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
__wur _MQ_RWLOCK_MAYBE_INLINE int64_t _MQ_Mutex_Lock_Fast(MQ_RWLock_t *l, MQ_Time_t wait_nsecs, const char *file, const int line, const char *func)
#else
__wur _MQ_RWLOCK_MAYBE_INLINE int64_t _MQ_Mutex_Lock_Fast(MQ_RWLock_t *l, MQ_Time_t wait_nsecs)
#endif
{
#ifdef MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT
  if (((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(l->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno_or_abort((l->Flags & MQ_RWLOCK_INITED) ? ENOTRECOVERABLE : EINVAL, l->Flags & MQ_RWLOCK_ABORTONMISUSE);
#endif

#ifdef MQ_RWLOCK_INTELRTM_SUPPORT
  if ((! l->MutEx.Count) && (l->Flags & MQ_RWLOCK_INTELRTM) && (__MQ_RWLock_ThreadState.HeldLocks_Index != MQ_RWLOCK_MAX_TRACKED) && (_MQ_MutEx_Lock_RTM(l) == 0))
    return 0; /* can't do _DEBUGGING as it would induce contention */
#endif

  int64_t ret = _MQ_RWLock_MutEx_Lock(l, &wait_nsecs);

#ifdef MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT
  if (ret<0)
    return ret;
  if (MQ_RWLOCK_RW_LOCKED_P(l)) {
    if ((ret = _MQ_RWLock_MutEx_Unlock(l)) < 0)
      return ret;
    else
      MQ_returnerrno(EBUSY);
  }
#endif

#if (! defined(MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT)) && defined(MQ_RWLOCK_DEBUGGING_SUPPORT)
  if 
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
     (ret<0)
#else
       (ret)
#endif
  {
    return ret;
  }
#endif
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
  if (l->Flags & MQ_RWLOCK_DEBUGGING) {
    l->last_M_Locker_file = file;
    l->last_M_Locker_line = line;
    l->last_M_Locker_function = func;
    l->last_M_Locker_tid = gettid();
  }
#endif
  return ret;
}

#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
_MQ_RWLOCK_MAYBE_INLINE int _MQ_Mutex_Unlock_Fast(MQ_RWLock_t *l, const char *file, const int line, const char *func)
#else
_MQ_RWLOCK_MAYBE_INLINE int _MQ_Mutex_Unlock_Fast(MQ_RWLock_t *l)
#endif
{
#ifdef MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT
  if (((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(l->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno_or_abort((l->Flags & MQ_RWLOCK_INITED) ? ENOTRECOVERABLE : EINVAL, l->Flags & MQ_RWLOCK_ABORTONMISUSE);
#endif

#ifdef MQ_RWLOCK_INTELRTM_SUPPORT
  if (__MQ_RWLock_ThreadState.HeldLocks_Index &&
      MQ_RWLOCK_TRACK_RTM_P(__MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index-1])) {
    if (__MQ_RWLock_ThreadState.HeldLocks[--__MQ_RWLock_ThreadState.HeldLocks_Index].ID_with_status == (l->ID|(MQ_RWLOCK_TRACK_RTM|MQ_RWLOCK_TRACK_MUTEX))) {
      if ((! __MQ_RWLock_ThreadState.HeldLocks_Index) || (! (__MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index-1].ID_with_status & MQ_RWLOCK_TRACK_RTM)))
	_xend();
      return 0;
    } else
      _xabort(1);
  }
#endif

#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
  if (l->Flags & MQ_RWLOCK_DEBUGGING) {
    int ret = _MQ_RWLock_MutEx_Unlock(l);
    if (! ret) {
/*    save_debug_info:*/
      l->last_M_Unlocker_file = file;
      l->last_M_Unlocker_line = line;
      l->last_M_Unlocker_function = func;
      l->last_M_Unlocker_tid = gettid();
    }
    return ret;
  } else
#endif

  return _MQ_RWLock_MutEx_Unlock(l);
}

/* the MQ RW lock and cond-signal facilities, layered on the foregoing mutex facility */

#define MQ_RWLOCK_W_UP(l) (++(l)->RW.Count)
/* the rest of the RW.Count incr/decr macros are always unsafe outside the locked context of the contention handlers, so appear there, not here. */

#ifdef __RTM__
extern __wur int _MQ_W_Lock_RTM(MQ_RWLock_t *l);
#endif
extern __wur int _MQ_W_Lock_contended(MQ_RWLock_t *l, MQ_Time_t wait_nsecs, const char *file, const int line, const char *func);
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
__wur _MQ_RWLOCK_MAYBE_INLINE int _MQ_W_Lock(MQ_RWLock_t *l, MQ_Time_t wait_nsecs, const char *file, const int line, const char *func)
#else
__wur _MQ_RWLOCK_MAYBE_INLINE int _MQ_W_Lock(MQ_RWLock_t *l, MQ_Time_t wait_nsecs)
#endif
{
#ifdef MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT
  if (((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(l->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno_or_abort((l->Flags & MQ_RWLOCK_INITED) ? ENOTRECOVERABLE : EINVAL, l->Flags & MQ_RWLOCK_ABORTONMISUSE);
#endif

#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
  if (__MQ_RWLock_ThreadState.HeldLocks_Index == MQ_RWLOCK_MAX_TRACKED) {
#endif
    if (MQ_TSA == MQ_TSA_INITIALIZER) {
      int ret;
      if ((ret=MQ_ThreadState_Init()))
	return ret;
    }
#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
    else
      MQ_returnerrno_or_abort(ENOLCK,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
  }
#endif

#ifdef MQ_RWLOCK_INTELRTM_SUPPORT
  if ((! MQ_RWLOCK_RW_RAWCOUNT(l)) && (l->Flags & MQ_RWLOCK_INTELRTM) && (_MQ_W_Lock_RTM(l) == 0))
    return 0;
#endif

  int ret;

#ifdef MQ_RWLOCK_STATISTICS_SUPPORT
  if (l->Flags & MQ_RWLOCK_STATISTICS)
    MQ_SyncInt_Increment(l->W_Reqs,1);
#endif

#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
  __MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index++].ID_with_status = (uint64_t)l|MQ_RWLOCK_TRACK_WRITE;
#endif

#ifdef MQ_RWLOCK_INHERITPEAKPRIORITY_SUPPORT
  if ((l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY)
#ifdef MQ_RWLOCK_RECURSION_SUPPORT
      && (l->RW.Owner != MQ_TSA)
#endif
      ) {
    if ((ret=_MQ_SchedState_Inherit(l))<0) {
      --__MQ_RWLock_ThreadState.HeldLocks_Index;
      return ret;
    }
  }
#endif

#if defined(MQ_RWLOCK_RECURSION_SUPPORT) || (! defined(MQ_RWLOCK_NOCOSTLYERRORCHECKING_SUPPORT))
  if (l->RW.Owner == MQ_TSA) {
#ifdef MQ_RWLOCK_RECURSION_SUPPORT
    if (l->Flags & MQ_RWLOCK_RECURSION) {
      MQ_RWLOCK_W_UP(l);
      goto successout;
    } else
#endif
    {
      ret = MQ_seterrpoint_or_abort(EDEADLK,l->Flags & MQ_RWLOCK_ABORTONEDEADLK);
      goto failout;
    }
  }
#endif

  {
    union MQ_LockCore start_core = { .EnBloc = l->RW.EnBloc };
    if (! start_core.Count) {
      union MQ_LockCore end_core = { .Count = 1, .Owner = MQ_TSA };
      if (MQ_SyncInt_ExchangeIfEq_MaybeLock(l->RW.EnBloc,start_core.EnBloc,end_core.EnBloc))
	goto successout;
    }
  }

#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
  ret = _MQ_W_Lock_contended(l, wait_nsecs, file, line, func);
#else
  ret = _MQ_W_Lock_contended(l, wait_nsecs, 0, 0, 0);
#endif

  if (ret<0) {
#if defined(MQ_RWLOCK_RECURSION_SUPPORT) || (! defined(MQ_RWLOCK_NOCOSTLYERRORCHECKING_SUPPORT))
  failout:
#endif
#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
    --__MQ_RWLock_ThreadState.HeldLocks_Index;
#endif
#ifdef MQ_RWLOCK_INHERITPEAKPRIORITY_SUPPORT
  if (l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY)
    (void)_MQ_SchedState_Disinherit(l);
#endif

#ifdef MQ_RWLOCK_STATISTICS_SUPPORT
    if (l->Flags & MQ_RWLOCK_STATISTICS)
      MQ_SyncInt_Increment(l->W_Fails,1);
#endif
    return ret;
  }

 successout:

#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
  if (l->Flags & MQ_RWLOCK_DEBUGGING) {
    l->last_W_Locker_file = file;
    l->last_W_Locker_line = line;
    l->last_W_Locker_function = func;
    l->last_W_Locker_tid = gettid();
  }
#endif

  return 0;
}

extern __wur int _MQ_W2R_Lock_contended(MQ_RWLock_t *l);
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
__wur _MQ_RWLOCK_MAYBE_INLINE int _MQ_W2R_Lock(MQ_RWLock_t *l, const char *file, const int line, const char *func)
#else
__wur _MQ_RWLOCK_MAYBE_INLINE int _MQ_W2R_Lock(MQ_RWLock_t *l)
#endif
{
#ifdef MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT
  if (((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(l->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno_or_abort((l->Flags & MQ_RWLOCK_INITED) ? ENOTRECOVERABLE : EINVAL, l->Flags & MQ_RWLOCK_ABORTONMISUSE);
  if (! l->ID)
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
  if (MQ_TSA == MQ_TSA_INITIALIZER)
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
#endif

#ifdef MQ_RWLOCK_INTELRTM_SUPPORT
  for (int lock_index=__MQ_RWLock_ThreadState.HeldLocks_Index-1;lock_index>=0;--lock_index) {
    if (! MQ_RWLOCK_TRACK_RTM_P(__MQ_RWLock_ThreadState.HeldLocks[lock_index]))
      break;
    if (MQ_RWLOCK_TRACK_ID(__MQ_RWLock_ThreadState.HeldLocks[lock_index]) != (uint64_t)l)
      continue;
    if (MQ_RWLOCK_TRACK_TYPE(__MQ_RWLock_ThreadState.HeldLocks[lock_index]) == MQ_RWLOCK_TRACK_WRITE) {
      __MQ_RWLock_ThreadState.HeldLocks[lock_index].ID_with_status = (__MQ_RWLock_ThreadState.HeldLocks[lock_index].ID_with_status ^ MQ_RWLOCK_TRACK_WRITE) | MQ_RWLOCK_TRACK_READ;
      return 0;
    } else
      break; /* _xabort()? */
  }
#endif

#ifndef MQ_RWLOCK_NOCOSTLYERRORCHECKING_SUPPORT
  if (l->RW.Owner != MQ_TSA)
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
#endif

#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
  int lock_index;
  for (lock_index=__MQ_RWLock_ThreadState.HeldLocks_Index-1;lock_index>=0;--lock_index) {
    if (MQ_RWLOCK_TRACK_ID(__MQ_RWLock_ThreadState.HeldLocks[lock_index]) != (uint64_t)l)
      continue;
    if (MQ_RWLOCK_TRACK_TYPE(__MQ_RWLock_ThreadState.HeldLocks[lock_index]) == MQ_RWLOCK_TRACK_WRITE)
      break;
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
  }
  if (lock_index<0)
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
#endif

  {
    union MQ_LockCore start_core = { .EnBloc = l->RW.EnBloc };
    if (start_core.Count == 1) {
      union MQ_LockCore end_core = { .Count = (MQ_RWLOCK_COUNT_READ)|1U, .Owner = MQ_TSA_MkInvalid(MQ_TSA) };
      if (MQ_SyncInt_ExchangeIfEq_MaybeLock(l->RW.EnBloc,start_core.EnBloc,end_core.EnBloc))
	goto successout;
    }
  }

  {
    int ret = _MQ_W2R_Lock_contended(l);
    if (ret)
      return ret;
  }

 successout:

#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
  __MQ_RWLock_ThreadState.HeldLocks[lock_index].ID_with_status = (__MQ_RWLock_ThreadState.HeldLocks[lock_index].ID_with_status ^ MQ_RWLOCK_TRACK_WRITE) | MQ_RWLOCK_TRACK_READ;
#endif

#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
  if (l->Flags & MQ_RWLOCK_DEBUGGING) {
    /* note these assignments are races */
    l->last_W_Unlocker_file = file;
    l->last_W_Unlocker_line = line;
    l->last_W_Unlocker_function = func;
    l->last_W_Unlocker_tid = gettid();
    l->last_R_Locker_file = file;
    l->last_R_Locker_line = line;
    l->last_R_Locker_function = func;
    l->last_R_Locker_tid = gettid();
    l->last_fresh_R_Locker_file = file;
    l->last_fresh_R_Locker_line = line;
    l->last_fresh_R_Locker_function = func;
    l->last_fresh_R_Locker_tid = gettid();
  }
#endif

  return 0;
}

extern __wur int _MQ_R2W_Lock_contended(MQ_RWLock_t *l, MQ_Time_t wait_nsecs, const char *file, const int line, const char *func);
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
__wur _MQ_RWLOCK_MAYBE_INLINE int _MQ_R2W_Lock(MQ_RWLock_t *l, MQ_Time_t wait_nsecs, const char *file, const int line, const char *func)
#else
__wur _MQ_RWLOCK_MAYBE_INLINE int _MQ_R2W_Lock(MQ_RWLock_t *l, MQ_Time_t wait_nsecs)
#endif
{
#ifdef MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT
  if (((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(l->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno_or_abort((l->Flags & MQ_RWLOCK_INITED) ? ENOTRECOVERABLE : EINVAL, l->Flags & MQ_RWLOCK_ABORTONMISUSE);
  if (! l->ID)
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
  if (MQ_TSA == MQ_TSA_INITIALIZER)
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
#endif

#ifdef MQ_RWLOCK_INTELRTM_SUPPORT
  for (int lock_index=__MQ_RWLock_ThreadState.HeldLocks_Index-1;lock_index>=0;--lock_index) {
    if (! MQ_RWLOCK_TRACK_RTM_P(__MQ_RWLock_ThreadState.HeldLocks[lock_index]))
      break;
    if (MQ_RWLOCK_TRACK_ID(__MQ_RWLock_ThreadState.HeldLocks[lock_index]) != (uint64_t)l)
      continue;
    if (MQ_RWLOCK_TRACK_TYPE(__MQ_RWLock_ThreadState.HeldLocks[lock_index]) == MQ_RWLOCK_TRACK_READ) {
      __MQ_RWLock_ThreadState.HeldLocks[lock_index].ID_with_status = (__MQ_RWLock_ThreadState.HeldLocks[lock_index].ID_with_status ^ MQ_RWLOCK_TRACK_READ) | MQ_RWLOCK_TRACK_WRITE;
      return 0;
    } else
      break; /* _xabort()? */
  }
#endif

  int ret;

#ifdef MQ_RWLOCK_STATISTICS_SUPPORT
  if (l->Flags & MQ_RWLOCK_STATISTICS)
    MQ_SyncInt_Increment(l->R2W_Reqs,1);
#endif

#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
  int lock_index = -1, scratch_lock_index;
  for (scratch_lock_index=__MQ_RWLock_ThreadState.HeldLocks_Index-1;scratch_lock_index>=0;--scratch_lock_index) {
    if (MQ_RWLOCK_TRACK_ID(__MQ_RWLock_ThreadState.HeldLocks[scratch_lock_index]) != (uint64_t)l)
      continue;
    if (MQ_RWLOCK_TRACK_TYPE(__MQ_RWLock_ThreadState.HeldLocks[scratch_lock_index]) == MQ_RWLOCK_TRACK_READ) {
      if (lock_index != -1)
	MQ_returnerrno_or_abort(EDEADLK,l->Flags & MQ_RWLOCK_ABORTONEDEADLK);
      lock_index = scratch_lock_index;
    } else if (MQ_RWLOCK_TRACK_TYPE(__MQ_RWLock_ThreadState.HeldLocks[scratch_lock_index]) == MQ_RWLOCK_TRACK_WRITE)
      MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
  }
  if (lock_index<0)
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
#endif

  {
    union MQ_LockCore start_core = { .EnBloc = l->RW.EnBloc };
    if (start_core.Count == (MQ_RWLOCK_COUNT_READ|1U)) {
      union MQ_LockCore end_core = { .Count = 1, .Owner = MQ_TSA };
      if (MQ_SyncInt_ExchangeIfEq_MaybeLock(l->RW.EnBloc,start_core.EnBloc,end_core.EnBloc)) {
	ret = 0;
	goto out;
      }
    }
  }

#ifdef MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT
  if (MQ_RWLOCK_R_NOTLOCKED_P(l))
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
#endif

#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
  ret = _MQ_R2W_Lock_contended(l,wait_nsecs,file,line,func);
#else
  ret = _MQ_R2W_Lock_contended(l,wait_nsecs,0,0,0);
#endif

 out:

  if (ret == 0) {
#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
    __MQ_RWLock_ThreadState.HeldLocks[lock_index].ID_with_status = (__MQ_RWLock_ThreadState.HeldLocks[lock_index].ID_with_status ^ MQ_RWLOCK_TRACK_READ) | MQ_RWLOCK_TRACK_WRITE;
#endif

#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
    if (l->Flags & MQ_RWLOCK_DEBUGGING) {
      l->last_W_Locker_file = file;
      l->last_W_Locker_line = line;
      l->last_W_Locker_function = func;
      l->last_W_Locker_tid = gettid();
    }
#endif
  }
#ifdef MQ_RWLOCK_STATISTICS_SUPPORT
  else {
    if (l->Flags & MQ_RWLOCK_STATISTICS)
      MQ_SyncInt_Increment(l->R2W_Fails,1);
  }
#endif

  return ret;
}

extern int _MQ_W_Unlock_contended(MQ_RWLock_t *l);
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
_MQ_RWLOCK_MAYBE_INLINE int _MQ_W_Unlock(MQ_RWLock_t *l, const char *file, const int line, const char *func)
#else
_MQ_RWLOCK_MAYBE_INLINE int _MQ_W_Unlock(MQ_RWLock_t *l)
#endif
{
#ifdef MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT
  if (((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(l->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno_or_abort((l->Flags & MQ_RWLOCK_INITED) ? ENOTRECOVERABLE : EINVAL, l->Flags & MQ_RWLOCK_ABORTONMISUSE);
  if (! l->ID)
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
  if (MQ_TSA == MQ_TSA_INITIALIZER)
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
#endif

#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
  int lastlock_index = __MQ_RWLock_ThreadState.HeldLocks_Index-1;
#endif

#ifdef MQ_RWLOCK_INTELRTM_SUPPORT
  if (__MQ_RWLock_ThreadState.HeldLocks_Index &&
      MQ_RWLOCK_TRACK_RTM_P(__MQ_RWLock_ThreadState.HeldLocks[lastlock_index])) {
    if (__MQ_RWLock_ThreadState.HeldLocks[--__MQ_RWLock_ThreadState.HeldLocks_Index].ID_with_status == ((uint64_t)l|(MQ_RWLOCK_TRACK_RTM|MQ_RWLOCK_TRACK_WRITE))) {
      if ((! __MQ_RWLock_ThreadState.HeldLocks_Index) || (! (__MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index-1].ID_with_status & MQ_RWLOCK_TRACK_RTM)))
	_xend();
      return 0;
    } else
      _xabort(1);
  }
#endif

#ifndef MQ_RWLOCK_NOCOSTLYERRORCHECKING_SUPPORT
  if (l->RW.Owner != MQ_TSA)
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
#endif

#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
  int lock_index;
  for (lock_index=lastlock_index;lock_index>=0;--lock_index) {
    if (MQ_RWLOCK_TRACK_ID(__MQ_RWLock_ThreadState.HeldLocks[lock_index]) != (uint64_t)l)
      continue;
    if (MQ_RWLOCK_TRACK_TYPE(__MQ_RWLock_ThreadState.HeldLocks[lock_index]) == MQ_RWLOCK_TRACK_WRITE)
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
#endif

  {
    union MQ_LockCore start_core = { .EnBloc = l->RW.EnBloc };
    if (! (start_core.Count & MQ_RWLOCK_COUNT_CONTENDED)) {
#ifdef MQ_RWLOCK_RECURSION_SUPPORT
      union MQ_LockCore end_core = { .Count = start_core.Count - 1, .Owner = (start_core.Count == 1) ? MQ_TSA_MkInvalid(MQ_TSA) : MQ_TSA };
#else
      union MQ_LockCore end_core = { .Count = 0, .Owner = MQ_TSA_MkInvalid(MQ_TSA) };
#endif
      if (MQ_SyncInt_ExchangeIfEq_MaybeUnlock(l->RW.EnBloc,start_core.EnBloc,end_core.EnBloc))
	goto successout;
    }
  }

  int ret;

  ret = _MQ_W_Unlock_contended(l);
  if (ret<0)
    return ret;

 successout:

#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
  if (l->Flags & MQ_RWLOCK_DEBUGGING) {
    /* note these assignments are races */
    l->last_W_Unlocker_file = file;
    l->last_W_Unlocker_line = line;
    l->last_W_Unlocker_function = func;
    l->last_W_Unlocker_tid = gettid();
  }
#endif

#ifdef MQ_RWLOCK_INHERITPEAKPRIORITY_SUPPORT
  if ((l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY)
#ifdef MQ_RWLOCK_RECURSION_SUPPORT
      && (l->RW.Owner != MQ_TSA)
#endif
      ) {
    int ret2;
    if ((ret2=_MQ_SchedState_Disinherit(l))<0)
      return ret2;
  }
#endif

  return 0;
}

#ifdef __RTM__
extern __wur int _MQ_R_Lock_RTM(MQ_RWLock_t *l);
#endif
extern __wur int _MQ_R_Lock_contended(MQ_RWLock_t *l, MQ_Time_t wait_nsecs, const char *file, const int line, const char *func);
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
__wur _MQ_RWLOCK_MAYBE_INLINE int _MQ_R_Lock(MQ_RWLock_t *l, MQ_Time_t wait_nsecs, const char *file, const int line, const char *func)
#else
__wur _MQ_RWLOCK_MAYBE_INLINE int _MQ_R_Lock(MQ_RWLock_t *l, MQ_Time_t wait_nsecs)
#endif
{
#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
  if (__MQ_RWLock_ThreadState.HeldLocks_Index == MQ_RWLOCK_MAX_TRACKED) {
#endif
    if (MQ_TSA == MQ_TSA_INITIALIZER) {
      int ret;
      if ((ret=MQ_ThreadState_Init()))
	return ret;
    }
#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
    else
      MQ_returnerrno_or_abort(ENOLCK,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
  }
#endif

#ifdef MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT
  if (((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(l->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno_or_abort((l->Flags & MQ_RWLOCK_INITED) ? ENOTRECOVERABLE : EINVAL, l->Flags & MQ_RWLOCK_ABORTONMISUSE);
#endif

#ifdef MQ_RWLOCK_INTELRTM_SUPPORT
  if ((! MQ_RWLOCK_RW_RAWCOUNT(l)) && (l->Flags & MQ_RWLOCK_INTELRTM) && (_MQ_R_Lock_RTM(l) == 0))
    return 0;
#endif

#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
  volatile int ret;
#endif

#ifdef MQ_RWLOCK_STATISTICS_SUPPORT
  if (l->Flags & MQ_RWLOCK_STATISTICS)
    MQ_SyncInt_Increment(l->R_Reqs,1);
#endif

#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
#ifdef MQ_RWLOCK_READOPTIMAL_SUPPORT
  __MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index++].ID_with_status = (uint64_t)l|MQ_RWLOCK_TRACK_READ|MQ_RWLOCK_TRACK_READOPTIMAL_UNCOUNTED;
#else
  __MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index++].ID_with_status = (uint64_t)l|MQ_RWLOCK_TRACK_READ;
#endif
#endif

#ifdef MQ_RWLOCK_INHERITPEAKPRIORITY_SUPPORT
  if (l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY) {
#ifndef MQ_RWLOCK_DEBUGGING_SUPPORT
    int ret;
#endif
    if ((ret=_MQ_SchedState_Inherit(l))<0) {
      --__MQ_RWLock_ThreadState.HeldLocks_Index;
      return ret;
    }
  }
#endif

  {
    uint32_t curcount = MQ_RWLOCK_RW_RAWCOUNT(l);
#ifdef MQ_RWLOCK_READOPTIMAL_SUPPORT
#error MQ_RWLOCK_READOPTIMAL_SUPPORT is work in progress
    if (l->Flags & MQ_RWLOCK_READOPTIMAL) {
      if (! curcount) {
	/* only need to do this initially and after passing through a write-locked phase */
	if (MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(MQ_RWLOCK_RW_RAWCOUNT(l),curcount,MQ_RWLOCK_COUNT_READ) ||
	    (curcount == MQ_RWLOCK_COUNT_READ))
	  goto successout;
      } else if (curcount == MQ_RWLOCK_COUNT_READ) {
	/* this is the routine uncontended read lock path */
	MQ_SyncInt_Fence_Full(); /* fencing guarantees the update to _HeldLocks registers before making the final determination that we are uncontended */
	if (MQ_RWLOCK_RW_RAWCOUNT(l) == MQ_RWLOCK_COUNT_READ)
	  goto successout;
      }
      /* atomically clear the _UNCOUNTED bit, but if it was already counted by a writer, we also have to decrement RW.Count, since we don't actually have the lock */
      /* obviously the write locker has to do atomic operations of its own to test the _READOPTIMAL_UNCOUNTED bit and set the _DEFERRED_COUNTED bit.
       * there is a race between the write locker incrementing RW.Count and this path decrementing it, but it doesn't matter because the write locker
       * owns the mutex.
       */
      uint64_t track_ent = MQ_SyncInt_Exchange(__MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index-1],(uint64_t)l|MQ_RWLOCK_TRACK_READ);
      if (track_ent & MQ_RWLOCK_TRACK_DEFERRED_COUNTED)
	MQ_SyncInt_Decrement(MQ_RWLOCK_RW_RAWCOUNT(l),1);
    }
#endif
    if (((! curcount) || ((curcount & (MQ_RWLOCK_COUNT_READ|MQ_RWLOCK_COUNT_CONTENDED)) == MQ_RWLOCK_COUNT_READ))) {
      for (;;) {
	if (MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(MQ_RWLOCK_RW_RAWCOUNT(l),curcount,(curcount|MQ_RWLOCK_COUNT_READ)+1UL)) {
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
	  if (l->Flags & MQ_RWLOCK_DEBUGGING)
	    ret = (int)((curcount & (MQ_RWLOCK_COUNT_MINFLAG-1))+1);
#endif
	  goto successout;
	}
	if (curcount && ((curcount & (MQ_RWLOCK_COUNT_READ|MQ_RWLOCK_COUNT_CONTENDED)) != MQ_RWLOCK_COUNT_READ))
	  break;
      }
    }
  }

  {
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
    ret = _MQ_R_Lock_contended(l,wait_nsecs,file,line,func);
#else
    int ret = _MQ_R_Lock_contended(l,wait_nsecs,0,0,0);
#endif
    if (ret<0) {
#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
      --__MQ_RWLock_ThreadState.HeldLocks_Index;
#endif
#ifdef MQ_RWLOCK_INHERITPEAKPRIORITY_SUPPORT
  if (l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY)
    (void)_MQ_SchedState_Disinherit(l);
#endif
#ifdef MQ_RWLOCK_STATISTICS_SUPPORT
      if (l->Flags & MQ_RWLOCK_STATISTICS)
	MQ_SyncInt_Increment(l->R_Fails,1);
#endif
      return ret;
    }
  }

 successout:

#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
  if (l->Flags & MQ_RWLOCK_DEBUGGING) {
    /* note these assignments are races */
    l->last_R_Locker_file = file;
    l->last_R_Locker_line = line;
    l->last_R_Locker_function = func;
    l->last_R_Locker_tid = gettid();
    if (ret == 1) {
      /* these assignments are not races unless we had to _MQ_RWLock_Wait() */
      l->last_fresh_R_Locker_file = file;
      l->last_fresh_R_Locker_line = line;
      l->last_fresh_R_Locker_function = func;
      l->last_fresh_R_Locker_tid = gettid();
    }
  }
#endif

  return 0;
}

extern int _MQ_R_Unlock_contended(MQ_RWLock_t *l);
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
_MQ_RWLOCK_MAYBE_INLINE int _MQ_R_Unlock(MQ_RWLock_t *l, const char *file, const int line, const char *func)
#else
_MQ_RWLOCK_MAYBE_INLINE int _MQ_R_Unlock(MQ_RWLock_t *l)
#endif
{
#ifdef MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT
  if (((l->Flags & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED) &&
      ((MQ_SyncInt_Get(l->Flags) & (MQ_RWLOCK_INITED|MQ_RWLOCK_LFLIST_CORRUPTED|MQ_RWLOCK_WAITLIST_CORRUPTED)) != MQ_RWLOCK_INITED))
    MQ_returnerrno_or_abort((l->Flags & MQ_RWLOCK_INITED) ? ENOTRECOVERABLE : EINVAL, l->Flags & MQ_RWLOCK_ABORTONMISUSE);
  if (! l->ID)
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
  if (MQ_TSA == MQ_TSA_INITIALIZER)
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
#endif

#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
  int lastlock_index = __MQ_RWLock_ThreadState.HeldLocks_Index-1;
#endif

#ifdef MQ_RWLOCK_INTELRTM_SUPPORT
  if (__MQ_RWLock_ThreadState.HeldLocks_Index &&
      MQ_RWLOCK_TRACK_RTM_P(__MQ_RWLock_ThreadState.HeldLocks[lastlock_index])) {
    if (__MQ_RWLock_ThreadState.HeldLocks[--__MQ_RWLock_ThreadState.HeldLocks_Index].ID_with_status == ((uint64_t)l|(MQ_RWLOCK_TRACK_RTM|MQ_RWLOCK_TRACK_READ))) {
      if ((! __MQ_RWLock_ThreadState.HeldLocks_Index) || (! (__MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index-1].ID_with_status & MQ_RWLOCK_TRACK_RTM)))
	_xend();
      return 0;
    } else
      _xabort(1);
  }
#endif

#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
  int lock_index;
  for (lock_index=lastlock_index;lock_index>=0;--lock_index) {
    if (MQ_RWLOCK_TRACK_ID(__MQ_RWLock_ThreadState.HeldLocks[lock_index]) != (uint64_t)l)
      continue;
    if (MQ_RWLOCK_TRACK_TYPE(__MQ_RWLock_ThreadState.HeldLocks[lock_index]) == MQ_RWLOCK_TRACK_READ)
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
#endif

  {
    uint32_t curcount = MQ_RWLOCK_RW_RAWCOUNT(l);
    if ((curcount & (MQ_RWLOCK_COUNT_CONTENDED|MQ_RWLOCK_COUNT_READ)) == MQ_RWLOCK_COUNT_READ) {
      for (;;) {
	if (MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(MQ_RWLOCK_RW_RAWCOUNT(l),curcount,(curcount==(MQ_RWLOCK_COUNT_READ|1) ? 0 : curcount-1)))
	  goto successout;
	if ((curcount & (MQ_RWLOCK_COUNT_CONTENDED|MQ_RWLOCK_COUNT_READ)) != MQ_RWLOCK_COUNT_READ)
	  break;
      }
    }
  }

  {
    int ret = _MQ_R_Unlock_contended(l);
    if (ret<0)
      return ret;
  }

 successout:

#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
  if (l->Flags & MQ_RWLOCK_DEBUGGING) {
    l->last_R_Unlocker_file = file;
    l->last_R_Unlocker_line = line;
    l->last_R_Unlocker_function = func;
    l->last_R_Unlocker_tid = gettid();
  }
#endif

#ifdef MQ_RWLOCK_INHERITPEAKPRIORITY_SUPPORT
  if (l->Flags & MQ_RWLOCK_INHERITPEAKPRIORITY) {
    /* if thread has multiple R locks on this, the disinheriting will be a no-op: */
    int ret2;
    if ((ret2=_MQ_SchedState_Disinherit(l))<0)
      return ret2;
  }
#endif

  return 0;
}

#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
_MQ_RWLOCK_MAYBE_INLINE int _MQ_Unlock(MQ_RWLock_t *l, const char *file, const int line, const char *func)
#else
_MQ_RWLOCK_MAYBE_INLINE int _MQ_Unlock(MQ_RWLock_t *l)
#endif
{
#ifdef MQ_RWLOCK_INTELRTM_SUPPORT
  if (__MQ_RWLock_ThreadState.HeldLocks_Index &&
      MQ_RWLOCK_TRACK_RTM_P(__MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index-1])) {
    if ((__MQ_RWLock_ThreadState.HeldLocks[--__MQ_RWLock_ThreadState.HeldLocks_Index].ID_with_status & (MQ_RWLOCK_TRACK_RTM|MQ_RWLOCK_TRACK_IDMASK)) == ((uint64_t)l|(MQ_RWLOCK_TRACK_RTM))) {
      if ((! __MQ_RWLock_ThreadState.HeldLocks_Index) || (! (__MQ_RWLock_ThreadState.HeldLocks[__MQ_RWLock_ThreadState.HeldLocks_Index-1].ID_with_status & MQ_RWLOCK_TRACK_RTM)))
	_xend();
      return 0;
    } else
      _xabort(1);
  }
#endif

  if (l->RW.Owner == MQ_TSA) {
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
    return _MQ_W_Unlock(l,file,line,func);
#else
    return _MQ_W_Unlock(l);
#endif
  } else if ((MQ_RWLOCK_RW_RAWCOUNT(l) & (MQ_RWLOCK_COUNT_READ|(MQ_RWLOCK_COUNT_MINFLAG-1))) > MQ_RWLOCK_COUNT_READ) {
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
    return _MQ_R_Unlock(l,file,line,func);
#else
    return _MQ_R_Unlock(l);
#endif
  } else if (l->MutEx.Owner == MQ_TSA) {
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
    return _MQ_Mutex_Unlock(l,file,line,func);
#else
    return _MQ_Mutex_Unlock(l);
#endif
  } else
    MQ_returnerrno_or_abort(EINVAL,l->Flags & MQ_RWLOCK_ABORTONMISUSE);
}

#endif
